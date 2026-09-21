// C++ 응용 계층이 사용하는 화면 인터페이스다. Slint 생성 타입을 숨기고 일반 C++ 값과 콜백만 공개한다.
#pragma once
#include "settings.hpp"
#include "catalog.hpp"
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace qm::ui
{
// 토글/외부 링크를 의미 있는 이름으로 구분한다. 화면 행 번호나 Windows 리소스 번호에 의존하지 않는다.
enum class Toggle
{
    Mute,
    Denoise,
    Expander,
    Highpass,
    Autostart,
    StartHidden,
    AutoProcess
};
enum class Link
{
    Transport,
    Slint
};

// 표시할 이름 배열과 현재 선택 인덱스를 묶는다. -1은 선택한 장치가 현재 목록에 없다는 뜻이다.
struct DeviceChoices
{
    std::vector<std::string> inputs, outputs, speakers;
    int input_index = -1, output_index = -1, speaker_index = -1;
    std::string output_name;
};
// 실행/복구/작업 중/미리듣기 여부를 함께 전달해 각 버튼과 문구가 같은 상태를 표현하게 한다.
struct ProcessingState
{
    bool running = false, recovering = false, busy = false, testing = false;
};
// 이미 표시용으로 가공된 레벨과 발화 확률, 성능 문구다. 원본 오디오 샘플은 UI로 보내지 않는다.
struct MeterReading
{
    float input = 0, output = 0;
    int voice_percent = 0;
    std::string performance;
};
// 화면이 컨트롤러에 요청할 수 있는 동작 목록이다. 빈 std::function은 어댑터에서 호출하지 않는다.
struct Actions
{
    std::function<void()> close, tray, start_stop, speaker_test, refresh, reset, setup;
    std::function<void()> load_custom_preset, save_custom_preset;
    std::function<void(float)> speaker_volume; // Normalized 0..1.
    std::function<void(int)> select_input, select_output, select_speaker;
    std::function<void(int)> select_language;
    std::function<void(Toggle)> toggle;
    std::function<void(Parameter, float)> parameter; // Domain units, not slider percentages.
    std::function<void(Preset)> preset;
    std::function<void(Link)> link;
};

// The controller sees value models and commands, never compiler-generated UI types.
class MainView final
{
    // PIMPL: 구현 타입을 전방 선언만 하고 unique_ptr로 소유한다. 생성 UI 헤더 변경이 응용 계층으로 퍼지는 것을 줄인다.
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit MainView(const std::filesystem::path &asset_directory);
    // Impl의 정의를 아는 cpp에서 소멸자를 정의하여 unique_ptr가 완전한 타입을 삭제하도록 한다.
    ~MainView();
    // 창과 콜백 소유권을 복제하지 않도록 복사 생성/대입을 금지한다.
    MainView(const MainView &) = delete;
    MainView &operator=(const MainView &) = delete;
    // 이벤트 연결은 bind, 컨트롤러→화면 값 반영은 set 함수들이 담당한다.
    void bind(Actions);
    void set_version(std::string_view);
    void set_catalog(const localization::Catalog &);
    void set_languages(const std::vector<std::string> &self_names, int selected_index);
    void set_settings(const Settings &);
    void set_devices(const DeviceChoices &);
    void set_processing(const ProcessingState &);
    void set_meter(const MeterReading &);
    void set_status(std::string_view);
    // UI의 0~100 퍼센트를 엔진용 0~1 값으로 변환해 반환한다.
    float preview_volume() const;
    void show();
    void show_volume();
    void hide();
};
} // namespace qm::ui
