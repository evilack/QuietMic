#pragma once
#include "usb_audio_server.hpp"
#include <filesystem>
#include <condition_variable>
#include <thread>

namespace qm::usbip
{
// 표시 이름이 아니라 USB 인스턴스 ID에서 얻은 컨테이너를 반환한다. 장치가 없으면 빈 문자열이다.
std::string device_container();
enum class SessionState
{
    Stopped,
    Preparing,
    Ready,
    Failed
};
struct SessionSnapshot
{
    SessionState state = SessionState::Stopped;
    std::string endpoint, error;
};
class UsbMicSession
{
    std::filesystem::path runtime_;
    std::shared_ptr<PcmOutputBuffer> buffer_ = std::make_shared<PcmOutputBuffer>();
    UsbAudioServer server_{buffer_};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    SessionSnapshot snapshot_;
    std::function<void()> handler_;
    bool import_lost_ = false;
    std::jthread worker_;
    void run(std::stop_token);
    void publish(SessionSnapshot, std::stop_token = {});

  public:
    explicit UsbMicSession(std::filesystem::path runtime);
    ~UsbMicSession();
    void start();
    void stop();
    // 준비/전송 스레드의 알림을 UI 이벤트 루프로 전달한다. 콜백에서 start/stop을 직접 호출하지 않는다.
    void set_changed_handler(std::function<void()>);
    SessionSnapshot snapshot() const;
    std::shared_ptr<AudioFrameSink> sink() const;
};
} // namespace qm::usbip
