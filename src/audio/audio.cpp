// Windows 장치 열거와 WASAPI 이벤트 기반 캡처를 DSP/출력으로 연결한다.
// 실행 경로는 AudioEngine::start → run → run_stream이며, 장치별 객체는 작업 스레드 안에서
// 생성·해제한다. 미리 듣기는 SpeakerPreview, 실제 가상 마이크 전송은 DriverTransport를 쓴다.
#include "audio.hpp"
#include "dsp.hpp"
#include "audio_policy.hpp"
#include "driver_transport.hpp"
#include "preview_buffer.hpp"
#include "usbip_session.hpp"
#include <algorithm>
#include <array>
#include <audioclient.h>
#include <avrt.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <stdexcept>
#include <wrl/client.h>
namespace qm
{
namespace
{
// 앱 내부 형식은 48 kHz, 모노, 32비트 float다. nAvgBytesPerSec는 초당 바이트 수,
// nBlockAlign은 한 샘플 시점의 바이트 수(모노 float이므로 4)이다.
constexpr WAVEFORMATEX float_wave_format()
{
    return {
        WAVE_FORMAT_IEEE_FLOAT, 1, format::sample_rate, format::sample_rate * sizeof(float), sizeof(float),
        sizeof(float) * 8,      0};
}
} // namespace
// 전용 속성(native)과 캡처 방향을 먼저 확인한 뒤 Windows가 인터페이스 이름을 붙인
// 표시 이름 변형까지 허용한다. 이름만 같은 다른 마이크는 QuietMic으로 인정하지 않는다.
bool quietmic_output(const Device &d)
{
    return d.capture && d.native &&
           (d.name == "QuietMic Output" || d.name == "QuietMic Output(" + d.interface_name + ")" ||
            d.name == "QuietMic Output (" + d.interface_name + ")");
}
bool original_route_present(const std::vector<Device> &list, const std::string &input,
                            const std::string &render, const std::string &capture)
{
    // 세 ID가 유효한지와 output 매핑을 먼저 확인하고, 아래에서 물리 입력과 지정된
    // 가상 캡처가 활성 장치 목록에 실제로 존재하는지 각각 검사한다.
    if (input.empty() || render.empty() || capture.empty() || configured_output(list, render) != capture)
        return false;
    bool mic = false, output = false;
    for (const auto &d : list)
    {
        if (d.id == input && d.capture && !d.native)
            mic = true;
        if (d.id == capture && quietmic_output(d))
            output = true;
    }
    return mic && output;
}
using Microsoft::WRL::ComPtr;
// 복구 가능한 WASAPI 오류를 구분하기 위해 오류 문자열과 함께 원래 HRESULT를 보관한다.
struct AudioFailure : std::runtime_error
{
    HRESULT code;
    AudioFailure(HRESULT value, const char *message) : std::runtime_error(message), code(value)
    {
    }
};
// Windows 실패 코드를 예외로 바꾸는 공통 경계다. 호출명과 16진수 코드를 함께 남겨
// 상위 run에서 사용자 메시지와 재연결 여부를 결정할 수 있게 한다.
static void check(HRESULT h, const char *what)
{
    if (FAILED(h))
    {
        char b[160];
        sprintf_s(b, "%s (0x%08lX)", what, (unsigned long)h);
        throw AudioFailure(h, b);
    }
}
// 작업 스레드는 MTA로 초기화하지만 UI가 이미 STA로 초기화한 경우 그 방식을 유지한다.
// 이 범위의 COM 객체는 호출 스레드 안에서만 사용하므로 두 방식 모두 허용된다.
// 직접 초기화 횟수를 늘린 경우(S_OK/S_FALSE)에만 해제하여 UI 소유의 COM을 해제하지 않는다.
struct Com
{
    HRESULT h = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com()
    {
        if (h != RPC_E_CHANGED_MODE)
            check(h, "COM initialization");
    }
    ~Com()
    {
        if (SUCCEEDED(h))
            CoUninitialize();
    }
};
// 캡처/재생 알림용 자동 리셋 이벤트다. 대기 하나를 깨우면 신호가 자동으로 내려가고,
// 객체가 파괴될 때 Windows 핸들을 닫는다. 엔진의 stop_은 별도 수동 리셋 이벤트다.
struct Event
{
    HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Event()
    {
        if (!h)
            throw std::runtime_error("Event creation failed");
    }
    ~Event()
    {
        CloseHandle(h);
    }
};
// 장치 열거 COM 객체를 만든다. ComPtr가 참조를 소유해 반환/예외 경로에서도 자동 해제한다.
static ComPtr<IMMDeviceEnumerator> enumerator()
{
    ComPtr<IMMDeviceEnumerator> e;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)),
          "Audio service unavailable");
    return e;
}
// 속성 값의 실제 자료형이 문자열 또는 GUID일 때만 UTF-8 문자열로 변환한다.
// 없는 속성은 빈 문자열이며 PROPVARIANT 내부 메모리는 마지막에 항상 정리한다.
static std::string property(IPropertyStore *p, REFPROPERTYKEY k)
{
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string s;
    if (SUCCEEDED(p->GetValue(k, &v)))
    {
        if (v.vt == VT_LPWSTR && v.pwszVal)
            s = utf8(v.pwszVal);
        else if (v.vt == VT_CLSID && v.puuid)
        {
            wchar_t b[64];
            StringFromGUID2(*v.puuid, b, 64);
            s = utf8(b);
        }
    }
    PropVariantClear(&v);
    return s;
}
std::vector<Device> devices()
{
    const auto usb_container = usbip::device_container();
    Com com;
    auto e = enumerator();
    std::vector<Device> out;
    // 활성 상태의 입력과 출력 장치를 별도로 열거한다. Windows가 할당한 ID 문자열은
    // std::string으로 복사한 뒤 CoTaskMemFree로 돌려주어 소유권을 분명히 한다.
    for (auto flow : {eCapture, eRender})
    {
        ComPtr<IMMDeviceCollection> list;
        check(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &list), "Enumerate audio devices");
        UINT count = 0;
        check(list->GetCount(&count), "Device count");
        for (UINT i = 0; i < count; i++)
        {
            ComPtr<IMMDevice> d;
            check(list->Item(i, &d), "Get device");
            LPWSTR id = nullptr;
            check(d->GetId(&id), "Get endpoint ID");
            Device item;
            item.id = utf8(id);
            CoTaskMemFree(id);
            ComPtr<IPropertyStore> p;
            check(d->OpenPropertyStore(STGM_READ, &p), "Device properties");
            item.name = property(p.Get(), PKEY_Device_FriendlyName);
            item.interface_name = property(p.Get(), PKEY_DeviceInterface_FriendlyName);
            item.container = property(p.Get(), PKEY_Device_ContainerId);
            item.capture = flow == eCapture;
            // 드라이버 전용 속성 키/값을 읽어 QuietMic 구성 요소인지 확인한다.
            // 표시 이름은 바뀔 수 있으므로 이름 검색만으로 드라이버 장치를 추측하지 않는다.
            static const PROPERTYKEY identity = {
                {0xad3b03ca, 0x42a7, 0x4db4, {0xbb, 0xd5, 0x87, 0xb9, 0x57, 0x4e, 0x77, 0xee}}, 2};
            item.native = property(p.Get(), identity) == "QuietMic.Output.v1" ||
                          (!usb_container.empty() && item.container == usb_container);
            out.push_back(std::move(item));
        }
    }
    return out;
}
// 설정의 ID와 정확히 일치하는 QuietMic 캡처 엔드포인트만 반환한다.
// 찾지 못하면 빈 문자열을 반환하여 호출자가 경로를 유효하지 않은 것으로 처리하게 한다.
std::string configured_output(const std::vector<Device> &list, const std::string &id)
{
    for (const auto &d : list)
        if (d.id == id && d.capture && d.native)
            return d.id;
    return {};
}
// 전용 속성으로 확인한 가상 캡처에만 이름 변경을 허용한다. 정책 서비스에 변경을
// 요청한 뒤 MMDevice 속성을 다시 읽어 요청이 실제 적용되었는지 확인한다.
void name_output(const std::string &id)
{
    auto list = devices();
    bool valid = false;
    for (auto &d : list)
        if (d.capture && d.native && d.id == id)
            valid = true;
    if (!valid)
        throw std::runtime_error("Not a QuietMic capture endpoint");
    Com com;
    auto e = enumerator();
    ComPtr<IMMDevice> d;
    check(e->GetDevice(wide(id).c_str(), &d), "Get output endpoint");
    ComPtr<IPropertyStore> p;
    check(d->OpenPropertyStore(STGM_READ, &p), "Read output name");
    // FriendlyName은 Windows가 장치 설명과 어댑터 이름을 합성한 읽기 전용 값이다.
    // 이미 설정된 이름이면 쓰기를 요청하지 않아 일반 실행에 관리자 권한이 필요하지 않다.
    if (property(p.Get(), PKEY_Device_DeviceDesc) == "QuietMic Output")
        return;
    PROPVARIANT value;
    PropVariantInit(&value);
    check(InitPropVariantFromString(L"QuietMic Output", &value), "Create device name");
    // Windows 오디오 정책 서비스에 사용자용 장치 설명의 변경을 요청한다.
    // 전달한 PROPVARIANT는 호출이 끝난 직후 정리하고 결과 코드의 실패를 검사한다.
    ComPtr<AudioPolicy> policy;
    check(CoCreateInstance(AudioPolicyClass, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy)),
          "Audio naming service");
    auto hr = policy->SetPropertyValue(wide(id).c_str(), FALSE, PKEY_Device_DeviceDesc, &value);
    PropVariantClear(&value);
    check(hr, "Set device name");
    p.Reset();
    check(d->OpenPropertyStore(STGM_READ, &p), "Verify device name");
    if (property(p.Get(), PKEY_Device_DeviceDesc) != "QuietMic Output")
        throw std::runtime_error("Device name verification failed");
}
// Temporary speaker audition uses the same processed frame as the virtual driver.
// 출력 장치와 PCM 큐를 이 객체가 소유하며 오디오 작업 스레드만 호출한다.
// 캡처와 재생의 이벤트 주기가 달라도 큐를 통해 10 ms 처리 프레임을 재생 요청 길이에 맞춘다.
class SpeakerPreview
{
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> sink;
    UINT capacity = 0;
    PreviewBuffer queue;
    bool started = false;

  public:
    Event event;
    explicit SpeakerPreview(const std::string &id)
    {
        auto e = enumerator();
        ComPtr<IMMDevice> device;
        check(e->GetDevice(wide(id).c_str(), &device), "Test speaker disconnected");
        check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client),
              "Activate test speaker");
        auto wave_format = float_wave_format();
        // 공유 모드로 앱의 모노 float 형식을 요청하고 실제 장치 형식 변환은 WASAPI에 맡긴다.
        // 시간 인수 300000은 100 ns 단위여서 30 ms다. 이벤트 등록과 서비스 획득 후 먼저 무음으로 채운다.
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                 300000, 0, &wave_format, nullptr),
              "Open test speaker");
        check(client->SetEventHandle(event.h), "Test speaker event");
        check(client->GetBufferSize(&capacity), "Test speaker buffer");
        check(client->GetService(IID_PPV_ARGS(&sink)), "Test speaker render service");
        render(true);
    }
    // 재생을 멈추고 WASAPI 내부 대기 샘플을 버린 다음 COM 참조가 해제되도록 한다.
    ~SpeakerPreview()
    {
        if (client)
        {
            client->Stop();
            client->Reset();
        }
    }
    // 음소거 전환에서는 앱 큐뿐 아니라 WASAPI에 이미 넘긴 음성도 제거해야 한다.
    // Stop → Reset → 무음 채움 순서로 비우고, 다음 write가 다시 두 프레임을 모아 시작한다.
    void reset()
    {
        queue.reset();
        check(client->Stop(), "Pause test output");
        check(client->Reset(), "Clear test output");
        // Reset discards WASAPI padding too. Re-prime before starting so the next
        // render event does not request a full buffer before capture has produced it.
        render(true);
        started = false; // Rebuild the same scheduling margin after every mute change.
    }
    void write(const std::array<float, FrameSize> &frame, float volume)
    {
        queue.write(frame, volume);
        // Two processed frames provide one frame of scheduling margin between
        // independent capture/render clocks; the queue remains bounded to 40 ms.
        // 초기 무음은 이미 장치 버퍼에 있고, 처리된 두 프레임은 앱 큐에서 대기한다.
        // 이 준비 상태에서만 Start하여 첫 재생 이벤트가 데이터를 너무 일찍 요구하지 않게 한다.
        if (!started && queue.ready_to_start())
        {
            check(client->Start(), "Start speaker test");
            started = true;
        }
    }
    driver::Status status() const
    {
        return queue.status();
    }
    // padding은 WASAPI에 아직 재생되지 않고 남은 샘플 시점 수다. 전체 용량에서 빼서
    // 빈 부분만 빌린 뒤 Queue로 채우고 ReleaseBuffer로 재생 엔진에 소유권을 돌려준다.
    void render(bool muted)
    {
        UINT padding = 0;
        check(client->GetCurrentPadding(&padding), "Test speaker padding");
        if (padding >= capacity)
            return;
        UINT count = capacity - padding;
        BYTE *data = nullptr;
        check(sink->GetBuffer(count, &data), "Test speaker buffer");
        queue.render(reinterpret_cast<float *>(data), count, muted);
        check(sink->ReleaseBuffer(count, muted ? AUDCLNT_BUFFERFLAGS_SILENT : 0), "Play filtered microphone");
    }
};
// 중단 신호는 수동 리셋으로 유지한다. 현재 대기와 재연결 대기 모두 같은 신호를
// 관찰할 수 있고, 다음 start에서 명시적으로 초기화한다.
AudioEngine::AudioEngine()
{
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_)
        throw std::runtime_error("Stop event creation failed");
}
// worker가 this와 stop_을 참조하므로 먼저 종료까지 기다린 후 콜백과 핸들을 정리한다.
AudioEngine::~AudioEngine()
{
    stop();
    set_changed_handler({});
    CloseHandle(stop_);
}
void AudioEngine::update(const Settings &s)
{
    // 구조체 전체 복사를 잠금으로 보호하고 수치를 검증한다. 입력/출력 ID는 start 인수로
    // 고정한 경로를 사용하므로 처리 설정 복사본에서는 지워 오디오 스레드의 불필요한 문자열을 줄인다.
    std::lock_guard l(mutex_);
    settings_ = s;
    sanitize(settings_);
    settings_.input_id.clear();
    settings_.output_id.clear();
    muted = s.muted;
}
AudioSnapshot AudioEngine::snapshot() const
{
    std::lock_guard lock(mutex_);
    // 오류 문자열은 mutex_ 아래에서, 미터는 atomic으로 읽어 UI용 값을 만든다.
    // 각 미터가 동시에 측정된 하나의 오디오 프레임임을 보장하는 스냅샷은 아니다.
    const auto state = lifecycle_.load();
    return {state == Lifecycle::Running,
            state == Lifecycle::Recovering,
            muted.load(),
            state == Lifecycle::Starting,
            input_peak.load(),
            output_peak.load(),
            voice.load(),
            processing_ms.load(),
            monitor_volume.load(),
            underruns.load(),
            overruns.load(),
            error_};
}
// 미리 듣기 볼륨만 바꾸는 경로다. 유효하지 않은 수치는 0으로 처리하고 0~1로 제한한다.
void AudioEngine::set_monitor_volume(float value)
{
    monitor_volume = std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
}
// 콜백 교체와 worker의 콜백 복사를 같은 잠금으로 보호한다. 이미 복사한 콜백 실행까지
// 취소하는 것은 아니므로 콜백 대상 객체의 파괴 전에는 worker 종료도 보장해야 한다.
void AudioEngine::set_changed_handler(std::function<void()> handler)
{
    std::lock_guard lock(callback_mutex_);
    changed = std::move(handler);
}
void AudioEngine::notify_changed()
{
    std::function<void()> handler;
    {
        std::lock_guard lock(callback_mutex_);
        handler = changed;
    }
    // 복사한 콜백은 잠금을 풀고 호출한다. 콜백에서 다른 엔진 메서드를 부를 때
    // callback_mutex_를 다시 기다리며 교착되지 않게 하며 실행 스레드는 여전히 worker다.
    if (handler)
        handler();
}
// 이벤트로 모든 대기를 깨운 뒤 join으로 자원 정리가 끝날 때까지 기다린다.
// worker 스스로 join할 수 없으므로 수명 제어 쪽에서 호출해야 한다.
void AudioEngine::stop()
{
    SetEvent(stop_);
    if (worker_.joinable())
        worker_.join();
    lifecycle_ = Lifecycle::Stopped;
    input_peak = output_peak = voice = 0;
}
// 이전 실행을 완전히 종료한 후 오류/중단 신호를 지우고 Starting으로 바꾼다.
// 입출력 ID는 람다에 값으로 캡처하여 호출자 문자열 수명과 관계없이 worker가 사용한다.
void AudioEngine::start(const std::string &in, const std::string &out, bool preview)
{
    start_session(in, out, preview, {});
}

void AudioEngine::start_to_sink(const std::string &input, const std::string &output,
                                std::shared_ptr<AudioFrameSink> sink)
{
    if (!sink)
        throw std::invalid_argument("Audio output sink is required");
    start_session(input, output, false, std::move(sink));
}

void AudioEngine::start_session(const std::string &in, const std::string &out, bool preview,
                                std::shared_ptr<AudioFrameSink> sink)
{
    stop();
    {
        std::lock_guard l(mutex_);
        error_.clear();
    }
    ResetEvent(stop_);
    lifecycle_ = Lifecycle::Starting;
    try
    {
        worker_ = std::thread(
            [this, in, out, preview, sink = std::move(sink)]
            {
                run(in, out, preview, sink);
            });
        // 스레드 생성 자체가 실패한 경우 아직 worker가 없으므로 여기서 Stopped로 되돌린다.
    }
    catch (...)
    {
        lifecycle_ = Lifecycle::Stopped;
        throw;
    }
}
// 장치 한 번 열기(run_stream)를 감싸는 복구 루프다. 실제로 Running까지 도달했던
// 일반 경로에서 특정 장치/서비스 오류가 났을 때만 같은 장치가 돌아오기를 기다린다.
// 전송 객체를 명시한 경로도 같은 입력/출력 ID가 돌아온 경우에만 재개한다.
// 이름이 비슷한 마이크나 기본 장치로 대체하지 않는다. 장치 소유권 식별은 세션 생성자의 책임이다.
static bool sink_route_present(const std::vector<Device> &list, const std::string &input,
                               const std::string &output)
{
    if (input.empty() || output.empty() || input == output)
        return false;
    const auto input_found = std::any_of(list.begin(), list.end(),
                                         [&](const Device &d)
                                         {
                                             return d.id == input && d.capture && !d.native;
                                         });
    const auto output_found = std::any_of(list.begin(), list.end(),
                                          [&](const Device &d)
                                          {
                                              return d.id == output && d.capture;
                                          });
    return input_found && output_found;
}

void AudioEngine::run(std::string input, std::string output, bool preview,
                      std::shared_ptr<AudioFrameSink> sink)
{
    bool has_run = false;
    std::string capture_id;
    while (WaitForSingleObject(stop_, 0) != WAIT_OBJECT_0)
    {
        bool retryable = false;
        try
        {
            run_stream(input, output, capture_id, preview, sink);
            break;
            // HRESULT가 장치 무효화/자원 무효화/서비스 중지인 경우만 재시도 후보로 분류한다.
            // 일반 예외나 최초 열기 실패, 제한 시간의 미리 듣기에는 자동 재연결을 적용하지 않는다.
        }
        catch (const AudioFailure &e)
        {
            retryable = e.code == AUDCLNT_E_DEVICE_INVALIDATED || e.code == AUDCLNT_E_RESOURCES_INVALIDATED ||
                        e.code == AUDCLNT_E_SERVICE_NOT_RUNNING;
            std::lock_guard lock(mutex_);
            error_ = e.what();
        }
        catch (const std::exception &e)
        {
            std::lock_guard lock(mutex_);
            error_ = e.what();
        }
        // run_stream에서 성공적으로 Running이 된 이력이 있어야 복구한다. 중단 요청도 함께
        // 확인하여 사용자가 멈춘 뒤 장치 재연결 루프로 다시 들어가지 않게 한다.
        has_run = has_run || lifecycle_.load() == Lifecycle::Running;
        input_peak = output_peak = voice = 0;
        if (preview || !has_run || !retryable || WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0)
            break;
        lifecycle_ = Lifecycle::Recovering;
        notify_changed();
        // No healthy-state polling. A failed route sleeps until cancelled, checking
        // only the original three endpoint IDs every three seconds while disconnected.
        // 정상 실행 중에는 장치 목록을 주기 조회하지 않는다. 장애 중에만 3초마다 기존 ID를
        // 확인하며 stop_이 신호를 받으면 3초를 다 기다리지 않고 즉시 대기를 끝낸다.
        bool returned = false;
        while (WaitForSingleObject(stop_,
                                   static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                          format::reconnect_interval)
                                                          .count())) == WAIT_TIMEOUT)
        {
            try
            {
                const auto list = devices();
                if (sink ? sink_route_present(list, input, output)
                         : original_route_present(list, input, output, capture_id))
                {
                    returned = true;
                    break;
                }
            }
            catch (const std::exception &)
            { /* Audio service may still be restarting. */
            }
        }
        if (!returned)
            break;
    }
    // 시작 직후 stop이 오면 run_stream에 진입하지 않을 수 있다. 스트림 내부의
    // 정리 객체만으로는 그 경로를 처리할 수 없으므로 worker 종료 경계에서도 비운다.
    if (sink)
    {
        try
        {
            sink->reset();
        }
        catch (const std::exception &e)
        {
            std::lock_guard lock(mutex_);
            error_ = e.what();
        }
    }
    lifecycle_ = Lifecycle::Stopped;
    input_peak = output_peak = voice = 0;
    notify_changed();
}
// 한 연결의 전체 수명이다. 진입 시 경로 검증부터 하고 이 함수 안에 COM/캡처/DSP/출력
// 객체를 두어 정상 종료나 예외 모두에서 한 연결의 자원이 정리되게 한다.
void AudioEngine::run_stream(const std::string &input, const std::string &output, std::string &capture_id,
                             bool preview, const std::shared_ptr<AudioFrameSink> &sink)
{
    // 정상 중단뿐 아니라 장치 오류/예외에서도 대기 음성을 폐기한다. reset 실패가
    // 원래 오류를 덮거나 예외 전파 중 프로그램을 종료시키지 않도록 소멸 경계에서 처리한다.
    struct ClearSink
    {
        const std::shared_ptr<AudioFrameSink> &sink;
        ~ClearSink()
        {
            if (sink)
            {
                try
                {
                    sink->reset();
                }
                catch (...)
                {
                }
            }
        }
    } clear_sink{sink};
    const auto list = devices();
    bool preview_route = false, physical = false;
    for (const auto &d : list)
    {
        if (d.id == input && d.capture && !d.native)
            physical = true;
        if (d.id == output && !d.capture && !d.native)
            preview_route = true;
    }
    // 미리 듣기는 물리 마이크 + 일반 재생 장치, 본 실행은 물리 마이크 + QuietMic 캡처를
    // 요구한다. 복구 시 저장된 capture_id가 달라지면 새 장치로 자동 전환하지 않고 실패시킨다.
    if (sink      ? !sink_route_present(list, input, output)
        : preview ? !(physical && preview_route)
                  : (!original_route_present(list, input, output, output) ||
                     (!capture_id.empty() && capture_id != output)))
        throw AudioFailure(AUDCLNT_E_DEVICE_INVALIDATED,
                           "QuietMic virtual microphone or original input unavailable");
    capture_id = output;
    Com com;
    auto e = enumerator();
    ComPtr<IMMDevice> in;
    check(e->GetDevice(wide(input).c_str(), &in), "Input disconnected");
    ComPtr<IAudioClient> client;
    check(in->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "Activate input");
    auto wave_format = float_wave_format();
    // 48 kHz 모노 float 캡처를 공유 모드로 요청한다. 200000은 100 ns 단위의 20 ms이며
    // 실제 전달 패킷 크기는 고정 480이라고 가정하지 않는다. 이벤트를 등록한 뒤 Start한다.
    check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST |
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                             200000, 0, &wave_format, nullptr),
          "Open microphone");
    Event captured;
    check(client->SetEventHandle(captured.h), "Capture event");
    ComPtr<IAudioCaptureClient> capture;
    check(client->GetService(IID_PPV_ARGS(&capture)), "Capture service");
    // 미리 듣기는 WASAPI, 기존 가상 출력은 IOCTL을 쓴다. 외부 전송 세션이 주어지면
    // 두 객체 모두 만들지 않고 같은 clean 프레임을 AudioFrameSink에 전달한다.
    std::unique_ptr<DriverTransport> transport;
    std::unique_ptr<SpeakerPreview> speaker;
    if (preview)
        speaker = std::make_unique<SpeakerPreview>(output);
    else if (!sink)
        transport = std::make_unique<DriverTransport>();
    DspPipeline dsp;
    // frame은 모으는 입력 480샘플, clean은 처리된 출력 480샘플이다. filled는 이전
    // 캡처 패킷에서 남은 개수이므로 이벤트/패킷 경계를 넘어 한 DSP 프레임을 완성할 수 있다.
    std::array<float, FrameSize> frame{}, clean{};
    size_t filled = 0;
    DWORD task = 0;
    // 작업 스레드를 Windows 오디오 스케줄링 대상으로 등록한다. 등록 성공 시 Revert의
    // 소멸자가 해제하므로 스트림 오류로 빠져나가도 스케줄링 등록을 남기지 않는다.
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    struct Revert
    {
        HANDLE h;
        ~Revert()
        {
            if (h)
                AvRevertMmThreadCharacteristics(h);
        }
    } revert{mm};
    Settings settings;
    {
        std::lock_guard lock(mutex_);
        settings = settings_;
    }
    bool was_muted = muted.load();
    check(client->Start(), "Start microphone");
    // 마이크 Start 성공 뒤에만 정리 객체를 만든다. 함수가 끝나거나 아래에서 예외가 나면
    // ComPtr가 해제되기 전에 캡처를 Stop한다.
    struct Stop
    {
        IAudioClient *p;
        ~Stop()
        {
            p->Stop();
        }
    } stop_client{client.Get()};
    underruns = overruns = 0;
    {
        std::lock_guard lock(mutex_);
        error_.clear();
        lifecycle_ = Lifecycle::Running;
    }
    notify_changed();
    // 대기 배열 순서가 우선순위다. 여러 이벤트가 함께 신호 상태면 가장 앞의 stop_,
    // 그다음 캡처, 마지막 재생이 선택된다. 캡처/재생 이벤트는 자동 리셋이고 stop_은 수동 리셋이다.
    HANDLE events[] = {stop_, captured.h, speaker ? speaker->event.h : nullptr};
    const auto preview_started = std::chrono::steady_clock::now();
    auto last_capture = preview_started;
    while (true)
    {
        // 이벤트를 기다리며 CPU를 양보한다. 최대 2초 대기로 입력이 멈춘 경우를 감지하고,
        // 미리 듣기는 별도로 전체 60초 제한과 마지막 실제 캡처 이후 2초 제한도 확인한다.
        auto wait = WaitForMultipleObjects(speaker ? 3 : 2, events, FALSE, 2000);
        if (wait == WAIT_OBJECT_0)
            break;
        if (speaker && std::chrono::steady_clock::now() - preview_started >= format::preview_limit)
            break;
        if (speaker && std::chrono::steady_clock::now() - last_capture >= std::chrono::seconds(2))
            throw AudioFailure(AUDCLNT_E_DEVICE_INVALIDATED, "Test microphone stopped responding");
        // 재생 이벤트이면 스피커의 빈 공간만 채우고 다음 대기로 돌아간다. 캡처 설정 복사와
        // DSP 실행은 캡처 이벤트 경로에서 수행한다.
        if (speaker && wait == WAIT_OBJECT_0 + 2)
        {
            speaker->render(muted.load());
            const auto status = speaker->status();
            underruns = status.underruns;
            overruns = status.overruns;
            continue;
        }
        if (wait == WAIT_TIMEOUT)
            throw AudioFailure(AUDCLNT_E_DEVICE_INVALIDATED, "Microphone stopped responding");
        if (wait == WAIT_FAILED)
            throw std::runtime_error("Capture event wait failed");
        {
            // 오디오 작업이 UI의 설정 잠금 때문에 기다리지 않도록 잠금 획득을 한 번만 시도한다.
            // 실패하면 직전 Settings 복사본을 사용하고 다음 캡처 기회에 다시 반영한다.
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (lock.owns_lock())
                settings = settings_;
        }
        // 음소거는 별도 atomic을 매번 읽어 설정 복사 잠금 실패와 무관하게 반영한다.
        // 전환 시 전송/미리 듣기 큐와 조립 중인 입력 프레임을 비워 이전 대기 음성을 제거한다.
        settings.muted = muted.load();
        if (settings.muted != was_muted)
        {
            if (transport)
                transport->reset();
            if (sink)
                sink->reset();
            if (speaker)
                speaker->reset();
            was_muted = settings.muted;
            filled = 0;
        }
        UINT packet = 0;
        check(capture->GetNextPacketSize(&packet), "Microphone disconnected");
        // 한 캡처 이벤트에 여러 패킷이 대기할 수 있으므로 GetNextPacketSize가 0일 때까지
        // 모두 소비한다. 패킷 크기와 DSP 프레임 크기가 다를 수 있어 안쪽에서 샘플을 모은다.
        while (packet)
        {
            BYTE *data;
            UINT count;
            DWORD flags;
            check(capture->GetBuffer(&data, &count, &flags, nullptr, nullptr), "Read microphone");
            if (count)
                last_capture = std::chrono::steady_clock::now();
            // GetBuffer로 빌린 WASAPI 메모리는 반드시 ReleaseBuffer로 돌려줘야 한다.
            // DSP/출력 중 예외가 나도 이 지역 객체가 반환하며, 정상 반환 후에는 n=0으로 중복 반환을 막는다.
            struct Release
            {
                IAudioCaptureClient *c;
                UINT n;
                ~Release()
                {
                    if (n)
                        c->ReleaseBuffer(n);
                }
            } release{capture.Get(), count};
            float peak = 0;
            for (UINT i = 0; i < count; i++)
            {
                // SILENT 플래그일 때는 data 내용을 읽지 않고 0을 사용한다. NaN/무한대도 먼저 0으로
                // 바꾸어 입력 미터와 DSP 이력에 비정상 값이 퍼지지 않게 한다.
                float value = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0 : reinterpret_cast<float *>(data)[i];
                if (!std::isfinite(value))
                    value = 0;
                peak = std::max(peak, std::abs(value));
                frame[filled++] = value;
                // 정확히 480샘플이 모였을 때만 상태를 가진 DSP를 한 번 호출한다. 처리 소요 시간만
                // 따로 재며, 남은 캡처 샘플은 같은 루프에서 다음 프레임으로 이어서 모은다.
                if (filled == FrameSize)
                {
                    auto begin = std::chrono::steady_clock::now();
                    voice = dsp.process(frame, clean, settings);
                    filled = 0;
                    processing_ms =
                        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin)
                            .count();
                    // DSP 계산 도중 UI가 음소거를 켰을 수 있으므로 출력 직전 최신 값을 다시 확인한다.
                    // 이 경우 이번 clean 전체를 0으로 바꾼 뒤 선택한 출력 경로에 전달한다.
                    if (muted.load())
                        clean.fill(0);
                    if (speaker)
                    {
                        speaker->write(clean, monitor_volume.load());
                        const auto status = speaker->status();
                        underruns = status.underruns;
                        overruns = status.overruns;
                    }
                    else if (sink)
                    {
                        const auto status = sink->write(clean);
                        underruns = status.underruns;
                        overruns = status.overruns;
                    }
                    else
                    {
                        auto status = transport->write(clean);
                        underruns = status.underruns;
                        overruns = status.overruns;
                    }
                    // 출력 미터는 clean 프레임의 최대 절댓값이다. 미리 듣기 전용 볼륨은 speaker 내부에서
                    // 적용하므로 이 미터는 모니터 볼륨을 곱하기 전의 처리 결과를 보여준다.
                    float output_level = 0;
                    for (auto sample : clean)
                        output_level = std::max(output_level, std::abs(sample));
                    output_peak = output_level;
                }
            }
            input_peak = peak;
            // Release before querying the next packet.
            // 현재 패킷의 소유권을 반환한 뒤 다음 패킷 크기를 조회해야 WASAPI 호출 순서가 맞는다.
            // 정상 반환이 끝나면 위 정리 객체의 반환 책임을 해제한다.
            check(capture->ReleaseBuffer(count), "Release microphone");
            release.n = 0;
            check(capture->GetNextPacketSize(&packet), "Next microphone packet");
        }
    }
}
} // namespace qm
