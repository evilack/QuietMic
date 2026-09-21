#pragma once
#include "audio_format.hpp"
#include <array>

namespace qm
{
struct AudioSinkStatus
{
    unsigned queued_samples = 0;
    unsigned underruns = 0;
    unsigned overruns = 0;
};

// DSP는 처리된 10ms 프레임만 전달한다. 소켓, USB 요청, 설치 상태를 알 필요가 없다.
// 구현은 write 안에서 네트워크 완료를 기다리지 않아야 한다.
class AudioFrameSink
{
  public:
    virtual ~AudioFrameSink() = default;
    virtual AudioSinkStatus write(const std::array<float, format::frame_samples> &frame) = 0;
    // 음소거 전환/생산자 종료 때 대기 음성을 폐기한다. 진단 카운터는 보존한다.
    virtual void reset() = 0;
};
} // namespace qm
