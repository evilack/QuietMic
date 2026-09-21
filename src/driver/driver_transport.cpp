#include "driver_transport.hpp"
#include "audio_format.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
namespace qm
{
// 커널이 만든 제어 장치의 사용자 모드 경로를 연다. 파일처럼 보이지만 디스크 파일은 아니다.
DriverTransport::DriverTransport()
{
    // 공유 모드 0은 다른 생산자가 같은 장치를 함께 열지 못하게 한다.
    // 커널도 별도로 파일 객체를 확인하므로 앱의 설정만 믿고 접근을 허용하지 않는다.
    handle_ = CreateFileW(driver::device_path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        throw std::runtime_error("QuietMic 가상 마이크 연결 실패 (" + std::to_string(GetLastError()) + ")");
}
// 예외로 오디오 처리가 끝나도 핸들을 반환한다. 커널의 CLEANUP/CLOSE에서 큐도 정리된다.
DriverTransport::~DriverTransport()
{
    if (handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(handle_);
}
// 이 함수의 경계에서 사용자 모드의 float 오디오를 공용 프로토콜의 정수 PCM으로 바꾼다.
driver::Status DriverTransport::write(const std::array<float, driver::frame_samples> &samples)
{
    driver::Packet packet{driver::version, driver::frame_samples, {}};
    for (size_t i = 0; i < samples.size(); i++)
    {
        // 범위를 벗어난 값/비정상 실수 처리까지 공통 변환 함수에 맡겨 드라이버 입력을 일정하게 한다.
        packet.samples[i] = format::to_pcm16(samples[i]);
    }
    driver::Status status{};
    DWORD bytes = 0;
    // DeviceIoControl은 IOCTL 요청이다. METHOD_BUFFERED이므로 Windows가 패킷을
    // 커널 버퍼로 복사하며, 커널은 앱의 samples 포인터를 직접 참조하지 않는다.
    if (!DeviceIoControl(handle_, driver::write_ioctl, &packet, sizeof(packet), &status, sizeof(status),
                         &bytes, nullptr))
        throw std::runtime_error("QuietMic 오디오 전송 실패 (" + std::to_string(GetLastError()) + ")");
    // 호출 성공만으로 호환성을 판단하지 않고 응답 길이와 프로토콜 버전도 확인한다.
    if (bytes != sizeof(status) || status.protocol_version != driver::version)
        throw std::runtime_error("QuietMic 앱과 가상 마이크 구성요소의 버전이 일치하지 않습니다.");
    return status;
}
// 별도 데이터가 없는 제어 요청으로 큐를 초기화한다. 실패를 숨기지 않고 상위 수명 관리에 알린다.
void DriverTransport::reset()
{
    DWORD bytes = 0;
    if (!DeviceIoControl(handle_, driver::reset_ioctl, nullptr, 0, nullptr, 0, &bytes, nullptr))
        throw std::runtime_error("QuietMic 출력 초기화 실패");
}
} // namespace qm
