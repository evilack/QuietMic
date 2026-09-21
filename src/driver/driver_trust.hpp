#pragma once
#include <filesystem>
#include <string_view>
namespace qm
{
// INF/SYS가 CAT에 실제로 포함되고 Windows 드라이버 신뢰 정책을 통과하는지 검사한다.
// 파일 존재 확인이나 자체 해시 비교만으로 설치를 허용하지 않는다. 실패하면 예외를 던진다.
void verify_native_driver(const std::filesystem::path &directory);
// 이름은 호출자가 고정한다. Microsoft 루트 검사는 기본 드라이버 신뢰/폐기 검사를 보완한다.
void verify_catalog_driver(const std::filesystem::path &directory, std::wstring_view basename,
                           bool require_microsoft_root);

} // namespace qm
