// 컨트롤러의 생명주기와 처리/미리듣기/설치 상태 전이를 구현한다. UI 조작은 이벤트 루프에서 수행한다.
#include "app_controller.hpp"
#include "build_info.hpp"
#include "setup.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace qm
{
namespace
{
using namespace std::chrono_literals;
// 표시는 100ms마다, 설정 저장은 마지막 변경 400ms 뒤 수행한다.
constexpr auto meter_interval = 100ms;
constexpr auto save_delay = 400ms;
// 장치 목록에서 표시 이름만 추출한다. 실제 장치 선택은 별도의 ID로 관리한다.
auto names(const std::vector<Device> &devices)
{
    std::vector<std::string> result;
    for (const auto &device : devices)
        result.emplace_back(device.name);
    return result;
}

std::vector<std::string> language_names()
{
    std::vector<std::string> result;
    for (const auto &language : localization::supported_languages())
        result.emplace_back(language.self_name);
    return result;
}

int language_index(std::string_view code)
{
    int index = 0;
    for (const auto &language : localization::supported_languages())
    {
        if (language.code == code)
            return index;
        ++index;
    }
    return 0;
}

TrayText tray_text(const localization::Catalog &catalog)
{
    const auto value = [&](std::string_view key)
    {
        return std::string(catalog.text(key));
    };
    return {value("tray.show"),     value("tray.volume"),      value("tray.mute"),
            value("tray.unmute"),   value("tray.start"),       value("tray.stop"),
            value("tray.denoise"),  value("tray.startup"),     value("tray.quit"),
            value("tray.tip.idle"), value("tray.tip.running"), value("tray.tip.muted"),
            value("tray.tip.error")};
}
} // namespace
// 멤버는 선언 순서로 초기화된다. 저장된 사용자 설정과 언어를 읽어 첫 화면을 준비한다.
AppController::AppController(platform::ApplicationPaths paths)
    : paths_(std::move(paths)), ui_(paths_.assets),
      settings_(load_settings(paths_.settings)),
      catalog_(localization::Catalog::load(paths_.assets / L"i18n", settings_.language)),
      virtual_microphone_(paths_.executable.parent_path() / L"components" / L"usbip")
{
}
// shared_ptr 생성 후 initialize를 호출해야 weak_from_this로 유효한 약한 참조를 얻을 수 있다.
std::shared_ptr<AppController> AppController::create(platform::ApplicationPaths paths)
{
    auto app = std::shared_ptr<AppController>(new AppController(std::move(paths)));
    app->initialize();
    return app;
}
// run이 예외로 끝나도 자원을 정리한다. 정상 종료 뒤에도 다시 정리할 수 있는 종료 흐름이다.
AppController::~AppController()
{
    shutdown();
}
// 초기 화면과 외부 이벤트를 연결한다. 장치 열거 오류는 guard를 통해 상태 문구로 알린다.
void AppController::initialize()
{
    ui_.set_version(build::version);
    publish_language();
    // 트레이 전용 스레드의 콜백이다. 컨트롤러가 살아 있으면 post로 UI 이벤트 루프에 작업을 맡긴다.
    tray_ = std::make_unique<Tray>(paths_.assets, tray_text(catalog_),
                                   [weak = weak_from_this()](TrayCommand command)
                                   {
                                       if (auto app = weak.lock())
                                           app->post(
                                               [command](AppController &self)
                                               {
                                                   self.tray_command(command);
                                               });
                                   });
    // 오디오 스레드도 UI를 직접 만지지 않는다. 소멸 뒤 알림은 weak.lock 실패로 무시한다.
    audio_.set_changed_handler(
        [weak = weak_from_this()]
        {
            if (auto app = weak.lock())
                app->post(&AppController::audio_changed);
        });
    virtual_microphone_.set_changed_handler(
        [weak = weak_from_this()]
        {
            if (auto app = weak.lock())
                app->post(&AppController::session_changed);
        });
    // OS 알림은 UI 이벤트 루프로 옮긴 뒤 묶어서 처리한다. 평상시 폴링 비용은 없다.
    guard(
        [&]
        {
            device_watcher_ = std::make_unique<DeviceWatcher>(
                [weak = weak_from_this()]
                {
                    // strong 참조를 콜백 스레드에 만들지 않는다. 마지막 소유권이 이곳에서
                    // 해제되어 컨트롤러/COM이 다른 스레드에서 파괴되는 일을 막는다.
                    slint::invoke_from_event_loop(
                        [weak]
                        {
                            if (auto app = weak.lock(); app && !app->closing_)
                                app->guard(
                                    [&]
                                    {
                                        app->schedule_device_refresh();
                                    });
                        });
                });
        });
    bind_ui();
    sync_controls();
    guard(
        [&]
        {
            refresh();
        });
    audio_.update(settings_);
}
// 로그인 실행 여부와 설정에 따라 창 표시/자동 처리를 결정한 뒤 이벤트 루프를 실행한다.
int AppController::run(bool login)
{
    if (!login || !settings_.start_hidden)
        show();
    if (settings_.auto_process && !settings_.input_id.empty())
        guard(
            [&]
            {
                toggle_processing();
            });
    // 창을 숨겨도 앱을 유지해야 하므로 명시적인 quit 요청까지 이벤트 루프를 유지한다.
    slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);
    shutdown();
    save();
    return exit_code_;
}
// closing으로 새 동작을 차단한 뒤 타이머→오디오 알림→오디오→트레이 순으로 정리한다.
void AppController::shutdown()
{
    closing_ = true;
    device_watcher_.reset();
    device_refresh_timer_.stop();
    meter_.stop();
    save_timer_.stop();
    // 정지 과정에서 추가 UI 작업이 예약되지 않도록 변경 알림부터 비운다.
    audio_.set_changed_handler({});
    virtual_microphone_.set_changed_handler({});
    audio_.stop();
    virtual_microphone_.stop();
    tray_.reset();
}
// 종료 코드를 기억하고 루프에 종료를 요청한다. 실제 정리와 마지막 저장은 run에서 이어진다.
void AppController::request_quit(int code)
{
    exit_code_ = code;
    closing_ = true;
    slint::quit_event_loop();
}
// 현재 설정을 사용자 프로필에 저장한다.
void AppController::save()
{
    save_settings(paths_.settings, settings_);
}
// 설정을 유효 범위로 보정하고 엔진에 전달한다. 디스크 저장만 잠시 늦춘다.
void AppController::commit()
{
    sanitize(settings_);
    audio_.update(settings_);
    // 같은 단발 타이머를 재시작하므로 연속 입력은 마지막 입력 뒤 한 번 저장된다. 이를 디바운스라 한다.
    save_timer_.start(slint::TimerMode::SingleShot, save_delay, guarded(&AppController::save));
}
// 숫자 리소스 ID를 UTF-8 안내 문구로 바꾸어 UI에 전달한다.
void AppController::set_status(std::string_view key)
{
    ui_.set_status(catalog_.text(key));
}
// 엔진 상태 사본으로 UI/트레이를 갱신한다. 아래 분기 순서가 여러 상태의 표시 우선순위다.
void AppController::sync_status()
{
    const auto state = audio_.snapshot();
    const auto session = virtual_microphone_.snapshot();
    const bool session_failed = session.state == usbip::SessionState::Failed;
    // 완전히 멈춘 경우에만 미리듣기를 해제한다. 시작/재연결 중은 아직 진행 중으로 취급한다.
    if (!state.running && !state.recovering && !state.starting)
        previewing_ = false;
    // 재연결은 중지할 수 있는 실행 상태로, 설치/시작은 조작을 막는 busy 상태로 전달한다.
    ui_.set_processing({state.running || state.recovering, state.recovering,
                        preparing_output() || state.starting, previewing_});
    tray_->update(state.running || state.recovering, settings_.muted, settings_.denoise, settings_.autostart,
                  !state.error.empty() || session_failed);
    // 설치→재연결→오류→시작→실행→정지 순으로 하나의 상태 문구를 선택한다.
    if (preparing_output())
        set_status("status.setup.running");
    else if (session_failed)
        ui_.set_status(std::string(catalog_.text("error.technical-detail")) + " " + session.error);
    else if (state.recovering)
        set_status("status.recovering");
    else if (!state.error.empty())
        ui_.set_status(std::string(catalog_.text("error.technical-detail")) + " " + state.error);
    else if (state.starting)
        set_status(previewing_ ? "status.preview.starting" : "status.connecting");
    else if (state.running && previewing_)
        set_status(settings_.muted ? "status.preview.muted" : "status.preview.running");
    else if (state.running)
        set_status(settings_.muted ? "status.microphone.muted" : "status.microphone.running");
    else
        set_status("status.stopped");
}
// 연결 한 번에 추가/활성화/속성 알림이 연속 발생하므로 마지막 알림 200ms 뒤 한 번 열거한다.
// 오디오 스트림은 건드리지 않으며 기존 선택은 DeviceCatalog가 장치 ID로 유지한다.
void AppController::schedule_device_refresh()
{
    device_refresh_timer_.start(slint::TimerMode::SingleShot, 200ms, guarded(&AppController::refresh));
}
// 목록을 갱신한 뒤 같은 장치 목록에서 표시 이름과 선택 인덱스를 함께 만든다.
void AppController::refresh()
{
    devices_.refresh(settings_);
    publish_devices();
}

void AppController::publish_devices()
{
    const auto name = devices_.output_name(settings_);
    // UI에는 이름 배열과 인덱스를 전달한다. 없는 장치는 -1, 출력 이름이 없으면 준비 안내를 표시한다.
    ui_.set_devices({names(devices_.inputs()), names(devices_.outputs()), names(devices_.speakers()),
                     devices_.input_index(settings_), devices_.output_index(settings_),
                     devices_.speaker_index(),
                     name.empty() ? std::string(catalog_.text("output.unavailable")) : name});
}

void AppController::publish_language()
{
    ui_.set_catalog(catalog_);
    ui_.set_languages(language_names(), language_index(settings_.language));
    if (tray_)
        tray_->update_text(tray_text(catalog_));
}

void AppController::change_language(int index)
{
    const auto languages = localization::supported_languages();
    if (index < 0 || static_cast<size_t>(index) >= languages.size())
        return;

    const auto code = languages[static_cast<size_t>(index)].code;
    if (settings_.language == code)
        return;
    auto replacement = localization::Catalog::load(paths_.assets / L"i18n", code);
    settings_.language = code;
    catalog_ = std::move(replacement);
    publish_language();
    publish_devices();
    sync_status();
    update_meter();
    // 언어 변경은 오디오 설정이 아니므로 엔진에 update를 보내지 않고 저장만 지연 예약한다.
    save_timer_.start(slint::TimerMode::SingleShot, save_delay, guarded(&AppController::save));
}
// 창을 표시하며 미터를 즉시 갱신한 다음 주기 갱신을 시작한다.
void AppController::show()
{
    visible_ = true;
    ui_.show();
    update_meter();
    meter_.start(slint::TimerMode::Repeated, meter_interval, guarded(&AppController::update_meter));
}
// 창 숨김은 종료가 아니다. 미리듣기는 중지하지만 일반 가상 마이크 처리는 계속된다.
void AppController::hide()
{
    if (previewing_)
    {
        audio_.stop();
        previewing_ = false;
        sync_status();
    }
    // 숨겨진 창에 미터를 그릴 필요가 없으므로 표시 타이머를 멈춘다.
    visible_ = false;
    meter_.stop();
    ui_.hide();
}
// 피크와 발화 확률을 표시용 수치로 바꾸고 처리 성능 문구를 만든다.
void AppController::update_meter()
{
    const auto state = audio_.snapshot();
    // 피크를 0~1로 제한한 후 제곱근으로 작은 신호도 보이게 한다. 오디오 샘플 자체는 바꾸지 않는다.
    ui::MeterReading reading{std::sqrt(std::clamp(state.input_peak, 0.f, 1.f)),
                             std::sqrt(std::clamp(state.output_peak, 0.f, 1.f)),
                             static_cast<int>(state.voice * 100)};
    if (state.running)
    {
        char text[160];
        const auto format = std::string(catalog_.text("performance.running"));
        // 고정 크기 버퍼에 처리 시간과 언더런 횟수를 넣는다. 형식은 문자열 리소스에서 가져온다.
        std::snprintf(text, sizeof(text), format.c_str(), static_cast<double>(state.processing_ms),
                      state.underruns);
        reading.performance = text;
    }
    else
        reading.performance = catalog_.text("performance.idle");
    ui_.set_meter(reading);
}
// UI 스레드에 도착한 엔진 알림을 처리한다. 복구 중이 아닌 오류는 창을 열어 알린다.
void AppController::audio_changed()
{
    sync_status();
    const auto state = audio_.snapshot();
    if (!state.error.empty() && !state.recovering)
        show();
}
// 실행 중이면 중지하고, 정지 상태면 가상 출력 준비를 확인한 뒤 시작한다.
void AppController::toggle_processing()
{
    const auto state = audio_.snapshot();
    // 설치/연결 시작 중에는 중복 시작 요청을 무시한다.
    if (preparing_output() || state.starting)
        return;
    // 재연결 대기 중도 중지할 수 있다. return으로 아래 시작 코드에 도달하지 않게 한다.
    if (state.running || state.recovering)
    {
        audio_.stop();
        sync_status();
        return;
    }
    refresh();
    if (virtual_microphone_.snapshot().state != usbip::SessionState::Ready)
    {
        // 사용자의 시작 의도를 기억하고 연결로 전환한다. 상태 알림에서 성공 확인 후 이어서 시작한다.
        resume_after_setup_ = true;
        show();
        start_setup();
        return;
    }
    previewing_ = false;
    audio_.update(settings_);
    // 목록 순서와 무관한 장치 ID로 비동기 시작을 요청한다.
    const auto session = virtual_microphone_.snapshot();
    settings_.output_id = session.endpoint;
    audio_.start_to_sink(settings_.input_id, session.endpoint, virtual_microphone_.sink());
    sync_status();
}
// 입력→현재 필터→실제 스피커 경로의 미리듣기다. 일반 처리와 동시에 시작하지 않는다.
void AppController::toggle_preview()
{
    if (previewing_)
    {
        audio_.stop();
        previewing_ = false;
        sync_status();
        return;
    }
    const auto state = audio_.snapshot();
    if (preparing_output() || state.starting || state.running || state.recovering)
        return;
    refresh();
    // 선택한 장치가 현재 목록에 없으면 열기 전에 입력/스피커 선택 오류를 알린다.
    if (devices_.speaker_index() < 0 || devices_.input_index(settings_) < 0)
        throw std::runtime_error(std::string(catalog_.text("error.preview.devices")));
    audio_.update(settings_);
    // UI 퍼센트를 어댑터가 0~1로 변환한 음량을 적용한다. 아래 true 인수는 미리듣기 경로를 선택한다.
    audio_.set_monitor_volume(ui_.preview_volume());
    previewing_ = true;
    audio_.start(settings_.input_id, devices_.speaker_id(), true);
    sync_status();
}
// 엔진 정지 상태에서만 가상 장치를 연결한다. 이미 준비된 출력이면 안내만 한다.
void AppController::start_setup()
{
    const auto state = audio_.snapshot();
    if (preparing_output() || state.starting || state.running || state.recovering)
        return;
    refresh();
    if (virtual_microphone_.snapshot().state == usbip::SessionState::Ready)
    {
        set_status("status.already-ready");
        return;
    }
    try
    {
        // 세션의 준비 스레드가 서버 시작→USB/IP 연결→장치 열거→이름 검증을 수행한다.
        // 오디오 끝점이 아직 없다는 이유만으로 이전 드라이버 설치기를 실행하지 않는다.
        virtual_microphone_.start();
    }
    catch (...)
    {
        resume_after_setup_ = false;
        throw;
    }
    sync_status();
}
// 준비 중에는 기다리고, 완료되면 상태와 실제 장치를 확인한 뒤 요청했던 처리를 재개한다.
void AppController::session_changed()
{
    const auto result = virtual_microphone_.snapshot();
    if (result.state == usbip::SessionState::Preparing)
        return;
    if (result.state == usbip::SessionState::Failed)
    {
        // 숨겨진 창에서도 연결 상실은 캡처를 중지하고 오류 아이콘과 창으로 알린다.
        audio_.stop();
        resume_after_setup_ = false;
        sync_status();
        show();
        return;
    }
    sync_status();
    // 재개 여부를 지역 변수로 옮기고 플래그를 먼저 지워 같은 시작 요청을 재사용하지 않는다.
    const bool resume = resume_after_setup_;
    resume_after_setup_ = false;
    if (result.state == usbip::SessionState::Ready)
    {
        // 늦게 예약된 동일 상태 알림이 설정 저장이나 처리 시작을 반복하지 않게 한다.
        if (!resume && settings_.output_id == result.endpoint)
            return;
        // 이번 세션이 확인한 끝점 ID를 선택한다. 이름이 같은 다른 장치를 선택하지 않는다.
        settings_.output_id = result.endpoint;
        refresh();
        if (!devices_.output_ready(settings_))
            // 연결 보고뿐 아니라 실제 출력까지 확인한다. 확인 실패는 저장/자동 시작 전에 예외로 전달한다.
            throw std::runtime_error(std::string(catalog_.text("error.setup.confirmation")));
        commit();
        set_status("status.setup.ready");
        if (resume)
            toggle_processing();
    }
    else
    {
        ui_.set_status(result.error.empty()
                           ? std::string(catalog_.text("status.connection-stopped"))
                           : std::string(catalog_.text("error.technical-detail")) + " " + result.error);
    }
}
bool AppController::preparing_output() const
{
    return virtual_microphone_.snapshot().state == usbip::SessionState::Preparing;
}
} // namespace qm
