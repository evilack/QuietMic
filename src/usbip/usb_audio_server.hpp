#pragma once
#include "pcm_output_buffer.hpp"
#include <memory>
#include <string>
#include <functional>

namespace qm::usbip
{
// 설치된 USB/IP 전송 드라이버가 가져갈 로컬 USB Audio Class 1 장치를 제공한다.
// 드라이버 설치나 마이크 캡처는 하지 않으며, 전달받은 큐의 소비자 역할만 맡는다.
class UsbAudioServer
{
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit UsbAudioServer(std::shared_ptr<PcmOutputBuffer>);
    ~UsbAudioServer();
    UsbAudioServer(const UsbAudioServer &) = delete;
    UsbAudioServer &operator=(const UsbAudioServer &) = delete;
    void start();
    void stop();
    // 가져오기 연결 스레드의 알림이다. 수명 제어(start/stop)는 호출자 스레드로 전달해야 한다.
    void set_import_lost_handler(std::function<void()>);
    unsigned short port() const;
};
} // namespace qm::usbip
