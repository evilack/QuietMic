// 생성된 Slint 헤더를 포함하는 비공개 구현이다. 응용 계층의 값/이벤트를 Slint 속성/콜백으로 변환한다.
#pragma once
#include "main_view.hpp"
#include "quietmic_window.h"
#include <optional>

namespace qm::ui
{
// 생성된 Slint 타입은 이 비공개 구현 안에서만 사용하며 응용 계층에는 노출하지 않는다.
struct MainView::Impl final
{
    // 컴포넌트 핸들이 생성된 창의 수명을 관리한다. create 시점에 실제 Slint 컴포넌트를 만든다.
    slint::ComponentHandle<generated::MainWindow> window = generated::MainWindow::create();
    std::optional<slint::ComponentHandle<generated::VolumeWindow>> volume_window;
    // 슬라이더 행 모델을 공유 소유한다. 같은 모델의 행을 갱신하면 UI가 변경을 통지받는다.
    std::shared_ptr<slint::VectorModel<generated::Control>> controls =
        std::make_shared<slint::VectorModel<generated::Control>>();
    // 작은 값 타입 사본을 보관해 나중에 만드는 볼륨 창에도 현재 언어를 즉시 적용한다.
    generated::Translations translations;
    // 컨트롤러가 건넨 콜백 묶음을 보관한다. 화면 이벤트가 발생할 때 이 묶음으로 전달한다.
    Actions actions;

    // UI와 설정 계층의 열거형을 명시적으로 변환한다. 정수 값이 우연히 같다는 가정으로 캐스팅하지 않는다.
    static generated::ParameterId to_ui(Parameter);
    static Parameter from_ui(generated::ParameterId);
    static generated::PresetId to_ui(Preset);
    static Preset from_ui(generated::PresetId);
    static Toggle from_ui(generated::ToggleId);
    static Link from_ui(generated::ExternalLink);
};
} // namespace qm::ui
