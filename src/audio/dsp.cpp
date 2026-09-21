// 처리 순서: 입력 정리 → 하이패스 → RNNoise → 시간 정렬된 원음 혼합 →
// 확장기/사용자 이득 → 음소거와 진폭 제한. 상태는 프레임 사이에 계속 유지된다.
#include "dsp.hpp"
#include "rnnoise.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
namespace qm
{
float Expander::next(float probability, const Settings &s)
{
    // 확장기는 RNNoise의 음성 확률에 의존한다. 해당 기능이 꺼졌거나 감쇠량이 0이면
    // 이전 감쇠 상태도 지우고 1배 이득으로 통과시킨다.
    if (!s.expander || !s.denoise || s.reduction <= 0)
    {
        reset();
        return 1;
    }
    // 열림 기준과 닫힘 기준 사이에 0.1의 간격을 둔다(히스테리시스).
    // 확률이 경계 근처에서 흔들릴 때 매 프레임 열렸다 닫히는 현상을 줄인다.
    if (probability >= s.threshold)
    {
        open_ = true;
        remaining_ = s.hold_ms;
    }
    else if (probability <= std::max(0.f, s.threshold - .1f) && open_)
    {
        open_ = false;
        remaining_ = s.hold_ms;
    }
    // 닫힘을 감지해도 hold_ms 동안은 0 dB를 목표로 유지한다. 호출 한 번이 10 ms이므로
    // 남은 시간을 프레임 길이만큼 줄이고, 유지 시간이 끝난 뒤에 감쇠를 시작한다.
    bool hold = !open_ && remaining_ > 0;
    if (hold)
        remaining_ = std::max(0.f, remaining_ - format::frame_ms);
    const float target = (open_ || hold) ? 0.f : -s.reduction;
    // 이득이 커지는 쪽은 attack, 작아지는 쪽은 release 시간이다. 전체 감쇠량을
    // 지정 시간에 이동할 속도로 나누고 min/max로 목표 dB를 지나치지 않게 한다.
    const float duration = target > gain_db_ ? s.attack_ms : s.release_ms;
    const float delta = duration <= 0 ? s.reduction : s.reduction * format::frame_ms / duration;
    if (target > gain_db_)
        gain_db_ = std::min(target, gain_db_ + delta);
    else
        gain_db_ = std::max(target, gain_db_ - delta);
    // dB는 로그 단위이므로 샘플에 곱할 진폭 비율로 바꾼다. 0 dB는 1배가 된다.
    return std::pow(10.f, gain_db_ / 20.f);
}
// 다음 음성을 바로 통과시킬 열린 상태로 되돌린다.
void Expander::reset()
{
    gain_db_ = remaining_ = 0;
    open_ = true;
}
// 모델을 만들고 무음 한 프레임으로 초기 내부 상태를 준비한다. 모델 메모리 확보에
// 실패하면 불완전한 객체를 계속 쓰지 않고 예외를 올린다.
DspPipeline::DspPipeline()
{
    model_ = rnnoise_create(nullptr);
    if (!model_)
        throw std::bad_alloc();
    std::array<float, FrameSize> zeros{};
    rnnoise_process_frame(model_, wet_.data(), zeros.data());
}
// 객체 수명이 끝날 때 이 객체가 소유한 RNNoise 상태를 돌려준다.
DspPipeline::~DspPipeline()
{
    if (model_)
        rnnoise_destroy(model_);
}
float DspPipeline::process(std::span<const float, FrameSize> in, std::span<float, FrameSize> out,
                           const Settings &s)
{
    // 하이패스 차단 주파수 변경은 한 번에 적용하지 않고 매 프레임 차이의 10%씩 따라간다.
    // 현재 차단 주파수(Hz)를 샘플당 각주파수로 바꾸어 2차 하이패스 계수를 다시 만든다.
    cutoff_ += .1f * (s.cutoff - cutoff_);
    const double w = 2 * std::numbers::pi * cutoff_ / format::sample_rate, c = std::cos(w),
                 alpha = std::sin(w) / std::sqrt(2.0), a0 = 1 + alpha;
    const double b0 = (1 + c) / 2 / a0, b1 = -(1 + c) / a0, b2 = b0, a1 = -2 * c / a0, a2 = (1 - alpha) / a0;
    // 입력을 유한한 -1~1 값으로 제한한 뒤 과거 입력/출력으로 현재 필터 출력을 계산한다.
    // 하이패스를 꺼도 이력은 갱신하므로 다시 켰을 때 필터가 오래된 입력에서 이어지지 않는다.
    for (int i = 0; i < FrameSize; i++)
    {
        double x = std::isfinite(in[i]) ? std::clamp(in[i], -1.f, 1.f) : 0;
        double y = b0 * x + b1 * x1_ + b2 * x2_ - a1 * y1_ - a2 * y2_;
        x2_ = x1_;
        x1_ = x;
        y2_ = y1_;
        y1_ = y;
        input_[i] = float(s.highpass ? y : x);
        // RNNoise API는 float 배열이지만 진폭 단위는 PCM16 범위다. 이 경계에서만
        // 32768을 곱하고, 처리 결과는 아래에서 다시 정규화된 진폭으로 돌린다.
        wet_[i] = input_[i] * format::pcm16_scale;
    }
    float vad = 0;
    // RNNoise가 내놓는 음성은 이전 프레임에 정렬된다. 기능을 껐을 때도 previous_를
    // 사용해 경로에 따라 지연이 바뀌지 않게 한다. 다시 켠 첫 결과는 오래된 합성 상태를
    // 포함할 수 있으므로 그 한 프레임만 정렬된 원음으로 대신한다.
    if (s.denoise)
    {
        vad = rnnoise_process_frame(model_, wet_.data(), wet_.data());
        for (auto &v : wet_)
            v /= format::pcm16_scale;
        // First re-enabled frame contains the old synthesis overlap; use aligned dry instead.
        if (!last_denoise_)
            wet_ = previous_;
    }
    else
        wet_ = previous_;
    // 확장기의 선형 이득과 사용자 dB 이득을 곱한다. dry_percent는 잡음 제거 결과에
    // 섞을 원음 비율이며, 둘 다 이전 프레임 시점이므로 어긋난 음성이 중첩되지 않는다.
    const float target = envelope_.next(vad, s) * std::pow(10.f, s.gain_db / 20),
                dry = s.denoise ? s.dry_percent * .01f : 0;
    for (int i = 0; i < FrameSize; i++)
    {
        // 프레임 첫 샘플부터 마지막 샘플까지 이전 이득에서 새 목표까지 선형으로 이동해
        // 경계에서 갑자기 진폭이 바뀌는 것을 줄인다. 음소거는 최종 출력에서 0을 선택한다.
        float gain = previous_gain_ + (target - previous_gain_) * float(i + 1) / FrameSize;
        float v = ((1 - dry) * wet_[i] + dry * previous_[i]) * gain;
        out[i] = s.muted ? 0.f : (std::isfinite(v) ? std::clamp(v, -1.f, 1.f) : 0.f);
    }
    // 현재 입력과 목표 이득을 다음 호출의 이전 상태로 넘긴다. 음소거 중에도 여기까지
    // 처리하므로 필터와 모델의 시간 흐름은 계속 진행된다.
    previous_gain_ = target;
    previous_ = input_;
    last_denoise_ = s.denoise;
    return vad;
}
} // namespace qm
