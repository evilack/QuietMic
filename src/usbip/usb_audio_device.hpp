#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace qm::usbip::wire
{
using Bytes = std::vector<unsigned char>;
inline constexpr unsigned device_id = 0x10001;
inline constexpr unsigned max_payload = 1024 * 1024;
inline constexpr unsigned max_iso_packets = 256;
inline constexpr const char *serial = "QUIETMIC-USBIP-001";
inline constexpr const wchar_t *instance_id = L"USB\\VID_FFFF&PID_51CA\\QUIETMIC-USBIP-001";
// 세션과 설치기가 같은 전역 잠금으로 가상 USB 식별자의 동시 사용을 막는다.
inline constexpr const wchar_t *ownership_mutex =
    L"Global\\QuietMic.USBIP.VID_FFFF.PID_51CA.QUIETMIC-USBIP-001";
// 시험용 USB VID/PID다. 정식 배포 식별자 정책은 설치기 확정 전에 검토한다.
unsigned number(const unsigned char *p);
void put(Bytes &, size_t offset, unsigned value);
Bytes operation(unsigned code, unsigned status = 0);
Bytes description();
Bytes configuration();
Bytes descriptor(unsigned type, unsigned index);
} // namespace qm::usbip::wire
