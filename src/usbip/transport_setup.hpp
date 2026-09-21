#pragma once
#include <filesystem>
#include <string_view>
namespace qm::usbip
{
// 설치와 검사는 분리한다. 검사는 파일/장치 상태를 읽기만 하고 정책을 변경하지 않는다.
void verify_transport_package(const std::filesystem::path &directory);
unsigned setup_transport(const std::filesystem::path &directory);
void require_transport_ready();
// 활성 공유 연결이 없을 때 호스트 컨트롤러와 두 Driver Store 패키지를 제거한다.
unsigned remove_transport();
bool transport_hardware_id(std::wstring_view id);
} // namespace qm::usbip
