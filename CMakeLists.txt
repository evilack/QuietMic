# 빌드의 시작점: 공통 런타임 → 진단 CLI/드라이버 → 데스크톱 앱 순으로 의존성을 선언한다.
# CMake는 이 정의로 VS 프로젝트를 생성하며 실제 C++ 실행 로직은 src 아래에 있다.
cmake_minimum_required(VERSION 3.24)
project(QuietMic VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake")

option(QUIETMIC_BUILD_APP "Build the Slint desktop app" ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()

configure_file(cmake/build_info.hpp.in generated/build_info.hpp @ONLY)
# configure_file은 버전 값만 치환한다. UI 이미지 에셋을 코드로 바꾸는 단계가 아니다.
configure_file(cmake/version_info.h.in generated/version_info.h @ONLY)

# The desktop app and installer helper share one implementation library. RNNoise
# is compiled into that library, so it does not add another solution project.
set(RN third_party/rnnoise)
set(runtime_sources
    src/audio/audio.cpp src/audio/audio.hpp src/audio/audio_policy.hpp
    src/audio/device_watcher.cpp src/audio/device_watcher.hpp
    src/audio/preview_buffer.hpp
    src/audio/audio_frame_sink.hpp src/audio/pcm_output_buffer.hpp
    src/usbip/usb_audio_server.cpp src/usbip/usb_audio_server.hpp
    src/usbip/usb_audio_device.cpp src/usbip/usb_audio_device.hpp
    src/usbip/usbip_session.cpp src/usbip/usbip_session.hpp
    src/usbip/transport_setup.cpp src/usbip/transport_setup.hpp
    src/audio/dsp.cpp src/audio/dsp.hpp
    src/config/settings.cpp src/config/settings.hpp
    src/config/parameters.cpp src/config/parameters.hpp
    src/localization/catalog.cpp src/localization/catalog.hpp
    src/app/device_catalog.cpp src/app/device_catalog.hpp
    src/platform/text_encoding.cpp src/platform/text_encoding.hpp
    src/driver/setup.cpp src/driver/setup.hpp
    src/driver/driver_transport.cpp src/driver/driver_transport.hpp
    src/driver/driver_trust.cpp src/driver/driver_trust.hpp
    src/shared/driver_protocol.hpp src/shared/audio_format.hpp)
add_library(QuietMicRuntime STATIC ${runtime_sources}
    "${PROJECT_BINARY_DIR}/generated/build_info.hpp"
    "${PROJECT_BINARY_DIR}/generated/version_info.h"
    ${RN}/src/denoise.c ${RN}/src/rnn.c ${RN}/src/rnn_data.c
    ${RN}/src/rnn_reader.c ${RN}/src/pitch.c ${RN}/src/kiss_fft.c ${RN}/src/celt_lpc.c)
target_include_directories(QuietMicRuntime
    PUBLIC src src/audio src/config src/driver src/shared src/platform src/app src/usbip src/localization
        "${PROJECT_BINARY_DIR}/generated"
    PRIVATE ${RN}/include ${RN}/src)
# PUBLIC 설정은 이 라이브러리를 쓰는 앱/CLI/검사에도 전달된다.
# PRIVATE인 RNNoise 내부 헤더는 외부 코드가 직접 의존하지 않도록 숨긴다.
target_compile_definitions(QuietMicRuntime
    PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    PRIVATE RNNOISE_BUILD _USE_MATH_DEFINES)
target_link_libraries(QuietMicRuntime PUBLIC
    ole32 propsys uuid avrt setupapi newdev bcrypt shell32 advapi32 wintrust crypt32 ws2_32 cfgmgr32)
set_target_properties(QuietMicRuntime PROPERTIES FOLDER "Libraries")
source_group(TREE "${PROJECT_SOURCE_DIR}/src" PREFIX "Source" FILES ${runtime_sources})
source_group("RNNoise" REGULAR_EXPRESSION "third_party/rnnoise/.*")
source_group("Generated" FILES
    "${PROJECT_BINARY_DIR}/generated/build_info.hpp" "${PROJECT_BINARY_DIR}/generated/version_info.h")

add_executable(quietmic_cli src/tools/cli.cpp)
target_link_libraries(quietmic_cli PRIVATE QuietMicRuntime)
set_target_properties(quietmic_cli PROPERTIES FOLDER "Tools")


if(QUIETMIC_BUILD_APP)
    # 설치된 Slint SDK를 찾고, 손으로 관리하는 코드/UI/Windows 리소스를 타깃에 묶는다.
    if(NOT Slint_DIR AND EXISTS "${PROJECT_SOURCE_DIR}/.deps/slint/lib/cmake/Slint/SlintConfig.cmake")
        set(Slint_DIR "${PROJECT_SOURCE_DIR}/.deps/slint/lib/cmake/Slint" CACHE PATH "Slint C++ SDK CMake package")
    endif()
    find_package(Slint 1.17.1 EXACT REQUIRED)
    set(app_sources
        src/main.cpp
        src/app/application.cpp src/app/application.hpp
        src/app/app_controller.cpp src/app/app_controller.hpp
        src/app/ui_bindings.cpp
        src/ui/main_view.cpp src/ui/main_view.hpp src/ui/main_view_impl.hpp
        src/platform/windows_services.cpp src/platform/windows_services.hpp
        src/platform/tray.cpp src/platform/tray.hpp)
    set(ui_sources
        ui/quietmic_window.slint ui/volume_window.slint ui/types.slint ui/localization.slint ui/theme.slint
        ui/components/controls.slint ui/components/parameter-row.slint
        ui/pages/microphone.slint ui/pages/filter.slint ui/pages/settings.slint)
    set(runtime_assets
        assets/icons/idle.png assets/icons/running.png assets/icons/muted.png assets/icons/error.png
        assets/icons/idle.ico assets/icons/running.ico assets/icons/muted.ico assets/icons/error.ico)
    set(localization_assets
        assets/i18n/en.lang assets/i18n/ko.lang assets/i18n/ja.lang assets/i18n/zh-Hans.lang
        assets/i18n/zh-Hant.lang assets/i18n/es.lang assets/i18n/fr.lang assets/i18n/de.lang)
    set(app_resources
        assets/app.rc assets/app.manifest assets/app.ico
        assets/resource_ids.h assets/tray.rc
        ${runtime_assets} ${localization_assets})
    # These fragments are included by app.rc, not compiled a second time.
    set_source_files_properties(assets/tray.rc PROPERTIES HEADER_FILE_ONLY TRUE)
    add_executable(QuietMic WIN32 ${app_sources} ${ui_sources} ${app_resources})
    target_include_directories(QuietMic PRIVATE assets src/ui)
    target_link_libraries(QuietMic PRIVATE QuietMicRuntime Slint::Slint shell32 advapi32 user32)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT QuietMic)
    set_target_properties(QuietMic PROPERTIES
        SLINT_EMBED_RESOURCES "as-absolute-path"
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/$<CONFIG>"
        VS_DEBUGGER_COMMAND "$<TARGET_FILE:QuietMic>"
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:QuietMic>"
        VS_DEBUGGER_ENVIRONMENT "SLINT_BACKEND=winit-software")
    source_group(TREE "${PROJECT_SOURCE_DIR}/src" PREFIX "Source" FILES ${app_sources})
    source_group(TREE "${PROJECT_SOURCE_DIR}/ui" PREFIX "UI" FILES ${ui_sources})
    source_group(TREE "${PROJECT_SOURCE_DIR}/assets" PREFIX "Resources" FILES ${app_resources})
    include(cmake/QuietMicUi.cmake)
    quietmic_generate_window(QuietMic)
    source_group("Generated/Slint (auto-generated, do not edit)" FILES
        "${PROJECT_BINARY_DIR}/generated/ui/quietmic_window.h"
        "${PROJECT_BINARY_DIR}/generated/ui/slint_generated_quietmic_window_1.cpp")
    # File outputs make both changed source images and missing runtime copies
    # invalidate the copy rule, even when no C++ file needs recompiling.
    list(TRANSFORM runtime_assets PREPEND "${PROJECT_SOURCE_DIR}/" OUTPUT_VARIABLE runtime_asset_sources)
    # 원본 이미지와 복사될 파일 양쪽을 빌드 의존성으로 기록한다.
    # 이미지 수정/복사본 삭제만으로도 복사 규칙이 다시 실행되며 C++ 재컴파일은 필요 없다.
    list(TRANSFORM runtime_assets PREPEND "${PROJECT_BINARY_DIR}/$<CONFIG>/" OUTPUT_VARIABLE runtime_asset_outputs)
    set(runtime_asset_directory "${PROJECT_BINARY_DIR}/$<CONFIG>/assets/icons")
    set(runtime_asset_stamp "${PROJECT_BINARY_DIR}/generated/runtime-assets-$<CONFIG>.stamp")
    add_custom_command(OUTPUT "${runtime_asset_stamp}" ${runtime_asset_outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${runtime_asset_directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${runtime_asset_sources} "${runtime_asset_directory}"
        COMMAND ${CMAKE_COMMAND} -E touch "${runtime_asset_stamp}"
        DEPENDS ${runtime_asset_sources}
        COMMENT "Copying external PNG and ICO runtime assets"
        VERBATIM)
    target_sources(QuietMic PRIVATE "${runtime_asset_stamp}")
    # 번역 파일도 이미지와 같은 외부 에셋이지만 별도 폴더에 복사해 역할과 갱신 의존성을 분리한다.
    list(TRANSFORM localization_assets PREPEND "${PROJECT_SOURCE_DIR}/" OUTPUT_VARIABLE localization_asset_sources)
    list(TRANSFORM localization_assets PREPEND "${PROJECT_BINARY_DIR}/$<CONFIG>/" OUTPUT_VARIABLE localization_asset_outputs)
    set(localization_asset_directory "${PROJECT_BINARY_DIR}/$<CONFIG>/assets/i18n")
    set(localization_asset_stamp "${PROJECT_BINARY_DIR}/generated/localization-assets-$<CONFIG>.stamp")
    add_custom_command(OUTPUT "${localization_asset_stamp}" ${localization_asset_outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${localization_asset_directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${localization_asset_sources} "${localization_asset_directory}"
        COMMAND ${CMAKE_COMMAND} -E touch "${localization_asset_stamp}"
        DEPENDS ${localization_asset_sources}
        COMMENT "Copying external localization catalogs"
        VERBATIM)
    target_sources(QuietMic PRIVATE "${localization_asset_stamp}")
    source_group("Generated/Build rules" REGULAR_EXPRESSION "runtime-assets-.*[.]stamp")
    add_dependencies(QuietMic quietmic_cli)
    # Microsoft 서명 드라이버와 통신하는 원본 사용자 모드 클라이언트만 앱 옆에 둔다.
    # 전송 드라이버의 설치/업데이트는 일반 앱 실행과 분리된 설치 단계의 책임이다.
    set(usbip_runtime_sources
        "${PROJECT_SOURCE_DIR}/third_party/usbip-win2/runtime/usbip.exe"
        "${PROJECT_SOURCE_DIR}/third_party/usbip-win2/runtime/libusbip.dll"
        "${PROJECT_SOURCE_DIR}/third_party/usbip-win2/runtime/resources.dll")
    # 자식 실행 파일은 상위 앱 폴더의 CRT를 자동으로 찾지 못한다.
    # 실제 import에 필요한 세 VC 런타임을 클라이언트 옆에 함께 둔다.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    set(app_crt_sources)
    foreach(runtime IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        get_filename_component(runtime_name "${runtime}" NAME)
        string(TOLOWER "${runtime_name}" runtime_name)
        if(runtime_name MATCHES "^(msvcp140|msvcp140_atomic_wait|vcruntime140|vcruntime140_1)[.]dll$")
            list(APPEND app_crt_sources "${runtime}")
        endif()
        if(runtime_name MATCHES "^(msvcp140|vcruntime140|vcruntime140_1)[.]dll$")
            list(APPEND usbip_runtime_sources "${runtime}")
        endif()
    endforeach()
    set(app_crt_outputs)
    foreach(runtime IN LISTS app_crt_sources)
        get_filename_component(runtime_name "${runtime}" NAME)
        list(APPEND app_crt_outputs "${PROJECT_BINARY_DIR}/$<CONFIG>/${runtime_name}")
    endforeach()
    set(app_crt_stamp "${PROJECT_BINARY_DIR}/generated/app-crt-$<CONFIG>.stamp")
    add_custom_command(OUTPUT "${app_crt_stamp}" ${app_crt_outputs}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${app_crt_sources} "${PROJECT_BINARY_DIR}/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E touch "${app_crt_stamp}"
        DEPENDS ${app_crt_sources} VERBATIM)
    target_sources(QuietMic PRIVATE "${app_crt_stamp}")
    source_group("Generated/Build rules" REGULAR_EXPRESSION "app-crt-.*[.]stamp")
    set(usbip_runtime_directory "${PROJECT_BINARY_DIR}/$<CONFIG>/components/usbip")
    set(usbip_runtime_outputs)
    foreach(runtime IN LISTS usbip_runtime_sources)
        get_filename_component(runtime_name "${runtime}" NAME)
        list(APPEND usbip_runtime_outputs "${usbip_runtime_directory}/${runtime_name}")
    endforeach()
    set(usbip_runtime_stamp "${PROJECT_BINARY_DIR}/generated/usbip-runtime-$<CONFIG>.stamp")
    add_custom_command(OUTPUT "${usbip_runtime_stamp}" ${usbip_runtime_outputs}
            "${usbip_runtime_directory}/LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${usbip_runtime_directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${usbip_runtime_sources} "${usbip_runtime_directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/licenses/USBIP-BSD-2-Clause.txt"
            "${usbip_runtime_directory}/LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E touch "${usbip_runtime_stamp}"
        DEPENDS ${usbip_runtime_sources} "${PROJECT_SOURCE_DIR}/licenses/USBIP-BSD-2-Clause.txt"
        COMMENT "Copying USB/IP client and its app-local runtime"
        VERBATIM)
    target_sources(QuietMic PRIVATE "${usbip_runtime_stamp}")
    source_group("Generated/Build rules" REGULAR_EXPRESSION "usbip-runtime-.*[.]stamp")
    # 원본 Microsoft 서명 패키지는 이름/내용을 바꾸지 않고 별도 폴더에 복사한다.
    # 자체 커널 프로젝트나 WDK 빌드를 제품 의존성으로 추가하지 않는다.
    set(transport_sources)
    set(transport_outputs)
    foreach(base usbip2_filter usbip2_ude)
        foreach(extension inf sys cat)
            list(APPEND transport_sources "${PROJECT_SOURCE_DIR}/third_party/usbip-win2/driver-packages/${base}.${extension}")
            list(APPEND transport_outputs "${PROJECT_BINARY_DIR}/$<CONFIG>/components/usbip-drivers/${base}.${extension}")
        endforeach()
    endforeach()
    set(transport_stamp "${PROJECT_BINARY_DIR}/generated/usbip-drivers-$<CONFIG>.stamp")
    add_custom_command(OUTPUT "${transport_stamp}" ${transport_outputs}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/$<CONFIG>/components/usbip-drivers"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${transport_sources}
            "${PROJECT_BINARY_DIR}/$<CONFIG>/components/usbip-drivers"
        COMMAND ${CMAKE_COMMAND} -E touch "${transport_stamp}"
        DEPENDS ${transport_sources} VERBATIM)
    target_sources(QuietMic PRIVATE "${transport_stamp}")
    source_group("Generated/Build rules" REGULAR_EXPRESSION "usbip-drivers-.*[.]stamp")
    add_custom_command(TARGET QuietMic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:slint_cpp-shared> $<TARGET_FILE_DIR:QuietMic>
        VERBATIM)
endif()

