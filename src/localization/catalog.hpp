// QuietMic의 외부 UTF-8 번역 파일을 읽는 독립 모듈이다. UI나 Win32 타입을 노출하지 않아
// 설정 검사, 트레이, Slint 어댑터가 같은 번역 원본을 사용할 수 있다.
#pragma once

#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qm::localization
{
struct LanguageInfo
{
    std::string_view code;
    std::string_view self_name;
};

std::string_view default_language();
std::span<const LanguageInfo> supported_languages();
bool is_supported_language(std::string_view code);

class Catalog final
{
  public:
    // 영어 파일을 기준으로 선택 언어를 덮어쓴다. 선택 언어가 손상돼도 영어 원본은 유지한다.
    static Catalog load(const std::filesystem::path &i18n_directory, std::string_view locale);

    std::string_view locale() const;
    std::string_view text(std::string_view key) const;
    const std::vector<std::string> &warnings() const;

  private:
    std::string locale_;
    std::map<std::string, std::string, std::less<>> values_;
    std::vector<std::string> warnings_;
};
} // namespace qm::localization
