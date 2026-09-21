// Windows의 오디오 장치 추가·제거·상태 변경을 구독한다. 주기적으로 장치를 열거하지 않는다.
#pragma once
#include <functional>
#include <memory>

namespace qm
{
class DeviceWatcher
{
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    // 콜백은 Windows 스레드에서 실행된다. UI 작업은 이벤트 루프로 전달해야 한다.
    // 생성과 소멸은 같은 스레드에서 실행하여 COM 초기화 수명을 맞춘다.
    explicit DeviceWatcher(std::function<void()> changed);
    ~DeviceWatcher();
    DeviceWatcher(const DeviceWatcher &) = delete;
    DeviceWatcher &operator=(const DeviceWatcher &) = delete;
};
} // namespace qm
