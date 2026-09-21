// Windows GUI 실행 파일의 시작점. 시작 절차를 응용 계층으로 넘기고 밖으로 나온 예외를 마지막에 표시한다.
#include "app/application.hpp"
#include "platform/windows_services.hpp"
#include "platform/text_encoding.hpp"
#include <exception>

// Windows가 호출하는 진입 함수다. 일반 콘솔 프로그램의 main과 달리 콘솔 창을 요구하지 않는다.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    try
    {
        // 실행 파일 이름을 제외한 인수 목록을 만들어 일반 실행과 설치 경로를 선택하게 한다.
        return qm::run_application(qm::platform::command_line());
    }
    catch (const std::exception &error)
    {
        // UTF-8 예외 메시지를 Windows의 UTF-16 문자열로 변환한다. 반환값 1은 실행 실패를 뜻한다.
        MessageBoxW(nullptr, qm::wide(error.what()).c_str(), L"QuietMic", MB_OK | MB_ICONERROR);
        return 1;
    }
}
