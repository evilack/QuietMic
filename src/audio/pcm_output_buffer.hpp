#pragma once
#include "audio_frame_sink.hpp"
#include <mutex>
#include <span>

namespace qm
{
// 오디오 생산자와 USB 소비자가 공유하는 60ms 고정 크기 큐다.
// 검증된 Queue의 변환/시작 여유 정책을 재사용하고 동시 접근만 이 계층에서 보호한다.
class PcmOutputBuffer final : public AudioFrameSink
{
    mutable std::mutex mutex_;
    driver::Queue queue_{true};
    bool active_ = false;
    unsigned previous_underruns_ = 0;
    unsigned previous_overruns_ = 0;

    AudioSinkStatus status_locked() const
    {
        const auto current = queue_.status();
        return {current.queued_samples, previous_underruns_ + current.underruns,
                previous_overruns_ + current.overruns};
    }

  public:
    AudioSinkStatus write(const std::array<float, format::frame_samples> &frame) override
    {
        driver::Packet packet{driver::version, format::frame_samples, {}};
        // 변환은 잠금 밖에서 수행한다. 공유 큐를 복사하는 짧은 구간에만 잠금을 잡는다.
        for (size_t i = 0; i < frame.size(); ++i)
            packet.samples[i] = format::to_pcm16(frame[i]);
        std::lock_guard lock(mutex_);
        queue_.write(packet, sizeof(packet));
        return status_locked();
    }

    void read(std::span<int16_t> destination)
    {
        std::lock_guard lock(mutex_);
        // USB 요청 크기와 관계없이 할당된 span 범위만 채운다. 부족하면 무음을 넣는다.
        queue_.read(destination.data(), static_cast<uint32_t>(destination.size()));
    }

    void set_consumer_active(bool active)
    {
        std::lock_guard lock(mutex_);
        if (active == active_)
            return;
        const auto old = queue_.status();
        previous_underruns_ += old.underruns;
        previous_overruns_ += old.overruns;
        // Queue의 연결 전환은 음성과 내부 카운터를 지우므로 합계는 별도로 보존한다.
        queue_.capture_active(active);
        active_ = active;
    }

    void reset() override
    {
        std::lock_guard lock(mutex_);
        queue_.reset(false);
    }

    AudioSinkStatus status() const
    {
        std::lock_guard lock(mutex_);
        return status_locked();
    }
};
} // namespace qm
