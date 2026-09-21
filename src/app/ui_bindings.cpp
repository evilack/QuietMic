// 화면/트레이 명령을 컨트롤러 동작에 연결한다. 설정 변경은 화면 반영과 엔진 전달/저장 예약으로 이어진다.
#include "app_controller.hpp"
#include <array>
#include <cmath>
#include <stdexcept>

namespace qm
{
// 현재 설정을 화면에 보내고 엔진 상태 표시도 함께 갱신한다.
void AppController::sync_controls()
{
    ui_.set_settings(settings_);
    sync_status();
}
// 식별자로 해당 설정 하나를 바꾼 뒤 화면과 실제 처리 설정을 맞춘다.
void AppController::change_parameter(Parameter id, float value)
{
    set_parameter(settings_, id, value);
    sync_controls();
    commit();
}
// 프리셋이 정의한 필터 값을 묶어서 적용하고, 표시와 저장 예약을 갱신한다.
void AppController::select_preset(Preset id)
{
    apply_preset(settings_, id);
    sync_controls();
    commit();
}
// 사용자 지정 슬롯은 현재 설정 파일 안에 저장한다. 불러오기는 슬롯이 있을 때만 필터 값을
// 바꾸며, 저장과 불러오기 모두 즉시 화면/오디오 엔진/지연 저장 흐름에 반영한다.
void AppController::load_custom_preset()
{
    if (!apply_custom_preset(settings_))
        return;
    sync_controls();
    commit();
}
void AppController::save_custom_preset()
{
    qm::save_custom_preset(settings_);
    sync_controls();
    commit();
}
// UI가 보낸 의미 있는 토글 ID에 따라 해당 불리언 설정만 뒤집는다.
void AppController::toggle(ui::Toggle id)
{
    switch (id)
    {
    case ui::Toggle::Mute:
        settings_.muted = !settings_.muted;
        break;
    case ui::Toggle::Denoise:
        settings_.denoise = !settings_.denoise;
        break;
    case ui::Toggle::Expander:
        settings_.expander = !settings_.expander;
        break;
    case ui::Toggle::Highpass:
        settings_.highpass = !settings_.highpass;
        break;
    case ui::Toggle::StartHidden:
        settings_.start_hidden = !settings_.start_hidden;
        break;
    case ui::Toggle::AutoProcess:
        settings_.auto_process = !settings_.auto_process;
        break;
    case ui::Toggle::Autostart:
        // Windows 시작 항목 변경이 성공한 뒤 설정을 뒤집는다. OS 변경 실패 시 메모리 상태를 성공으로 표시하지 않는다.
        platform::set_startup(!settings_.autostart, paths_.executable);
        settings_.autostart = !settings_.autostart;
        break;
    default:
        throw std::invalid_argument("Unknown UI toggle");
    }
    sync_controls();
    commit();
}
// Windows 메뉴 번호 대신 TrayCommand를 받아 화면 버튼과 같은 처리 함수를 호출한다.
void AppController::tray_command(TrayCommand command)
{
    switch (command)
    {
    case TrayCommand::ShowVolume:
        ui_.show_volume();
        break;
    case TrayCommand::Show:
        show();
        break;
    case TrayCommand::ToggleMute:
        toggle(ui::Toggle::Mute);
        break;
    case TrayCommand::ToggleProcessing:
        toggle_processing();
        break;
    case TrayCommand::Quit:
        request_quit();
        break;
    case TrayCommand::ToggleDenoise:
        toggle(ui::Toggle::Denoise);
        break;
    case TrayCommand::ToggleStartup:
        toggle(ui::Toggle::Autostart);
        break;
    }
}
// 각 UI 이벤트에 guarded 콜백을 연결한다. 종료된 컨트롤러 접근과 이벤트 밖으로 나가는 표준 예외를 막는다.
void AppController::bind_ui()
{
    ui::Actions actions;
    // 창 닫기와 트레이 숨기기는 모두 hide에 연결한다. 앱 종료는 트레이의 종료 명령이 담당한다.
    actions.close = guarded(&AppController::hide);
    actions.tray = guarded(&AppController::hide);
    actions.start_stop = guarded(&AppController::toggle_processing);
    actions.speaker_test = guarded(&AppController::toggle_preview);
    // 미리듣기 음량은 엔진에 즉시 전달한다. 어댑터가 이미 0~1 값으로 바꿔 준다.
    actions.speaker_volume = guarded(
        [](AppController &app, float value)
        {
            app.audio_.set_monitor_volume(value);
        });
    actions.refresh = guarded(
        [](AppController &app)
        {
            app.refresh();
            app.set_status("status.refreshed");
        });
    // UI의 비활성 표시와 별개로 엔진/설치 상태를 다시 검사해 처리 중 장치 변경을 차단한다.
    actions.select_input = guarded(
        [](AppController &app, int index)
        {
            const auto state = app.audio_.snapshot();
            if (state.running || state.starting || state.recovering || app.preparing_output())
                return;
            // 받은 인덱스를 장치 ID로 바꾸고 변경을 엔진에 전달하면서 저장을 예약한다.
            app.devices_.select_input(index, app.settings_);
            app.commit();
        });
    // 출력 변경 뒤에는 다시 열거하여 출력 이름과 선택 인덱스 표시도 맞춘다.
    actions.select_output = guarded(
        [](AppController &app, int index)
        {
            const auto state = app.audio_.snapshot();
            if (state.running || state.starting || state.recovering || app.preparing_output())
                return;
            app.devices_.select_output(index, app.settings_);
            app.refresh();
            app.commit();
        });
    // 스피커 선택은 미리듣기 전용이며 설정 파일에 저장하는 input/output 선택과 분리된다.
    actions.select_speaker = guarded(
        [](AppController &app, int index)
        {
            const auto state = app.audio_.snapshot();
            if (state.running || state.starting || state.recovering || app.preparing_output())
                return;
            app.devices_.select_speaker(index);
        });
    actions.select_language = guarded(&AppController::change_language);
    actions.toggle = guarded(&AppController::toggle);
    actions.parameter = guarded(&AppController::change_parameter);
    actions.preset = guarded(&AppController::select_preset);
    actions.load_custom_preset = guarded(&AppController::load_custom_preset);
    actions.save_custom_preset = guarded(&AppController::save_custom_preset);
    // 초기화 버튼은 균형 프리셋 적용으로 연결된다.
    actions.reset = guarded(
        [](AppController &app)
        {
            app.select_preset(Preset::Balanced);
        });
    actions.setup = guarded(&AppController::start_setup);
    // UI 열거형을 미리 정해진 주소로 바꾸어 기본 브라우저로 연다.
    actions.link = guarded(
        [](AppController &, ui::Link link)
        {
            switch (link)
            {
            case ui::Link::Transport:
                platform::open_url(L"https://github.com/vadimgrn/usbip-win2");
                break;
            case ui::Link::Slint:
                platform::open_url(L"https://slint.dev");
                break;
            }
        });
    // 완성한 콜백 묶음의 소유권을 뷰로 이동한다. 이후 UI가 필요할 때 이 동작들을 호출한다.
    ui_.bind(std::move(actions));
}
} // namespace qm
