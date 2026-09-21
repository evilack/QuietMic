// QuietMic 설치 프로그램이 호출하는 드라이버 관리 도우미다.
// 일반 사용자 기능과 진단 코드는 데스크톱 앱에 두고, 이 실행 파일은 설치·제거에 필요한
// 최소 명령만 제공한다. 각 명령은 정확한 인수 개수를 검사해 잘못된 호출을 거부한다.
#include "setup.hpp"
#include "transport_setup.hpp"

#include <filesystem>
#include <iostream>
#include <string>

using namespace qm;

// Windows 설치 프로그램이 유니코드 경로를 손실 없이 전달할 수 있도록 wchar_t 인수를 받는다.
int wmain(int argc, wchar_t **argv)
{
    try
    {
        // 설치·제거 전에 실행 중인 QuietMic이 없는지 확인한다. 실행 중이면 설치 프로그램이
        // 사용자에게 앱을 먼저 종료하도록 안내할 수 있도록 실패 코드를 반환한다.
        if (argc == 2 && std::wstring(argv[1]) == L"--require-app-stopped")
        {
            require_app_stopped();
            return 0;
        }

        // 지정한 드라이버 폴더를 절대 경로로 정규화한 뒤 서명과 파일 구성을 확인하고 설치한다.
        // 관리자 권한은 이 도우미가 우회하지 않으며, 관리자 권한으로 실행된 설치 프로그램이 제공한다.
        if (argc == 3 && std::wstring(argv[1]) == L"--install-audio")
        {
            require_app_stopped();
            return usbip::setup_transport(std::filesystem::absolute(argv[2]));
        }

        // 제거도 앱 종료를 먼저 확인한다. 전송 서비스와 드라이버를 정리한 결과를 그대로 반환한다.
        if (argc == 2 && std::wstring(argv[1]) == L"--remove-audio")
        {
            require_app_stopped();
            return usbip::remove_transport();
        }

        // 패키징 단계에서 설치 없이 Microsoft 카탈로그 서명과 드라이버 파일을 검증한다.
        if (argc == 3 && std::wstring(argv[1]) == L"--verify-driver")
        {
            usbip::verify_transport_package(std::filesystem::absolute(argv[2]));
            std::cout << "Driver catalog and payload verification passed. No installation performed.\n";
            return 0;
        }

        // 명령 없이 실행하면 설치 도우미임을 알리고 성공으로 끝낸다. 알 수 없는 인수는
        // 설치 스크립트가 오타를 성공으로 오인하지 않도록 사용 오류 코드 2를 반환한다.
        std::cout << "QuietMic installer helper\n"
                     "  --verify-driver package-directory\n"
                     "  --require-app-stopped\n"
                     "  --install-audio package-directory\n"
                     "  --remove-audio\n";
        return argc == 1 ? 0 : 2;
    }
    catch (const std::exception &error)
    {
        // 예외 메시지는 설치 로그의 원인 설명으로 사용되며, 1은 명령 실행 실패를 뜻한다.
        std::cerr << error.what() << '\n';
        return 1;
    }
}
