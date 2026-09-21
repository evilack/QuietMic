// 장치를 용도별로 분류하고, 현재 목록에서 사용자의 ID 기반 선택을 찾아 UI에 연결한다.
#include "device_catalog.hpp"
#include "setup.hpp"

namespace qm
{
// 선형 탐색으로 동일한 ID를 찾는다. -1은 목록에 해당 장치가 없다는 뜻이다.
int DeviceCatalog::index_of(const std::vector<Device> &list, const std::string &id)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].id == id)
            return static_cast<int>(i);
    return -1;
}
// OS 장치 열거 결과를 replace에 넘겨 한 번에 목록을 교체한다.
void DeviceCatalog::refresh(Settings &settings)
{
    replace(devices(), settings);
}
// 새 목록과 선택 값을 지역 변수에서 준비한 뒤 게시한다. 준비 중 실패해도 기존 목록은 유지된다.
void DeviceCatalog::replace(std::vector<Device> list, Settings &settings)
{
    std::vector<Device> inputs, outputs, speakers;
    // 실제 재생 장치는 스피커, 일반 캡처 장치는 입력, 전용 가상 캡처 장치는 출력 선택 목록에 넣는다.
    for (const auto &device : list)
    {
        if (!device.capture && !device.native)
            speakers.push_back(device);
        if (device.capture && !device.native)
            inputs.push_back(device);
        if (device.capture && device.native)
            outputs.push_back(device);
    }
    // 선택 ID를 복사해 준비한다. 기존 ID가 비어 있을 때만 기본 장치를 선택하므로 일시 분리된 선택은 남는다.
    auto input = settings.input_id;
    auto output = settings.output_id;
    auto speaker = speaker_id_;
    if (input.empty() && !inputs.empty())
        input = inputs.front().id;
    // 가상 출력의 기본 선택은 전체 목록을 해석하는 공통 선택 함수에 맡긴다.
    if (output.empty())
        output = qm::select_output(list);
    if (speaker.empty() && !speakers.empty())
        speaker = speakers.front().id;
    // Validation and allocations finish before publishing index-to-device mappings.
    // 여기부터 준비된 벡터와 문자열을 swap으로 교체한다. 할당 작업을 끝낸 뒤 목록/ID 매핑을 함께 게시하는 구조다.
    all_.swap(list);
    inputs_.swap(inputs);
    outputs_.swap(outputs);
    speakers_.swap(speakers);
    settings.input_id.swap(input);
    settings.output_id.swap(output);
    speaker_id_.swap(speaker);
}
// 음수 또는 목록 밖 인덱스는 무시하여 잘못된 배열 접근을 막는다. 유효한 행의 ID만 설정에 저장한다.
void DeviceCatalog::select_input(int index, Settings &settings) const
{
    if (index >= 0 && index < static_cast<int>(inputs_.size()))
        settings.input_id = inputs_[index].id;
}
// 출력 UI 인덱스를 현재 가상 출력 목록의 장치 ID로 바꾼다.
void DeviceCatalog::select_output(int index, Settings &settings) const
{
    if (index >= 0 && index < static_cast<int>(outputs_.size()))
        settings.output_id = outputs_[index].id;
}
// 미리듣기 스피커의 ID를 별도로 기억한다.
void DeviceCatalog::select_speaker(int index)
{
    if (index >= 0 && index < static_cast<int>(speakers_.size()))
        speaker_id_ = speakers_[index].id;
}
// 전체 목록과 저장된 선택을 공통 출력 해석 함수에 넘겨 사용 가능한 출력 ID를 구한다.
std::string DeviceCatalog::output_id(const Settings &settings) const
{
    return configured_output(all_, settings.output_id);
}
// 목록에 없으면 빈 문자열을 반환하여 컨트롤러가 대체 안내를 표시하도록 한다.
std::string DeviceCatalog::output_name(const Settings &settings) const
{
    auto index = output_index(settings);
    return index < 0 ? std::string{} : outputs_[index].name;
}
// 선택이 존재하는지만 아니라 QuietMic 출력으로 준비된 장치인지까지 검사한다.
bool DeviceCatalog::output_ready(const Settings &settings) const
{
    auto index = output_index(settings);
    return index >= 0 && quietmic_output(outputs_[index]);
}
} // namespace qm
