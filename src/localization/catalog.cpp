// .lang 파일의 검증과 영어 대체 규칙을 구현한다. 번역 파일 경로는 고정 로케일 표에서만
// 만들기 때문에 저장된 설정 문자열이 디렉터리 밖의 파일을 가리킬 수 없다.
#include "catalog.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace qm::localization
{
namespace
{
constexpr std::array languages{LanguageInfo{"en", "English"},       LanguageInfo{"ko", "한국어"},
                               LanguageInfo{"ja", "日本語"},        LanguageInfo{"zh-Hans", "简体中文"},
                               LanguageInfo{"zh-Hant", "繁體中文"}, LanguageInfo{"es", "Español"},
                               LanguageInfo{"fr", "Français"},      LanguageInfo{"de", "Deutsch"}};

struct ParsedFile
{
    std::map<std::string, std::string, std::less<>> values;
    std::vector<std::string> errors;
};

bool valid_utf8(std::string_view bytes)
{
    size_t index = 0;
    while (index < bytes.size())
    {
        const auto first = static_cast<unsigned char>(bytes[index]);
        if (first <= 0x7f)
        {
            ++index;
            continue;
        }

        size_t continuation = 0;
        unsigned codepoint = 0;
        if (first >= 0xc2 && first <= 0xdf)
        {
            continuation = 1;
            codepoint = first & 0x1f;
        }
        else if (first >= 0xe0 && first <= 0xef)
        {
            continuation = 2;
            codepoint = first & 0x0f;
        }
        else if (first >= 0xf0 && first <= 0xf4)
        {
            continuation = 3;
            codepoint = first & 0x07;
        }
        else
            return false;

        if (index + continuation >= bytes.size())
            return false;
        for (size_t offset = 1; offset <= continuation; ++offset)
        {
            const auto next = static_cast<unsigned char>(bytes[index + offset]);
            if ((next & 0xc0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) || (continuation == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
        index += continuation + 1;
    }
    return true;
}

std::string decode_value(std::string_view encoded, bool &valid)
{
    std::string value;
    value.reserve(encoded.size());
    for (size_t index = 0; index < encoded.size(); ++index)
    {
        if (encoded[index] != '\\')
        {
            value.push_back(encoded[index]);
            continue;
        }
        if (++index == encoded.size())
        {
            valid = false;
            return {};
        }
        switch (encoded[index])
        {
        case '\\':
            value.push_back('\\');
            break;
        case '=':
            value.push_back('=');
            break;
        case 'n':
            value.push_back('\n');
            break;
        default:
            valid = false;
            return {};
        }
    }
    return value;
}

ParsedFile parse_file(const std::filesystem::path &path)
{
    ParsedFile result;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        result.errors.emplace_back("Cannot open " + path.string());
        return result;
    }

    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (bytes.starts_with("\xEF\xBB\xBF"))
        bytes.erase(0, 3);
    if (!valid_utf8(bytes))
    {
        result.errors.emplace_back("Invalid UTF-8 in " + path.string());
        return result;
    }

    size_t line_number = 0;
    for (size_t start = 0; start <= bytes.size();)
    {
        const auto end = bytes.find('\n', start);
        std::string_view line(bytes.data() + start, (end == std::string::npos ? bytes.size() : end) - start);
        ++line_number;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty() && line.front() != '#')
        {
            size_t separator = std::string_view::npos;
            bool escaped = false;
            for (size_t index = 0; index < line.size(); ++index)
            {
                if (!escaped && line[index] == '=')
                {
                    separator = index;
                    break;
                }
                if (line[index] == '\\' && !escaped)
                    escaped = true;
                else
                    escaped = false;
            }
            const auto prefix = path.filename().string() + ':' + std::to_string(line_number) + ": ";
            if (separator == std::string_view::npos || separator == 0)
                result.errors.emplace_back(prefix + "Malformed entry");
            else
            {
                const std::string key(line.substr(0, separator));
                bool value_valid = true;
                auto value = decode_value(line.substr(separator + 1), value_valid);
                if (!value_valid)
                    result.errors.emplace_back(prefix + "Invalid escape");
                else if (!result.values.emplace(key, std::move(value)).second)
                    result.errors.emplace_back(prefix + "Duplicate key " + key);
            }
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return result;
}
} // namespace

std::string_view default_language()
{
    return languages.front().code;
}

std::span<const LanguageInfo> supported_languages()
{
    return languages;
}

bool is_supported_language(std::string_view code)
{
    for (const auto &language : languages)
        if (language.code == code)
            return true;
    return false;
}

Catalog Catalog::load(const std::filesystem::path &i18n_directory, std::string_view requested_locale)
{
    Catalog result;
    result.locale_ = is_supported_language(requested_locale) ? std::string(requested_locale)
                                                             : std::string(default_language());
    if (result.locale_ != requested_locale)
        result.warnings_.emplace_back("Unsupported locale; using English");

    auto english = parse_file(i18n_directory / "en.lang");
    if (!english.errors.empty() || english.values.empty())
    {
        const auto detail = english.errors.empty() ? "English catalog is empty" : english.errors.front();
        throw std::runtime_error("Cannot load the English localization catalog: " + detail);
    }
    result.values_ = std::move(english.values);

    if (result.locale_ == default_language())
        return result;

    auto translation = parse_file(i18n_directory / (result.locale_ + ".lang"));
    if (!translation.errors.empty())
    {
        result.warnings_.insert(result.warnings_.end(), translation.errors.begin(), translation.errors.end());
        return result;
    }
    for (auto &[key, value] : translation.values)
    {
        const auto found = result.values_.find(key);
        if (found == result.values_.end())
            result.warnings_.emplace_back("Unknown translation key " + key);
        else
            found->second = std::move(value);
    }
    // 선택 언어에 없는 항목은 이미 보관한 영어 값을 그대로 사용한다. 진단 목록에도
    // 키를 남겨 번역 파일이 일부만 갱신된 상태를 배포 전에 발견할 수 있게 한다.
    for (const auto &[key, value] : result.values_)
    {
        (void)value;
        if (!translation.values.contains(key))
            result.warnings_.emplace_back("Missing translation key " + key + "; using English");
    }
    return result;
}

std::string_view Catalog::locale() const
{
    return locale_;
}

std::string_view Catalog::text(std::string_view key) const
{
    const auto found = values_.find(key);
    if (found == values_.end())
        throw std::out_of_range("Missing English translation key: " + std::string(key));
    return found->second;
}

const std::vector<std::string> &Catalog::warnings() const
{
    return warnings_;
}
} // namespace qm::localization
