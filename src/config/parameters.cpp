// Parameter 식별자를 실제 Settings 멤버로 연결하고, 수치 검증과 프리셋을 구현한다.
#include "parameters.hpp"
#include "settings.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace qm
{
namespace
{
using namespace parameter_limits;
constexpr std::array definitions{dry_mix, cutoff, threshold, reduction, hold, release, attack, gain};
// 반환형은 float 값이나 일반 주소가 아니라 Settings 안의 float 멤버를 가리키는 포인터다.
// 아래 settings.*field(id) 문법으로 선택한 설정 객체의 해당 필드를 읽거나 쓴다.
float Settings::*field(Parameter id)
{
    switch (id)
    {
    case Parameter::DryMix:
        return &Settings::dry_percent;
    case Parameter::HighpassCutoff:
        return &Settings::cutoff;
    case Parameter::VoiceThreshold:
        return &Settings::threshold;
    case Parameter::Reduction:
        return &Settings::reduction;
    case Parameter::Hold:
        return &Settings::hold_ms;
    case Parameter::Release:
        return &Settings::release_ms;
    case Parameter::Attack:
        return &Settings::attack_ms;
    case Parameter::Gain:
        return &Settings::gain_db;
    }
    // 정의되지 않은 enum 값은 임의의 필드로 대체하지 않고 호출 오류로 알린다.
    throw std::invalid_argument("Unknown audio parameter");
}
} // namespace
// 정적 배열의 뷰를 반환하므로 함수가 끝나도 반환한 범위는 유효하다.
std::span<const ParameterDescriptor> parameter_definitions()
{
    return definitions;
}
// 선택한 식별자의 범위/기본값 정의를 찾는다. 잘못된 식별자는 설정 쓰기 전에 예외가 난다.
const ParameterDescriptor &parameter_definition(Parameter id)
{
    for (const auto &definition : definitions)
        if (definition.id == id)
            return definition;
    throw std::invalid_argument("Unknown audio parameter");
}
// 같은 멤버 매핑을 읽기와 쓰기에 재사용하여 파일 저장 순서와 실제 필드 연결을 일치시킨다.
float parameter_value(const Settings &settings, Parameter id)
{
    return settings.*field(id);
}
void set_parameter(Settings &settings, Parameter id, float value)
{
    // 유한한 값은 범위 안으로 잘라내고 유한하지 않은 값은 기본값으로 복구한다.
    // 설정 파일이나 UI에서 온 값이 DSP 계산에 NaN을 전파하지 않도록 하는 경계다.
    const auto &definition = parameter_definition(id);
    settings.*field(id) = std::isfinite(value) ? std::clamp(value, definition.minimum, definition.maximum)
                                               : definition.default_value;
}
void sanitize_parameters(Settings &settings)
{
    // 현재 값을 다시 공통 쓰기 경로에 통과시켜 모든 수치 필드를 한 번에 정리한다.
    for (const auto &definition : definitions)
        set_parameter(settings, definition.id, parameter_value(settings, definition.id));
}
void apply_preset(Settings &settings, Preset preset)
{
    struct NoiseValues
    {
        float mix, cutoff, threshold, reduction, hold, release, attack;
    } values;
    // 프리셋은 원음 혼합, 하이패스, 음성 판정, 감쇠량, 시간 응답의 묶음이다.
    // Balanced는 별도 숫자를 중복하지 않고 파라미터 기본 정의에서 값을 가져온다.
    switch (preset)
    {
    case Preset::Natural:
        values = {15, 70, .2f, 9, 260, 300, 5};
        break;
    case Preset::Strong:
        values = {0, 100, .45f, 30, 120, 160, 5};
        break;
    case Preset::Balanced:
        values = {dry_mix.default_value,   cutoff.default_value, threshold.default_value,
                  reduction.default_value, hold.default_value,   release.default_value,
                  attack.default_value};
        break;
    default:
        throw std::invalid_argument("Unknown noise preset");
    }
    // 필요한 필터를 켜고 각 값을 공통 검증 함수를 거쳐 적용한다. denoise 스위치와
    // gain_db는 여기서 대입하지 않으므로 기존 사용자 선택을 유지한다.
    settings.highpass = settings.expander = true;
    set_parameter(settings, Parameter::DryMix, values.mix);
    set_parameter(settings, Parameter::HighpassCutoff, values.cutoff);
    set_parameter(settings, Parameter::VoiceThreshold, values.threshold);
    set_parameter(settings, Parameter::Reduction, values.reduction);
    set_parameter(settings, Parameter::Hold, values.hold);
    set_parameter(settings, Parameter::Release, values.release);
    set_parameter(settings, Parameter::Attack, values.attack);
}

void sanitize_filter_preset(FilterPresetSettings &preset)
{
    // 기존 Settings 검증 코드를 재사용하기 위해 필터 값만 임시 설정으로 옮긴다.
    // 출력 이득은 사용자 지정 프리셋 대상이 아니므로 복사하지 않는다.
    Settings values;
    values.dry_percent = preset.dry_percent;
    values.cutoff = preset.cutoff;
    values.threshold = preset.threshold;
    values.reduction = preset.reduction;
    values.hold_ms = preset.hold_ms;
    values.release_ms = preset.release_ms;
    values.attack_ms = preset.attack_ms;
    sanitize_parameters(values);
    preset.dry_percent = values.dry_percent;
    preset.cutoff = values.cutoff;
    preset.threshold = values.threshold;
    preset.reduction = values.reduction;
    preset.hold_ms = values.hold_ms;
    preset.release_ms = values.release_ms;
    preset.attack_ms = values.attack_ms;
}

void save_custom_preset(Settings &settings)
{
    // 화면에 표시된 현재 필터 상태를 하나의 값 객체로 복사한 뒤 범위를 정리한다.
    settings.custom_preset = {settings.denoise,      settings.expander, settings.highpass,
                              settings.dry_percent,  settings.cutoff,   settings.threshold,
                              settings.reduction,    settings.hold_ms,  settings.release_ms,
                              settings.attack_ms};
    sanitize_filter_preset(settings.custom_preset);
    settings.custom_preset_saved = true;
}

bool apply_custom_preset(Settings &settings)
{
    if (!settings.custom_preset_saved)
        return false;
    // 파일이나 코드에서 만든 값도 적용 직전에 다시 검증한다. 이후에는 필터 관련 멤버만
    // 덮어쓰므로 장치 선택, 음소거, 앱 옵션과 출력 이득은 그대로 유지된다.
    auto preset = settings.custom_preset;
    sanitize_filter_preset(preset);
    settings.denoise = preset.denoise;
    settings.expander = preset.expander;
    settings.highpass = preset.highpass;
    settings.dry_percent = preset.dry_percent;
    settings.cutoff = preset.cutoff;
    settings.threshold = preset.threshold;
    settings.reduction = preset.reduction;
    settings.hold_ms = preset.hold_ms;
    settings.release_ms = preset.release_ms;
    settings.attack_ms = preset.attack_ms;
    return true;
}
} // namespace qm
