// 수치형 오디오 설정의 이름, 범위, 기본값을 한곳에서 정의한다.
// UI, 파일 저장/읽기, 프리셋이 같은 정의를 사용해 서로 다른 제한을 적용하지 않게 한다.
#pragma once
#include <span>
#include <string_view>

namespace qm
{
struct Settings;
struct FilterPresetSettings;
enum class Parameter
{
    DryMix,
    HighpassCutoff,
    VoiceThreshold,
    Reduction,
    Hold,
    Release,
    Attack,
    Gain
};
enum class Preset
{
    Natural,
    Balanced,
    Strong
};
// key는 설정 파일의 키이고 minimum/maximum/default_value는 DSP가 쓰는 실제 단위다.
// ui_scale은 UI 표시 배율이다. 예를 들어 확률 0.3에 100을 곱하면 화면에서는 30으로 다룬다.
struct ParameterDescriptor
{
    Parameter id;
    std::string_view key;
    float minimum, maximum, default_value, ui_scale = 1, step = 1;
};
// 단위: dry_mix는 원음 비율(%), cutoff는 Hz, threshold는 0~1 확률,
// reduction/gain은 dB, hold/release/attack은 ms이다.
namespace parameter_limits
{
inline constexpr ParameterDescriptor dry_mix{Parameter::DryMix, "dry_percent", 0, 50, 0};
inline constexpr ParameterDescriptor cutoff{Parameter::HighpassCutoff, "cutoff", 60, 160, 80};
inline constexpr ParameterDescriptor threshold{Parameter::VoiceThreshold, "threshold", .1f, .8f, .3f, 100};
inline constexpr ParameterDescriptor reduction{Parameter::Reduction, "reduction", 0, 36, 18};
inline constexpr ParameterDescriptor hold{Parameter::Hold, "hold_ms", 0, 600, 180};
inline constexpr ParameterDescriptor release{Parameter::Release, "release_ms", 50, 600, 200};
inline constexpr ParameterDescriptor attack{Parameter::Attack, "attack_ms", 0, 50, 10};
inline constexpr ParameterDescriptor gain{Parameter::Gain, "gain_db", -15, 15, 0};
} // namespace parameter_limits
// 반환 span은 정적 정의 배열을 바라보는 읽기 전용 뷰다. 호출자는 복사나 해제 없이 순회한다.
std::span<const ParameterDescriptor> parameter_definitions();
const ParameterDescriptor &parameter_definition(Parameter);
float parameter_value(const Settings &, Parameter);
// UI 배율을 제거한 실제 단위 값을 받는다. 허용 범위를 벗어나면 경계값으로 제한하고
// NaN·무한대는 기본값으로 바꾼다. 개별 설정과 전체 정리에 동일한 규칙을 사용한다.
void set_parameter(Settings &, Parameter, float domain_value);
void sanitize_parameters(Settings &);
// 잡음 처리 관련 수치와 highpass/expander만 조정한다. 장치 선택, 음소거,
// 사용자 출력 이득 등은 호출 전 값을 유지한다.
void apply_preset(Settings &, Preset);
// 현재 필터 값만 사용자 지정 슬롯에 저장하고, 저장된 슬롯만 다시 적용한다.
// apply는 저장된 슬롯이 없으면 false를 반환하며 Settings를 바꾸지 않는다.
void save_custom_preset(Settings &);
bool apply_custom_preset(Settings &);
void sanitize_filter_preset(FilterPresetSettings &);
} // namespace qm
