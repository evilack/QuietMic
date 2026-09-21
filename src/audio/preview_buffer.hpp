// 스피커 미리 듣기용 완충 큐다. DSP의 10 ms 프레임을 받아 WASAPI가 요구하는
// 서로 다른 길이로 꺼내며, 드라이버와 같은 PCM16 변환/큐 정책을 사용한다.
#pragma once
#include "audio_format.hpp"
#include <array>
#include <span>

namespace qm
{
// Accessed only by the audio worker. Diagnostics cover the entire preview session,
// including mute transitions; clearing queued speech must not erase prior failures.
class PreviewBuffer
{
    driver::Queue queue_;
    unsigned previous_underruns_ = 0, previous_overruns_ = 0;

  public:
    // 미리 듣기는 이 객체가 바로 소비자 역할을 하므로 생성 시 큐를 활성화한다.
    PreviewBuffer()
    {
        queue_.capture_active(true);
    }
    // 20 ms(두 프레임)를 모은 뒤 재생을 시작한다. 캡처/재생 시계의 이벤트 시점이
    // 달라도 한 프레임의 여유를 두기 위한 시작 조건이다.
    bool ready_to_start() const
    {
        return queue_.status().queued_samples >= 2 * format::frame_samples;
    }
    // 볼륨은 0~1 범위의 미리 듣기 전용 배율이다. 각 float 샘플을 안전한 PCM16으로
    // 바꾸어 고정 크기 패킷으로 넣는다. 큐가 넘치면 내부 Queue가 오래된 음성을 버린다.
    void write(std::span<const float, format::frame_samples> frame, float volume)
    {
        driver::Packet packet{driver::version, format::frame_samples, {}};
        volume = std::isfinite(volume) ? std::clamp(volume, 0.f, 1.f) : 0.f;
        for (size_t i = 0; i < frame.size(); ++i)
            packet.samples[i] = format::to_pcm16(frame[i] * volume);
        queue_.write(packet, sizeof(packet));
    }
    void render(float *out, unsigned count, bool muted)
    {
        std::array<int16_t, format::frame_samples> pcm{};
        // WASAPI 요청 길이가 480보다 클 수 있으므로 임시 배열 크기 단위로 나누어 채운다.
        // 일반 재생은 부족한 부분을 Queue가 0으로 채우며 부족 횟수도 기록한다.
        for (unsigned at = 0; at < count;)
        {
            const auto chunk = std::min<unsigned>(pcm.size(), count - at);
            // Intentional silence still drains available speech, but is not starvation.
            // 음소거 중에는 실제 남은 음성만 소비한다. 의도적으로 0을 출력하는 상황에서
            // 빈 큐를 읽어 허위 underrun을 만들지 않으며, 남은 음성이 나중에 재생되는 것도 막는다.
            const auto available = muted ? std::min(chunk, queue_.status().queued_samples) : chunk;
            if (available)
                queue_.read(pcm.data(), available);
            for (unsigned i = 0; i < chunk; ++i)
                out[at + i] = muted ? 0.f : pcm[i] / format::pcm16_scale;
            at += chunk;
        }
    }
    // 음소거 전환으로 큐를 비워도 이전의 부족/초과 기록은 세션 합계에 보관한다.
    // Queue::reset 자체는 카운터까지 지우므로 먼저 따로 더해 두어야 한다.
    void reset()
    {
        const auto old = queue_.status();
        previous_underruns_ += old.underruns;
        previous_overruns_ += old.overruns;
        queue_.reset();
    }
    // 현재 큐 길이에 초기화 전후의 누적 진단 카운터를 합쳐 반환한다.
    driver::Status status() const
    {
        auto result = queue_.status();
        result.underruns += previous_underruns_;
        result.overruns += previous_overruns_;
        return result;
    }
};
} // namespace qm
