// 화면, 오디오, 장치 목록, 트레이, 설치 프로세스를 조율하는 컨트롤러의 선언이다.
#pragma once
#include "main_view.hpp"
#include <slint.h>
#include "audio.hpp"
#include "device_catalog.hpp"
#include "device_watcher.hpp"
#include "tray.hpp"
#include "windows_services.hpp"
#include "usbip_session.hpp"
#include "catalog.hpp"
#include <memory>
#include <functional>

namespace qm
{
// enable_shared_from_this는 기존 shared_ptr 소유권으로부터 weak_ptr를 얻도록 한다.
// 콜백이 컨트롤러를 영구 소유하지 않도록 create에서 공유 소유권을 만든 뒤 초기화한다.
class AppController : public std::enable_shared_from_this<AppController>
{
    // 경로/설정은 컨트롤러가 보관하고 UI에는 표시용 값만 전달한다. 멤버 자원은 컨트롤러와 함께 정리된다.
    platform::ApplicationPaths paths_;
    ui::MainView ui_;
    Settings settings_;
    localization::Catalog catalog_;
    AudioEngine audio_;
    DeviceCatalog devices_;
    std::unique_ptr<DeviceWatcher> device_watcher_;
    std::unique_ptr<Tray> tray_;
    usbip::UsbMicSession virtual_microphone_;
    // 순서대로 미터 갱신, 연속 입력 후 지연 저장용 이벤트 루프 타이머다.
    slint::Timer meter_, save_timer_, device_refresh_timer_;
    // visible은 표시 여부, closing은 종료가 시작된 뒤 도착한 늦은 콜백을 차단한다.
    bool visible_ = false, closing_ = false;
    // 미리듣기 여부와 설치 후 원래의 처리 시작 요청을 이어갈지를 각각 기억한다.
    bool previewing_ = false, resume_after_setup_ = false;
    int exit_code_ = 0;

    // 생성자는 멤버를 준비하고 initialize가 공유 소유권 기반 콜백을 연결하므로 외부 생성을 제한한다.
    explicit AppController(platform::ApplicationPaths);
    void initialize();
    void bind_ui();
    void commit();
    void save();
    void sync_controls();
    void sync_status();
    void refresh();
    void publish_devices();
    void publish_language();
    void change_language(int index);
    void schedule_device_refresh();
    void show();
    void hide();
    void update_meter();
    void toggle_processing();
    void toggle_preview();
    void toggle(ui::Toggle);
    void change_parameter(Parameter, float);
    void select_preset(Preset);
    void load_custom_preset();
    void save_custom_preset();
    void tray_command(TrayCommand);
    void start_setup();
    void session_changed();
    bool preparing_output() const;
    void audio_changed();
    void request_quit(int exit_code = 0);
    void shutdown();
    void set_status(std::string_view catalog_key);
    // 동작 하나를 실행하고 표준 예외를 상태 문구로 바꾸어 이벤트 처리 중 오류를 보여 준다.
    template <class Action> void guard(Action &&action)
    {
        try
        {
            // invoke로 호출 객체를 실행한다. forward는 전달받은 객체의 값 범주를 유지한다.
            std::invoke(std::forward<Action>(action));
        }
        catch (const std::exception &error)
        {
            ui_.set_status(std::string(catalog_.text("error.technical-detail")) + " " + error.what());
        }
    }
    // UI 스레드에서 사용할 콜백을 만든다. 멤버 함수 포인터와 람다를 같은 방식으로 연결할 수 있다.
    template <class Action> auto guarded(Action action)
    {
        // weak_ptr는 수명을 연장하지 않는다. lock에 성공하고 종료 중이 아닐 때만 임시 소유권으로 안전하게 실행한다.
        return [weak = weak_from_this(), action](auto... args)
        {
            if (auto self = weak.lock(); self && !self->closing_)
                self->guard(
                    [&]
                    {
                        std::invoke(action, *self, args...);
                    });
        };
    }
    // 오디오/트레이 스레드의 알림을 UI 스레드로 옮긴다. 실제 UI 속성 갱신은 예약된 동작에서 수행한다.
    template <class Action> void post(Action action)
    {
        // 예약과 실행 사이에 객체가 소멸할 수 있어 실행 직전에도 약한 참조와 종료 상태를 확인한다.
        slint::invoke_from_event_loop(
            [weak = weak_from_this(), action]
            {
                if (auto self = weak.lock(); self && !self->closing_)
                    self->guard(
                        [&]
                        {
                            std::invoke(action, *self);
                        });
            });
    }

  public:
    // 반환된 shared_ptr가 앱 실행 동안 컨트롤러의 수명을 유지한다.
    static std::shared_ptr<AppController> create(platform::ApplicationPaths);
    ~AppController();
    int run(bool login);
};
} // namespace qm
