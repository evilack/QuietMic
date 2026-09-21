#pragma once
#include "audio.hpp"
#include <filesystem>
#include <string_view>
namespace qm
{
// 장치 검색/서명 검증/관리자 설치를 연결하는 사용자 모드 API이다.
// 실제 커널 코드 실행은 Windows가 검증한 드라이버를 로드한 뒤에 일어난다.
// Empty means no endpoint; multiple native endpoints are an error.
// 캡처 장치 중 QuietMic 식별자가 있는 하나만 선택한다. 이름이 비슷한 장치는 대상이 아니다.
std::string select_output(const std::vector<Device> &list);
void verify_driver_package(const std::filesystem::path &directory);
// Runs only in the elevated helper. 0=ready, 3010=reboot required.
// 등록 후 실제 오디오 끝점이 나타나는 것까지 확인한다. 3010은 재부팅이 필요하다는 Windows 코드이다.
unsigned setup_virtual_microphone(const std::filesystem::path &package);
bool quietmic_hardware_id(std::wstring_view id);
// 설치/제거와 실행 중인 앱이 같은 드라이버를 동시에 사용하지 않도록 선행 조건을 검사한다.
void require_app_stopped();
void require_no_installer();
// Elevated installer only; removes only QuietMic hardware instances and their unused OEM INF.
// 정확한 하드웨어 ID로 대상 장치를 추린 후, 다른 장치가 쓰지 않는 해당 OEM 패키지만 제거한다.
unsigned remove_virtual_microphone();
} // namespace qm
