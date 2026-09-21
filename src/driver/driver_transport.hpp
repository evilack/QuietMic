#pragma once
#include "driver_protocol.hpp"
#include <array>
#include <windows.h>
namespace qm
{
// 사용자 모드 앱에서 커널 드라이버로 PCM을 보내는 전용 통로이다.
// 핸들은 이 객체 하나가 소유하고 소멸자가 닫는다(RAII). 복사를 금지해 중복 닫기를 막는다.
class DriverTransport
{
    HANDLE handle_ = INVALID_HANDLE_VALUE;

  public:
    DriverTransport();
    ~DriverTransport();
    DriverTransport(const DriverTransport &) = delete;
    DriverTransport &operator=(const DriverTransport &) = delete;
    // 10ms 분량의 실수 샘플을 PCM16 패킷으로 보내고 커널 큐 상태를 돌려받는다.
    driver::Status write(const std::array<float, driver::frame_samples> &samples);
    // 아직 소비되지 않은 샘플과 큐 통계를 비워 이전 음성이 이어 나오지 않게 한다.
    void reset();
};
} // namespace qm
