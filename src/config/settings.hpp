// 앱의 저장 가능한 설정 값과 파일 입출력 계약이다. 기본 생성만으로 안전한 기본 설정을
// 얻으며, 오디오 엔진에는 이 구조체의 복사본을 전달한다.
#pragma once
#include "parameters.hpp"
#include <filesystem>
#include <string>
namespace qm
{
// 사용자 지정 프리셋은 필터 처리에 속하는 값만 보관한다. 장치, 음소거, 출력 이득과
// 앱 시작 옵션은 프리셋을 불러올 때 유지되어야 하므로 이 구조체에 넣지 않는다.
struct FilterPresetSettings
{
    bool denoise = true, expander = true, highpass = true;
    float dry_percent = parameter_limits::dry_mix.default_value;
    float cutoff = parameter_limits::cutoff.default_value;
    float threshold = parameter_limits::threshold.default_value;
    float reduction = parameter_limits::reduction.default_value;
    float hold_ms = parameter_limits::hold.default_value;
    float release_ms = parameter_limits::release.default_value;
    float attack_ms = parameter_limits::attack.default_value;
};

struct Settings
{
    // 첫 묶음은 처리 기능/음소거, 다음 묶음은 앱 시작 동작이다. 음소거도 저장 대상이다.
    bool denoise = true, expander = true, highpass = true, muted = false;
    bool autostart = false, start_hidden = true, auto_process = false;
    // 수치 기본값은 parameters.hpp에서 가져온다. Hz/dB/ms/확률 단위가 섞여 있으므로
    // UI 표기값을 그대로 넣지 말고 각 파라미터의 실제 단위를 사용한다.
    float dry_percent = parameter_limits::dry_mix.default_value;
    float cutoff = parameter_limits::cutoff.default_value;
    float threshold = parameter_limits::threshold.default_value;
    float reduction = parameter_limits::reduction.default_value;
    float hold_ms = parameter_limits::hold.default_value;
    float release_ms = parameter_limits::release.default_value;
    float attack_ms = parameter_limits::attack.default_value;
    float gain_db = parameter_limits::gain.default_value;
    // 저장 버튼을 누르기 전에는 사용자 지정 불러오기를 비활성화한다. 값 자체는 항상
    // 직렬화하여 파일 형식을 고정하고, saved 플래그만 실제 사용 가능 여부를 나타낸다.
    bool custom_preset_saved = false;
    FilterPresetSettings custom_preset;
    // 사람에게 보이는 이름이 아니라 Windows 엔드포인트 ID를 저장해 같은 장치를 다시 찾는다.
    std::string input_id, output_id;
    // 앱 표시 언어는 고정된 BCP 47 부분집합 코드다. 새 설치와 버전 1 마이그레이션의 기본은 영어다.
    std::string language = "en";
};
// 수치 범위와 장치 ID 길이를 정리한다. load는 파일이 불완전하면 전체 기본 설정을 반환하고,
// save는 정리한 복사본을 임시 파일에 쓴 뒤 기존 파일을 교체한다.
void sanitize(Settings &);
Settings load_settings(const std::filesystem::path &);
void save_settings(const std::filesystem::path &, const Settings &);
} // namespace qm
