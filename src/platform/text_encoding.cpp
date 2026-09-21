// 두 번의 Windows API 호출로 필요한 길이를 구한 뒤 정확한 버퍼에 인코딩을 변환한다.
#include "text_encoding.hpp"
#include <windows.h>
#include <limits>
#include <stdexcept>
namespace qm
{
// UTF-16 입력을 UTF-8로 바꾼다. 빈 입력은 Windows 변환 API를 부르지 않고 빈 결과를 돌려준다.
std::string utf8(const std::wstring &text)
{
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("Text too long");
    // 출력 버퍼를 nullptr로 주면 필요한 UTF-8 바이트 수만 계산한다. 길이는 int 한계를 먼저 검사한다.
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count)
        throw std::runtime_error("Invalid UTF-16");
    // 계산한 바이트 수만큼 버퍼를 만든 뒤 두 번째 호출에서 실제 변환 결과를 채운다.
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), count, nullptr, nullptr))
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}
// UTF-8 입력을 UTF-16으로 바꾼다. 잘못된 바이트열은 MB_ERR_INVALID_CHARS로 오류가 된다.
std::wstring wide(const std::string &text)
{
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("Text too long");
    // 필요한 wchar_t 개수를 먼저 구한다. 이 길이는 바이트 수가 아니라 UTF-16 코드 단위 수다.
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error("Invalid UTF-8");
    // 정확한 길이의 쓰기 가능한 문자열을 만들고 변환한다. 명시적 길이를 사용하므로 끝의 널 문자를 입력에 포함하지 않는다.
    std::wstring result(count, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), count))
        throw std::runtime_error("UTF-16 conversion failed");
    return result;
}
} // namespace qm
