// 앱 쪽 오디오 단위와 변환을 모은 파일이다. 실제 전송 규격은 driver_protocol.hpp를 따라
// 48,000 Hz 모노, 480샘플(10 ms)을 한 처리 묶음으로 사용한다.
#pragma once
#include "driver_protocol.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
namespace qm::format
{
inline constexpr auto sample_rate = driver::sample_rate;
inline constexpr auto frame_samples = driver::frame_samples;
inline constexpr float frame_ms = 1000.f * frame_samples / sample_rate;
// DSP의 부동소수점 진폭 1을 PCM16 정수의 32768에 대응시킨다. 시간 제한은 음성 길이와
// 장치 재연결 간격이며 샘플 개수와 혼용하지 않는다.
inline constexpr float pcm16_scale = 32768.f;
inline constexpr auto preview_limit = std::chrono::seconds(60);
inline constexpr auto reconnect_interval = std::chrono::seconds(3);
// 정규화된 진폭을 드라이버/WAV용 정수로 바꾼다. NaN·무한대는 무음 처리하고,
// 양수 최대값은 32767이므로 +1보다 조금 작게 제한한 뒤 변환해야 넘침을 피한다.
inline int16_t to_pcm16(float sample)
{
    if (!std::isfinite(sample))
        return 0;
    return static_cast<int16_t>(std::clamp(sample, -1.f, (pcm16_scale - 1.f) / pcm16_scale) * pcm16_scale);
}
} // namespace qm::format
