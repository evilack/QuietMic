// 실행 옵션에 따라 설치 도우미 또는 일반 UI로 분기하고, 중복 실행과 이벤트 루프의 시작을 관리한다.
#include "application.hpp"
#include "app_controller.hpp"
#include "setup.hpp"
#include "transport_setup.hpp"

namespace qm
{
// UI 객체를 만들기 전에 실행 목적부터 결정한다. 설치 전용 실행은 화면과 엔진을 만들지 않고 끝난다.
int run_application(const std::vector<std::wstring> &arguments)
{
    // 실행 파일 기준으로 드라이버/이미지 경로를, 사용자 폴더 기준으로 설정 경로를 구한다.
    const auto paths = platform::ApplicationPaths::current();
    // 관리자 권한으로 재실행된 설치 전용 경로다. 실패는 기록하고 Windows 설치 실패 코드를 돌려준다.
    if (arguments.size() == 1 && arguments.front() == L"--setup-audio")
    {
        try
        {
            return usbip::setup_transport(paths.driver_package);
        }
        catch (const std::exception &error)
        {
            platform::report_setup_failure(error.what());
            return ERROR_INSTALL_FAILURE;
        }
    }
    // 장치 이름 지정 경로다. 두 번째 인수만 UTF-8로 변환하여 전달한다.
    if (arguments.size() == 2 && arguments.front() == L"--name-output")
    {
        name_output(utf8(arguments[1]));
        return 0;
    }
    // 로그인 자동 실행은 인수가 정확히 하나일 때만 인정한다. 알 수 없는 옵션은 오류로 처리한다.
    const bool login = arguments.size() == 1 && arguments.front() == L"--startup";
    if (!arguments.empty() && !login)
        throw std::invalid_argument("Unknown QuietMic launch option");
    // 설치 작업과 충돌하지 않는지 검사한 뒤 이름 있는 뮤텍스로 중복 실행을 확인한다.
    require_no_installer();
    platform::UniqueHandle instance(CreateMutexW(nullptr, FALSE, L"Local\\QuietMic.Instance"));
    // 다른 API가 마지막 오류를 덮기 전에 뮤텍스 생성 직후의 값을 보관한다.
    const auto error = GetLastError();
    if (!instance.get())
        throw std::runtime_error("Cannot create application instance lock");
    // 이미 실행 중이면 기존 트레이 창에 표시 요청만 보내고 새 프로세스는 끝낸다.
    if (error == ERROR_ALREADY_EXISTS)
    {
        Tray::show_existing();
        return 0;
    }
    // Slint 백엔드를 사용자가 지정하지 않았을 때만 기본 소프트웨어 렌더러를 선택한다.
    if (!GetEnvironmentVariableW(L"SLINT_BACKEND", nullptr, 0))
        SetEnvironmentVariableW(L"SLINT_BACKEND", L"winit-software");
    // 공유 소유권을 만든 뒤 콜백을 연결하는 팩터리다.
    auto app = AppController::create(paths);
    // 여기서 UI 이벤트 루프를 실행하고, 종료 요청이 오면 컨트롤러의 종료 코드를 반환한다.
    return app->run(login);
}
} // namespace qm
