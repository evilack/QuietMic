// 장치 목록, 오디오 작업 스레드, UI가 읽는 상태의 공개 인터페이스다. 실제 캡처와 DSP는
// audio.cpp의 작업 스레드에서 실행하고 UI는 설정 전달과 snapshot 조회로 소통한다.
#pragma once
#include "settings.hpp"
#include "text_encoding.hpp"
#include "audio_frame_sink.hpp"
#include <memory>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
namespace qm
{
// id는 Windows의 고유 엔드포인트 ID, name은 표시 이름이다. capture는 입력 방향,
// native는 QuietMic 전용 식별 속성이 확인된 장치라는 뜻이다.
struct Device
{
    std::string id, name, interface_name, container;
    bool capture = false, native = false;
};
std::vector<Device> devices();
// 출력이라 부르지만 다른 앱이 마이크로 읽는 Windows 캡처 엔드포인트를 판별한다.
bool quietmic_output(const Device &device);
// Select only endpoints tagged by the QuietMic driver's private identity property.
std::string configured_output(const std::vector<Device> &, const std::string &capture_id);
// 선택했던 물리 마이크와 가상 마이크 ID가 여전히 같은 경로인지 확인한다.
// 장치가 사라졌을 때 임의의 기본 장치로 바꾸지 않기 위한 조건이다.
bool original_route_present(const std::vector<Device> &, const std::string &input_id,
                            const std::string &render_id, const std::string &capture_id);
void name_output(const std::string &capture_id);
// UI로 복사할 상태다. 처리 시간은 10 ms 프레임 한 번의 계산 소요(ms), voice는 음성 확률,
// 피크는 선형 진폭, monitor_volume은 미리 듣기 배율이다.
struct AudioSnapshot
{
    bool running, recovering, muted, starting;
    float input_peak, output_peak, voice, processing_ms, monitor_volume;
    unsigned underruns, overruns;
    std::string error;
};
class AudioEngine
{
    // 한 번의 실행마다 worker_ 하나가 캡처/DSP/출력 객체를 소유한다. stop_ 이벤트로 중단을
    // 요청하고 join으로 종료를 기다린 뒤에 객체와 이벤트 핸들을 파괴한다.
    std::thread worker_;
    HANDLE stop_ = nullptr;
    // mutex_는 설정 구조체와 오류 문자열, callback_mutex_는 콜백 교체/복사를 보호한다.
    // 개별 미터와 수명 상태는 atomic으로 공유하므로 UI가 작업 스레드의 값을 안전하게 읽는다.
    mutable std::mutex mutex_;
    std::mutex callback_mutex_;
    Settings settings_;
    std::string error_;
    void start_session(const std::string &, const std::string &, bool, std::shared_ptr<AudioFrameSink>);
    void run(std::string input, std::string output, bool preview, std::shared_ptr<AudioFrameSink> sink);
    void run_stream(const std::string &input, const std::string &output, std::string &capture_id,
                    bool preview, const std::shared_ptr<AudioFrameSink> &sink);

    // 시작 요청 → 장치 열기 → Running, 재시도 가능한 장애 → Recovering,
    // 중단/복구 불가/미리 듣기 종료 → Stopped 순으로 상태가 바뀐다.
    enum class Lifecycle
    {
        Stopped,
        Starting,
        Running,
        Recovering
    };
    std::atomic<Lifecycle> lifecycle_ = Lifecycle::Stopped;
    std::atomic<bool> muted = false;
    std::atomic<float> input_peak = 0, output_peak = 0, voice = 0, processing_ms = 0;
    std::atomic<unsigned> underruns = 0, overruns = 0;
    std::function<void()> changed;
    std::atomic<float> monitor_volume = 0.25f;
    void notify_changed();

  public:
    AudioEngine();
    ~AudioEngine();
    // 호출자는 start/stop과 객체 수명 변경을 직렬화해야 한다. start는 기존 스레드를 먼저
    // 끝내고 새 경로를 연다. update는 실행 중 처리 설정만 바꾸며 경로 자체를 다시 열지 않는다.
    void start(const std::string &, const std::string &, bool preview = false);
    // 명시적으로 생성한 전송 세션에 처리 결과를 보낸다. 호출자는 해당 세션이 소유한
    // 출력 엔드포인트 ID를 전달해야 하며, sink는 작업 스레드가 끝날 때까지 공유 소유한다.
    void start_to_sink(const std::string &input, const std::string &output,
                       std::shared_ptr<AudioFrameSink> sink);
    void stop();
    void update(const Settings &);
    AudioSnapshot snapshot() const;
    void set_monitor_volume(float);
    // Callbacks run on the worker thread; detach and stop before destroying their owner.
    void set_changed_handler(std::function<void()>);
};
} // namespace qm
