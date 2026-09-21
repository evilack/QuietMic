// 실시간 캡처와 오프라인 WAV 처리가 공유하는 DSP 상태 정의다.
// 한 객체를 순서대로 호출해야 필터 이력과 RNNoise의 프레임 지연이 이어진다.
#pragma once
#include "settings.hpp"
#include "audio_format.hpp"
#include <array>
#include <memory>
#include <span>
struct DenoiseState;
namespace qm
{
constexpr int FrameSize = format::frame_samples;
// 음성 확률로 조용한 구간의 이득을 낮추는 확장기다. gain_db_는 현재 감쇠(dB),
// remaining_은 닫힌 뒤 유지할 시간(ms), open_은 음성 통과 여부다.
class Expander
{
    float gain_db_ = 0, remaining_ = 0;
    bool open_ = true;

  public:
    // 10 ms마다 한 번 호출한다. 0~1 음성 확률을 받아 최종 샘플에 곱할 선형 이득을 반환한다.
    float next(float probability, const Settings &);
    void reset();
};
class DspPipeline
{
    // RNNoise 상태는 이 객체가 생성/파괴한다. 복사를 금지해 같은 포인터의 중복 해제를 막는다.
    DenoiseState *model_ = nullptr;
    Expander envelope_;
    // input_은 현재 하이패스 적용 결과, wet_은 잡음 제거 결과, previous_는 한 프레임
    // 늦춘 원음이다. RNNoise 출력과 원음의 시간을 맞춰 섞기 위해 이전 입력을 보관한다.
    std::array<float, FrameSize> previous_{}, input_{}, wet_{};
    // biquad 하이패스의 과거 입력 두 개(x)와 출력 두 개(y)를 프레임 경계 너머로 유지한다.
    double x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
    float previous_gain_ = 1, cutoff_ = parameter_limits::cutoff.default_value;
    bool last_denoise_ = true;

  public:
    DspPipeline();
    ~DspPipeline();
    DspPipeline(const DspPipeline &) = delete;
    DspPipeline &operator=(const DspPipeline &) = delete;
    // 정확히 480개의 정규화된 모노 샘플을 읽고 같은 수를 out에 쓴다. 반환값은
    // 출력 샘플이 아니라 RNNoise 음성 확률이며, 출력 음성은 입력보다 한 프레임 늦다.
    float process(std::span<const float, FrameSize>, std::span<float, FrameSize>, const Settings &);
};
} // namespace qm
