// 장치의 영구 식별자(ID)와 UI 목록 순서(인덱스)를 서로 연결하는 장치 목록 관리 클래스다.
#pragma once
#include "audio.hpp"
#include "settings.hpp"

namespace qm
{
class DeviceCatalog
{
    // 전체 목록과 용도별 목록을 함께 보관한다. 목록 순서가 달라져도 설정에는 문자열 ID를 저장한다.
    std::vector<Device> all_, inputs_, outputs_, speakers_;
    std::string speaker_id_;
    static int index_of(const std::vector<Device> &, const std::string &);

  public:
    // 현재 OS 장치를 열거하여 replace로 게시한다. replace는 열거와 게시 책임을 분리하는 경계다.
    void refresh(Settings &);
    void replace(std::vector<Device>, Settings &);
    // 목록은 읽기 전용 참조로 제공한다. 호출자는 갱신 후 예전 인덱스를 다시 사용하지 않도록 해야 한다.
    const std::vector<Device> &inputs() const
    {
        return inputs_;
    }
    const std::vector<Device> &outputs() const
    {
        return outputs_;
    }
    const std::vector<Device> &speakers() const
    {
        return speakers_;
    }
    // 저장된 ID를 현재 목록에서 찾는다. 존재하지 않는 선택은 -1이며 첫 장치로 몰래 바꾸지 않는다.
    int input_index(const Settings &s) const
    {
        return index_of(inputs_, s.input_id);
    }
    // 가상 출력의 표시 인덱스도 ID로 다시 찾는다.
    int output_index(const Settings &s) const
    {
        return index_of(outputs_, s.output_id);
    }
    // 미리듣기 스피커 선택은 사용자 Settings와 별개의 speaker_id_에서 찾는다.
    int speaker_index() const
    {
        return index_of(speakers_, speaker_id_);
    }
    const std::string &speaker_id() const
    {
        return speaker_id_;
    }
    // UI에서 받은 인덱스를 범위 검사 후 실제 ID로 변환한다.
    void select_input(int, Settings &) const;
    void select_output(int, Settings &) const;
    void select_speaker(int);
    std::string output_id(const Settings &) const;
    std::string output_name(const Settings &) const;
    bool output_ready(const Settings &) const;
};
} // namespace qm
