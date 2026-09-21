# Slint 선언 파일을 C++ 바인딩으로 컴파일하는 규칙을 앱 타깃에 연결한다.
# 여기서 만들어지는 코드는 레이아웃/컨트롤 코드이며 PNG/ICO 파일의 런타임 로드와는 별개다.
function(quietmic_generate_window target)
    # Slint's SDK uses this variable for generated paths. Keep the override
    # local while registering its custom command in the app target's directory.
    # A child directory's custom command would not be attached to the app.
    set(CMAKE_CURRENT_BINARY_DIR "${PROJECT_BINARY_DIR}/generated/ui")
    # 함수 안의 경로 변경은 호출자 전체의 빌드 경로를 바꾸지 않는다.
    # 생성 규칙은 기존 타깃의 디렉터리에 등록하고 결과물 위치만 generated/ui로 모은다.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    slint_target_sources(${target} "${PROJECT_SOURCE_DIR}/ui/quietmic_window.slint"
        NAMESPACE qm::ui::generated)
endfunction()
