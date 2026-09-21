// 텍스트 설정 파일의 저장/복구를 담당한다. 헤더, 각 키의 단일 값, END 표시를 확인하여
// 쓰다가 끊긴 파일의 일부만 설정으로 채택하지 않도록 한다.
#include "settings.hpp"
#include "catalog.hpp"
#include <array>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <windows.h>

namespace qm
{
namespace
{
// 키 문자열과 Settings의 bool 멤버를 연결한다. 동일 표를 읽기와 쓰기에 사용하므로
// 두 경로가 같은 항목 집합을 처리한다.
struct BooleanField
{
    const char *key;
    bool Settings::*member;
};
constexpr std::array booleans{BooleanField{"denoise", &Settings::denoise},
                              BooleanField{"expander", &Settings::expander},
                              BooleanField{"highpass", &Settings::highpass},
                              BooleanField{"muted", &Settings::muted},
                              BooleanField{"autostart", &Settings::autostart},
                              BooleanField{"start_hidden", &Settings::start_hidden},
                              BooleanField{"auto_process", &Settings::auto_process}};
struct PresetBooleanField
{
    const char *key;
    bool FilterPresetSettings::*member;
};
constexpr std::array preset_booleans{
    PresetBooleanField{"custom_denoise", &FilterPresetSettings::denoise},
    PresetBooleanField{"custom_expander", &FilterPresetSettings::expander},
    PresetBooleanField{"custom_highpass", &FilterPresetSettings::highpass}};
struct PresetNumberField
{
    const char *key;
    float FilterPresetSettings::*member;
};
constexpr std::array preset_numbers{
    PresetNumberField{"custom_dry_percent", &FilterPresetSettings::dry_percent},
    PresetNumberField{"custom_cutoff", &FilterPresetSettings::cutoff},
    PresetNumberField{"custom_threshold", &FilterPresetSettings::threshold},
    PresetNumberField{"custom_reduction", &FilterPresetSettings::reduction},
    PresetNumberField{"custom_hold_ms", &FilterPresetSettings::hold_ms},
    PresetNumberField{"custom_release_ms", &FilterPresetSettings::release_ms},
    PresetNumberField{"custom_attack_ms", &FilterPresetSettings::attack_ms}};
constexpr size_t maximum_endpoint_id_length = 2048;
constexpr size_t maximum_language_length = 16;
constexpr const char *version_one_header = "QUIETMIC 1";
constexpr const char *version_two_header = "QUIETMIC 2";
constexpr const char *version_three_header = "QUIETMIC 3";
} // namespace
// 숫자는 공통 파라미터 규칙으로 정리하고 비정상적으로 긴 장치 ID는 비운다.
// ID 문자열을 잘라서 존재하지 않는 다른 ID로 만드는 대신 다시 선택할 수 있게 한다.
void sanitize(Settings &settings)
{
    sanitize_parameters(settings);
    sanitize_filter_preset(settings.custom_preset);
    if (settings.input_id.size() > maximum_endpoint_id_length)
        settings.input_id.clear();
    if (settings.output_id.size() > maximum_endpoint_id_length)
        settings.output_id.clear();
    if (settings.language.size() > maximum_language_length ||
        !localization::is_supported_language(settings.language))
        settings.language = localization::default_language();
}
Settings load_settings(const std::filesystem::path &path)
{
    // 먼저 기본 설정을 준비한다. 파일이 없거나 헤더/버전이 맞지 않으면 예외 대신
    // 그 기본값을 반환하며, 정상 헤더를 읽었을 때만 필드 해석을 진행한다.
    Settings settings;
    std::ifstream file(path);
    std::string line;
    if (!std::getline(file, line) ||
        (line != version_one_header && line != version_two_header && line != version_three_header))
        return settings;
    const bool has_language = line == version_two_header || line == version_three_header;
    const bool has_custom_preset = line == version_three_header;
    // valid는 알려진 키의 값 오류/중복 여부, complete는 END 도달 여부다.
    // seen은 성공 여부와 별개로 알려진 키의 중복과 누락을 검사하기 위한 집합이다.
    bool valid = true, complete = false;
    std::set<std::string> seen;
    while (std::getline(file, line))
    {
        std::istringstream row(line);
        std::string key;
        if (!(row >> key))
            continue;
        // END를 만나면 파일 한 건이 끝난 것으로 본다. 빈 줄은 건너뛰고, 모르는 키는
        // 아래 검사 대상에 넣지 않아 알려진 설정 항목만 검증한다.
        if (key == "END")
        {
            complete = true;
            break;
        }
        bool known = false;
        // 먼저 bool 표, 다음 숫자 정의 표에서 키를 찾는다. 숫자는 읽기에 성공했을 때만
        // set_parameter를 통과시켜 저장된 값도 허용 범위 안으로 들어오게 한다.
        for (const auto &field : booleans)
        {
            if (key == field.key)
            {
                known = true;
                row >> settings.*field.member;
                break;
            }
        }
        for (const auto &definition : parameter_definitions())
        {
            if (key == definition.key)
            {
                known = true;
                float value = 0;
                if (row >> value)
                    set_parameter(settings, definition.id, value);
                break;
            }
        }
        // 버전 3은 한 개의 사용자 지정 필터 슬롯을 고정된 키 집합으로 저장한다.
        // 이전 버전에서는 이 키를 요구하지 않아 기존 설정 파일을 그대로 마이그레이션한다.
        if (has_custom_preset && key == "custom_preset_saved")
        {
            known = true;
            row >> settings.custom_preset_saved;
        }
        if (has_custom_preset)
        {
            for (const auto &field : preset_booleans)
            {
                if (key == field.key)
                {
                    known = true;
                    row >> (settings.custom_preset.*field.member);
                    break;
                }
            }
            for (const auto &field : preset_numbers)
            {
                if (key == field.key)
                {
                    known = true;
                    row >> (settings.custom_preset.*field.member);
                    break;
                }
            }
        }
        // 장치 ID는 따옴표 형식으로 읽어 공백과 이스케이프 문자가 포함된 문자열을 보존한다.
        if (key == "input_id")
        {
            known = true;
            row >> std::quoted(settings.input_id);
        }
        if (key == "output_id")
        {
            known = true;
            row >> std::quoted(settings.output_id);
        }
        // language는 버전 2에서만 필수다. 버전 1에는 필드가 없으며 영어로 마이그레이션한다.
        if (has_language && key == "language")
        {
            known = true;
            row >> std::quoted(settings.language);
        }
        // 알려진 키는 값 읽기 실패, 같은 키의 재등장, 값 뒤의 불필요한 토큰을 오류로 기록한다.
        // 그 자리에서 일부 설정만 반환하지 않고 파일 전체의 유효 여부를 마지막에 판정한다.
        if (known)
        {
            if (row.fail() || !seen.insert(key).second)
                valid = false;
            std::string trailing;
            if (row >> trailing)
                valid = false;
        }
    }
    // END와 모든 필수 키가 정확히 한 번씩 있어야 채택한다. 불완전하거나 중복된 파일은
    // 부분 적용하지 않고 새 Settings의 기본값으로 한꺼번에 복구한다.
    const size_t custom_fields =
        has_custom_preset ? 1 + preset_booleans.size() + preset_numbers.size() : 0;
    const size_t required_fields =
        booleans.size() + parameter_definitions().size() + 2 + (has_language ? 1 : 0) + custom_fields;
    if (!valid || !complete || seen.size() != required_fields)
        return Settings{};
    if (!has_language)
        settings.language = localization::default_language();
    sanitize(settings);
    return settings;
}
void save_settings(const std::filesystem::path &path, const Settings &value)
{
    // 호출자가 가진 설정은 바꾸지 않고 복사본만 정리해 저장한다. 필요한 상위 폴더를 만든 뒤
    // 같은 위치의 .tmp 파일에 전체 내용을 먼저 작성한다.
    Settings settings = value;
    sanitize(settings);
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += L".tmp";
    std::ofstream file(temporary, std::ios::trunc);
    if (!file)
        throw std::runtime_error("Cannot write settings");
    // 읽기와 같은 표를 순회해 모든 필수 항목을 기록한다. 마지막 END는 내용이 끝까지
    // 작성되었음을 읽기 쪽에서 판단하는 표시이며 ID는 quoted로 왕복 가능하게 저장한다.
    file << version_three_header << '\n';
    for (const auto &field : booleans)
        file << field.key << ' ' << settings.*field.member << '\n';
    for (const auto &definition : parameter_definitions())
        file << definition.key << ' ' << parameter_value(settings, definition.id) << '\n';
    file << "custom_preset_saved " << settings.custom_preset_saved << '\n';
    for (const auto &field : preset_booleans)
        file << field.key << ' ' << (settings.custom_preset.*field.member) << '\n';
    for (const auto &field : preset_numbers)
        file << field.key << ' ' << (settings.custom_preset.*field.member) << '\n';
    file << "input_id " << std::quoted(settings.input_id) << "\noutput_id " << std::quoted(settings.output_id)
         << "\nlanguage " << std::quoted(settings.language) << "\nEND\n";
    // 닫는 과정의 쓰기 실패까지 확인한 후 최종 경로를 교체한다. 완성된 임시 파일을
    // 같은 디렉터리에서 이동하므로 기존 설정을 직접 잘라 쓴 상태로 남기지 않는다.
    file.close();
    if (!file)
        throw std::runtime_error("Settings write failed");
    // 기존 파일을 교체하고 쓰기 완료를 요청하는 Windows 플래그를 사용한다.
    // 교체 실패는 호출자가 알 수 있도록 예외로 전달한다.
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace settings");
}
} // namespace qm
