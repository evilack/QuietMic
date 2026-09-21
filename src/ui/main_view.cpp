// C++ 값과 Slint 생성 API 사이의 어댑터 구현. 이미지 검증, 콜백 연결, 설정 단위 변환을 담당한다.
#include "main_view_impl.hpp"
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace qm::ui
{
namespace
{
// 도메인 파라미터와 UI 파라미터의 대응표 한 행이다. 행의 위치 대신 ID 자체를 대응시킨다.
struct ParameterBinding
{
    Parameter domain;
    generated::ParameterId control;
};
// 표를 한곳에 두어 양방향 변환이 같은 대응 관계를 사용하게 한다.
constexpr std::array parameter_bindings{
    ParameterBinding{Parameter::DryMix, generated::ParameterId::DryMix},
    ParameterBinding{Parameter::HighpassCutoff, generated::ParameterId::HighpassCutoff},
    ParameterBinding{Parameter::VoiceThreshold, generated::ParameterId::VoiceThreshold},
    ParameterBinding{Parameter::Reduction, generated::ParameterId::Reduction},
    ParameterBinding{Parameter::Hold, generated::ParameterId::Hold},
    ParameterBinding{Parameter::Release, generated::ParameterId::Release},
    ParameterBinding{Parameter::Attack, generated::ParameterId::Attack},
    ParameterBinding{Parameter::Gain, generated::ParameterId::Gain}};

// 아직 연결되지 않은 std::function을 호출하면 예외가 나므로 존재하는 동작만 실행한다.
template <typename Action, typename... Arguments>
void dispatch_action(const Action &action, Arguments... arguments)
{
    if (action)
        action(arguments...);
}

// 표준 문자열 목록을 Slint가 읽을 수 있는 공유 문자열 모델로 복사한다.
std::shared_ptr<slint::Model<slint::SharedString>> string_model(const std::vector<std::string> &strings)
{
    std::vector<slint::SharedString> rows;
    // 필요한 행 수만큼 용량을 미리 확보하여 반복 중 벡터의 재할당을 줄인다.
    rows.reserve(strings.size());
    for (const auto &string : strings)
        rows.emplace_back(std::string_view(string));
    return std::make_shared<slint::VectorModel<slint::SharedString>>(rows);
}

// 실행 파일 옆 이미지 경로를 구성하고 파일 존재와 디코딩된 크기를 검사한다.
slint::Image load_icon(const std::filesystem::path &asset_directory, const char *filename)
{
    const auto path = asset_directory / "icons" / filename;
    // Windows 경로도 UTF-8로 표현하여 Slint 경로 API와 오류 메시지에 사용한다.
    const auto utf8 = path.u8string();
    const std::string printable(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    std::error_code error;
    // error_code 형태의 검사를 사용하고, 누락 시 어떤 파일인지 포함한 자체 오류를 만든다.
    if (!std::filesystem::is_regular_file(path, error))
        throw std::runtime_error("Missing UI image asset: " + printable);
    const auto image = slint::Image::load_from_path(slint::SharedString(std::u8string_view(utf8)));
    const auto size = image.size();
    // 파일이 존재해도 유효한 이미지로 읽지 못했다면 0 크기 검사를 통해 실패를 알린다.
    if (!size.width || !size.height)
        throw std::runtime_error("Cannot load UI image asset: " + printable);
    return image;
}

// 외부 카탈로그의 의미 있는 키를 Slint의 명명된 값 구조로 옮긴다. 배열 인덱스를 쓰지 않아
// 문구를 추가하거나 순서를 바꾸어도 다른 컨트롤의 문구로 바뀌지 않는다.
generated::Translations translation_set(const localization::Catalog &catalog)
{
    generated::Translations copy;
    const auto text = [&](std::string_view key)
    {
        return slint::SharedString(catalog.text(key));
    };
    copy.app_tagline = text("app.tagline");
    copy.nav_microphone = text("nav.microphone");
    copy.nav_filter = text("nav.filter");
    copy.nav_settings = text("nav.settings");
    copy.page_microphone_subtitle = text("page.microphone.subtitle");
    copy.page_filter_subtitle = text("page.filter.subtitle");
    copy.page_settings_subtitle = text("page.settings.subtitle");
    copy.state_reconnecting = text("state.reconnecting");
    copy.state_muted = text("state.muted");
    copy.state_running = text("state.running");
    copy.state_stopped = text("state.stopped");
    copy.action_hide = text("action.hide");
    copy.action_mute = text("action.mute");
    copy.action_unmute = text("action.unmute");
    copy.microphone_input_title = text("microphone.input.title");
    copy.microphone_input_help = text("microphone.input.help");
    copy.microphone_output_title = text("microphone.output.title");
    copy.microphone_output_help = text("microphone.output.help");
    copy.microphone_output_setup = text("microphone.output.setup");
    copy.microphone_vad = text("microphone.vad");
    copy.microphone_refresh = text("microphone.refresh");
    copy.microphone_connecting = text("microphone.connecting");
    copy.microphone_stop = text("microphone.stop");
    copy.microphone_start = text("microphone.start");
    copy.filter_preset_natural = text("filter.preset.natural");
    copy.filter_preset_balanced = text("filter.preset.balanced");
    copy.filter_preset_strong = text("filter.preset.strong");
    copy.filter_preset_custom = text("filter.preset.custom");
    copy.filter_preset_save = text("filter.preset.save");
    copy.filter_denoise_title = text("filter.denoise.title");
    copy.filter_denoise_detail = text("filter.denoise.detail");
    copy.filter_expander_title = text("filter.expander.title");
    copy.filter_expander_enabled = text("filter.expander.enabled");
    copy.filter_expander_disabled = text("filter.expander.disabled");
    copy.filter_highpass_title = text("filter.highpass.title");
    copy.filter_highpass_detail = text("filter.highpass.detail");
    copy.settings_language_title = text("settings.language.title");
    copy.settings_language_detail = text("settings.language.detail");
    copy.settings_language_accessible = text("settings.language.accessible");
    copy.settings_preview_title = text("settings.preview.title");
    copy.settings_preview_detail = text("settings.preview.detail");
    copy.settings_preview_refresh = text("settings.preview.refresh");
    copy.settings_preview_volume = text("settings.preview.volume");
    copy.settings_preview_stop = text("settings.preview.stop");
    copy.settings_preview_listen = text("settings.preview.listen");
    copy.settings_preview_active = text("settings.preview.active");
    copy.settings_preview_processing = text("settings.preview.processing");
    copy.settings_preview_ready = text("settings.preview.ready");
    copy.settings_startup_title = text("settings.startup.title");
    copy.settings_autostart_title = text("settings.autostart.title");
    copy.settings_autostart_detail = text("settings.autostart.detail");
    copy.settings_hidden_title = text("settings.hidden.title");
    copy.settings_hidden_detail = text("settings.hidden.detail");
    copy.settings_auto_title = text("settings.auto.title");
    copy.settings_auto_detail = text("settings.auto.detail");
    copy.settings_virtual_title = text("settings.virtual.title");
    copy.settings_virtual_detail = text("settings.virtual.detail");
    copy.settings_virtual_preparing = text("settings.virtual.preparing");
    copy.settings_virtual_setup = text("settings.virtual.setup");
    copy.settings_transport_detail = text("settings.transport.detail");
    copy.settings_transport_source = text("settings.transport.source");
    copy.settings_privacy = text("settings.privacy");
    copy.settings_reset = text("settings.reset");
    copy.parameter_dry_label = text("parameter.dry.label");
    copy.parameter_dry_detail = text("parameter.dry.detail");
    copy.parameter_cutoff_label = text("parameter.cutoff.label");
    copy.parameter_cutoff_detail = text("parameter.cutoff.detail");
    copy.parameter_threshold_label = text("parameter.threshold.label");
    copy.parameter_threshold_detail = text("parameter.threshold.detail");
    copy.parameter_reduction_label = text("parameter.reduction.label");
    copy.parameter_reduction_detail = text("parameter.reduction.detail");
    copy.parameter_hold_label = text("parameter.hold.label");
    copy.parameter_hold_detail = text("parameter.hold.detail");
    copy.parameter_release_label = text("parameter.release.label");
    copy.parameter_release_detail = text("parameter.release.detail");
    copy.parameter_attack_label = text("parameter.attack.label");
    copy.parameter_attack_detail = text("parameter.attack.detail");
    copy.parameter_gain_label = text("parameter.gain.label");
    copy.parameter_gain_detail = text("parameter.gain.detail");
    copy.volume_title = text("volume.title");
    return copy;
}
} // namespace

// 설정 파라미터를 대응표에서 찾아 UI 식별자로 바꾼다. 없는 값은 오류로 처리한다.
generated::ParameterId MainView::Impl::to_ui(Parameter id)
{
    for (const auto &binding : parameter_bindings)
        if (binding.domain == id)
            return binding.control;
    throw std::invalid_argument("Unknown audio parameter");
}

// UI 이벤트의 파라미터 ID를 설정 계층의 ID로 되돌린다.
Parameter MainView::Impl::from_ui(generated::ParameterId id)
{
    for (const auto &binding : parameter_bindings)
        if (binding.control == id)
            return binding.domain;
    throw std::invalid_argument("Unknown UI parameter");
}

// 프리셋 이름별로 명시적인 대응을 정의한다. 어느 case도 맞지 않으면 알 수 없는 값이다.
generated::PresetId MainView::Impl::to_ui(Preset preset)
{
    switch (preset)
    {
    case Preset::Natural:
        return generated::PresetId::Natural;
    case Preset::Balanced:
        return generated::PresetId::Balanced;
    case Preset::Strong:
        return generated::PresetId::Strong;
    }
    throw std::invalid_argument("Unknown audio preset");
}

// UI가 요청한 프리셋을 도메인의 프리셋 열거형으로 바꾼다.
Preset MainView::Impl::from_ui(generated::PresetId preset)
{
    switch (preset)
    {
    case generated::PresetId::Natural:
        return Preset::Natural;
    case generated::PresetId::Balanced:
        return Preset::Balanced;
    case generated::PresetId::Strong:
        return Preset::Strong;
    }
    throw std::invalid_argument("Unknown UI preset");
}

// 각 UI 스위치의 의미를 C++ 토글 명령으로 번역한다.
Toggle MainView::Impl::from_ui(generated::ToggleId toggle)
{
    switch (toggle)
    {
    case generated::ToggleId::Mute:
        return Toggle::Mute;
    case generated::ToggleId::Denoise:
        return Toggle::Denoise;
    case generated::ToggleId::Expander:
        return Toggle::Expander;
    case generated::ToggleId::Highpass:
        return Toggle::Highpass;
    case generated::ToggleId::Autostart:
        return Toggle::Autostart;
    case generated::ToggleId::StartHidden:
        return Toggle::StartHidden;
    case generated::ToggleId::AutoProcess:
        return Toggle::AutoProcess;
    }
    throw std::invalid_argument("Unknown UI toggle");
}

// UI 링크 종류를 변환한다. 실제 주소는 컨트롤러가 결정한다.
Link MainView::Impl::from_ui(generated::ExternalLink link)
{
    switch (link)
    {
    case generated::ExternalLink::Transport:
        return Link::Transport;
    case generated::ExternalLink::Slint:
        return Link::Slint;
    }
    throw std::invalid_argument("Unknown UI link");
}

// 비공개 구현과 창을 만든 뒤 외부 PNG를 읽고, 빈 컨트롤 모델과 기본 설정을 연결한다.
MainView::MainView(const std::filesystem::path &asset_directory) : impl_(std::make_unique<Impl>())
{
    impl_->window->set_running_icon(load_icon(asset_directory, "running.png"));
    impl_->window->set_idle_icon(load_icon(asset_directory, "idle.png"));
    impl_->window->set_muted_icon(load_icon(asset_directory, "muted.png"));
    impl_->window->set_error_icon(load_icon(asset_directory, "error.png"));
    impl_->window->set_controls(impl_->controls);
    set_settings(Settings{});
}

// Impl 정의가 보이는 이 위치에서 기본 소멸자를 생성하여 창과 콜백을 함께 정리한다.
MainView::~MainView() = default;

// Slint 이벤트를 일반 C++ Actions로 연결한다. UI 코드는 오디오 엔진을 직접 호출하지 않는다.
void MainView::bind(Actions actions)
{
    impl_->actions = std::move(actions);
    // 이 포인터는 MainView가 소유하는 Impl을 가리킨다. 콜백을 가진 창도 같은 Impl에 속한다.
    const auto view = impl_.get();
    auto &window = view->window;
    window->window().on_close_requested(
        [view]
        {
            dispatch_action(view->actions.close);
            // 닫기 요청의 처리는 컨트롤러 hide에 맡긴다. Slint의 자동 창 닫기는 억제해 트레이 실행을 유지한다.
            return slint::CloseRequestResponse::KeepWindowShown;
        });
    window->on_tray(
        [view]
        {
            dispatch_action(view->actions.tray);
        });
    window->on_start_stop(
        [view]
        {
            dispatch_action(view->actions.start_stop);
        });
    window->on_speaker_test(
        [view]
        {
            dispatch_action(view->actions.speaker_test);
        });
    window->on_refresh(
        [view]
        {
            dispatch_action(view->actions.refresh);
        });
    window->on_reset(
        [view]
        {
            dispatch_action(view->actions.reset);
        });
    window->on_setup(
        [view]
        {
            dispatch_action(view->actions.setup);
        });
    window->on_select_input(
        [view](int index)
        {
            dispatch_action(view->actions.select_input, index);
        });
    window->on_select_output(
        [view](int index)
        {
            dispatch_action(view->actions.select_output, index);
        });
    window->on_select_speaker(
        [view](int index)
        {
            dispatch_action(view->actions.select_speaker, index);
        });
    window->on_select_language(
        [view](int index)
        {
            dispatch_action(view->actions.select_language, index);
        });
    // UI 볼륨은 0~100이므로 컨트롤러로 보낼 때 100으로 나누어 0~1로 맞춘다.
    window->on_speaker_volume(
        [view](float volume)
        {
            dispatch_action(view->actions.speaker_volume, volume / 100.f);
        });
    window->on_toggle(
        [view](generated::ToggleId toggle)
        {
            dispatch_action(view->actions.toggle, Impl::from_ui(toggle));
        });
    // 슬라이더 이벤트는 UI ID와 표시 단위의 숫자다. ID 변환 후 설정 메타데이터의 간격/배율을 적용한다.
    window->on_parameter(
        [view](generated::ParameterId id, float value)
        {
            const auto parameter = Impl::from_ui(id);
            const auto &definition = parameter_definition(parameter);
            // 먼저 표시 값을 step의 배수로 반올림하고 ui_scale로 나누어 엔진의 실제 단위로 되돌린다.
            const auto domain_value =
                std::round(value / definition.step) * definition.step / definition.ui_scale;
            dispatch_action(view->actions.parameter, parameter, domain_value);
        });
    window->on_preset(
        [view](generated::PresetId preset)
        {
            dispatch_action(view->actions.preset, Impl::from_ui(preset));
        });
    window->on_load_custom_preset(
        [view]
        {
            dispatch_action(view->actions.load_custom_preset);
        });
    window->on_save_custom_preset(
        [view]
        {
            dispatch_action(view->actions.save_custom_preset);
        });
    window->on_link(
        [view](generated::ExternalLink link)
        {
            dispatch_action(view->actions.link, Impl::from_ui(link));
        });
}

// 문자열 뷰를 Slint 공유 문자열로 만들어 창의 버전 속성에 복사한다.
void MainView::set_version(std::string_view version)
{
    impl_->window->set_app_version(slint::SharedString(version));
}

void MainView::set_catalog(const localization::Catalog &catalog)
{
    impl_->translations = translation_set(catalog);
    impl_->window->set_translations(impl_->translations);
    if (impl_->volume_window)
        (*impl_->volume_window)->set_translations(impl_->translations);
}

void MainView::set_languages(const std::vector<std::string> &self_names, int selected_index)
{
    impl_->window->set_languages(string_model(self_names));
    impl_->window->set_language_index(selected_index);
}

// 현재 설정으로 스위치 상태와 파라미터 모델을 갱신한다. 값의 의미/범위는 설정 정의에서 가져온다.
void MainView::set_settings(const Settings &settings)
{
    const auto &window = impl_->window;
    window->set_muted(settings.muted);
    window->set_denoise(settings.denoise);
    window->set_expander(settings.expander);
    window->set_highpass(settings.highpass);
    window->set_custom_preset_saved(settings.custom_preset_saved);
    window->set_autostart(settings.autostart);
    window->set_start_hidden(settings.start_hidden);
    window->set_auto_process(settings.auto_process);
    // 설정 정의마다 값/최솟값/최댓값을 같은 ui_scale로 확대하여 UI 슬라이더 단위를 일치시킨다.
    const auto definitions = parameter_definitions();
    std::vector<generated::Control> rows;
    rows.reserve(definitions.size());
    for (const auto &definition : definitions)
    {
        rows.push_back({parameter_value(settings, definition.id) * definition.ui_scale,
                        definition.minimum * definition.ui_scale, definition.maximum * definition.ui_scale,
                        Impl::to_ui(definition.id)});
        if (definition.id == Parameter::Gain)
        {
            // 별도 설정을 저장하지 않는다. 동일한 도메인 값으로 세 화면을 함께 갱신한다.
            window->set_gain_control(rows.back());
            if (impl_->volume_window)
                (*impl_->volume_window)->set_control(rows.back());
        }
    }
    // 행 수가 바뀔 때만 모델 전체를 교체한다. 같으면 행별 갱신으로 기존 모델을 유지하며 변경을 통지한다.
    if (impl_->controls->row_count() != rows.size())
        impl_->controls->set_vector(rows);
    else
        for (size_t index = 0; index < rows.size(); ++index)
            impl_->controls->set_row_data(index, rows[index]);
}

// 목록 모델을 먼저 보내고 그 목록에 대응하는 선택 인덱스를 설정한다.
void MainView::set_devices(const DeviceChoices &choices)
{
    const auto &window = impl_->window;
    window->set_inputs(string_model(choices.inputs));
    window->set_outputs(string_model(choices.outputs));
    window->set_speakers(string_model(choices.speakers));
    window->set_input_index(choices.input_index);
    window->set_output_index(choices.output_index);
    window->set_speaker_index(choices.speaker_index);
    window->set_output_name(slint::SharedString(std::string_view(choices.output_name)));
}

// 한 상태 묶음을 각 Slint 속성에 풀어 넣으면 조건부 문구/버튼 바인딩이 자동으로 갱신된다.
void MainView::set_processing(const ProcessingState &state)
{
    const auto &window = impl_->window;
    window->set_running(state.running);
    window->set_recovering(state.recovering);
    window->set_busy(state.busy);
    window->set_testing(state.testing);
}

// 표시용 계측값과 성능 문자열만 창 속성으로 보낸다.
void MainView::set_meter(const MeterReading &meter)
{
    const auto &window = impl_->window;
    window->set_input_level(meter.input);
    window->set_output_level(meter.output);
    window->set_vad(meter.voice_percent);
    window->set_performance(slint::SharedString(std::string_view(meter.performance)));
}

// 컨트롤러가 결정한 상태/오류 문구를 창 하단에 표시한다.
void MainView::set_status(std::string_view status)
{
    impl_->window->set_status(slint::SharedString(status));
}

// 미리듣기 시작 시 현재 UI 퍼센트를 엔진용 정규화 음량으로 읽는다.
float MainView::preview_volume() const
{
    return impl_->window->get_test_volume() / 100.f;
}

// 창을 표시한 뒤 다시 그리기를 요청해 최신 상태를 화면에 반영한다.
void MainView::show()
{
    impl_->window->show();
    impl_->window->window().request_redraw();
}

// 트레이의 볼륨 설정 메뉴에서 요청할 때만 작은 창을 생성한다.
// 다른 창으로 포커스를 옮겨도 닫지 않으며 X/Esc로 닫은 뒤 다시 열 수 있다.
void MainView::show_volume()
{
    if (!impl_->volume_window)
    {
        impl_->volume_window.emplace(generated::VolumeWindow::create());
        const auto view = impl_.get();
        auto &volume = *view->volume_window;
        volume->set_app_icon(view->window->get_running_icon());
        volume->set_translations(view->translations);
        volume->on_parameter(
            [view](generated::ParameterId id, float value)
            {
                dispatch_action(view->actions.parameter, Impl::from_ui(id), value);
            });
        volume->on_dismiss(
            [view]
            {
                (*view->volume_window)->hide();
            });
    }
    (*impl_->volume_window)->set_control(impl_->window->get_gain_control());
    (*impl_->volume_window)->show();
}
// 창을 숨길 뿐 컴포넌트나 이벤트 루프를 없애지 않는다.
void MainView::hide()
{
    impl_->window->hide();
}
} // namespace qm::ui
