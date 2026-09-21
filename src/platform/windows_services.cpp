// Windows API 호출과 핸들/메모리 수명 관리를 모아 UI 및 컨트롤러에서 분리한다.
#include "windows_services.hpp"
#include "text_encoding.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <stdexcept>

namespace qm::platform
{
// 현재 작업 폴더에 의존하지 않고 실행 파일 경로와 사용자 LocalAppData를 기준으로 필요한 경로를 만든다.
ApplicationPaths ApplicationPaths::current()
{
    // Windows 실행 파일 경로를 받을 큰 버퍼를 준비한다. 0 또는 버퍼가 가득 찬 결과는 실패/잘림으로 처리한다.
    std::wstring filename(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
    if (!length || length == filename.size())
        throw std::runtime_error("Executable path unavailable");
    filename.resize(length);
    // Known Folder API가 할당한 문자열을 받을 포인터다. 성공 뒤 아래 지역 소유자가 CoTaskMemFree로 해제한다.
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        throw std::runtime_error("LocalAppData unavailable");
    struct Free
    {
        PWSTR value;
        ~Free()
        {
            CoTaskMemFree(value);
        }
        // 지역 소유자의 소멸자는 정상 반환뿐 아니라 이후 경로 구성 중 예외가 나도 실행된다.
    } owner{local};
    ApplicationPaths paths;
    paths.executable = filename;
    // 설정은 사용자마다 다른 폴더에 두고, 배포된 드라이버와 이미지는 실행 파일 옆에서 찾는다.
    paths.settings = std::filesystem::path(local) / L"QuietMic" / L"settings.ini";
    paths.driver_package = paths.executable.parent_path() / L"components" / L"usbip-drivers";
    paths.assets = paths.executable.parent_path() / L"assets";
    return paths;
}
// Windows 규칙대로 따옴표 등을 해석하여 명령행을 인수 목록으로 만든다.
std::vector<std::wstring> command_line()
{
    int count = 0;
    auto values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values)
        throw std::runtime_error("Cannot read command line");
    struct Free
    {
        LPWSTR *value;
        ~Free()
        {
            LocalFree(value);
        }
        // CommandLineToArgvW의 메모리는 LocalFree로 해제한다. 반환 벡터는 문자열을 복사하므로 이후에도 유효하다.
    } owner{values};
    // 첫 원소는 실행 파일 이름이므로 건너뛰고 나머지 인수만 반환한다.
    return {values + 1, values + count};
}
// 현재 사용자 Run 레지스트리 값으로 로그인 자동 실행을 켜거나 끈다. 다른 사용자 계정은 건드리지 않는다.
void set_startup(bool enabled, const std::filesystem::path &executable)
{
    HKEY key = nullptr;
    auto error = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                                 nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (error != ERROR_SUCCESS)
        throw std::runtime_error("Cannot open login startup settings");
    struct Close
    {
        HKEY key;
        ~Close()
        {
            RegCloseKey(key);
        }
        // 레지스트리 키는 일반 CloseHandle이 아니라 RegCloseKey로 정리해야 한다.
    } owner{key};
    if (enabled)
    {
        // 공백이 있는 실행 파일 경로도 하나의 경로로 해석되도록 따옴표로 감싸고 로그인 실행 옵션을 붙인다.
        const auto command = L"\"" + executable.wstring() + L"\" --startup";
        error = RegSetValueExW(key, L"QuietMic", 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                               // REG_SZ의 크기는 바이트 단위이며 끝의 널 문자까지 포함한다.
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        error = RegDeleteValueW(key, L"QuietMic");
        // 이미 꺼진 상태에서 다시 끄는 요청도 성공으로 취급한다.
        if (error == ERROR_FILE_NOT_FOUND)
            error = ERROR_SUCCESS;
    }
    if (error != ERROR_SUCCESS)
        throw std::runtime_error("Cannot update login startup settings");
}
// 같은 실행 파일을 관리자 권한 설치 모드로 실행하고, 나중에 종료를 확인할 프로세스 핸들을 얻는다.
void SetupProcess::start(const std::filesystem::path &executable)
{
    if (active())
        throw std::logic_error("Setup operation already active");
    SHELLEXECUTEINFOW info{sizeof(info)};
    // SEE_MASK_NOCLOSEPROCESS로 프로세스 핸들을 요청한다. runas는 Windows 권한 상승 절차를 사용한다.
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = executable.c_str();
    info.lpParameters = L"--setup-audio";
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess)
        throw std::runtime_error("Automatic setup elevation was cancelled or could not start");
    // 실행 성공 후 반환된 프로세스 핸들의 정리를 UniqueHandle에 맡긴다.
    process_.reset(info.hProcess);
}
// UI 타이머에서 호출할 수 있도록 기다림 시간을 0으로 지정하여 완료 여부만 확인한다.
std::optional<DWORD> SetupProcess::poll()
{
    if (!active())
        return std::nullopt;
    // WAIT_TIMEOUT은 아직 실행 중이라는 뜻이다. 그 외 예상하지 못한 대기 결과는 오류로 처리한다.
    const auto wait = WaitForSingleObject(process_.get(), 0);
    if (wait == WAIT_TIMEOUT)
        return std::nullopt;
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error("Cannot wait for setup process");
    // 종료가 확인된 뒤 종료 코드를 읽는다. 성공적으로 읽었으면 핸들을 닫아 active 상태를 해제한다.
    DWORD result = ERROR_INSTALL_FAILURE;
    if (!GetExitCodeProcess(process_.get(), &result))
        throw std::runtime_error("Cannot read setup process result");
    process_.reset();
    return result;
}
// 별도의 대화상자 대신 디버그 출력과 Windows 이벤트 로그에 설치 오류를 기록한다.
void report_setup_failure(const std::string &error)
{
    OutputDebugStringA(error.c_str());
    auto source = RegisterEventSourceW(nullptr, L"QuietMic");
    // 이벤트 로그 등록이 실패하면 디버그 출력만 남긴 채 돌아간다.
    if (!source)
        return;
    const auto text = wide(error);
    // ReportEventW가 요구하는 문자열 포인터 배열로 오류 문구를 전달한 뒤 이벤트 소스 핸들을 해제한다.
    LPCWSTR strings[]{text.c_str()};
    ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1001, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}
// Windows 셸의 기본 연결 프로그램으로 URL을 연다. 반환값 32 이하는 ShellExecute의 오류 범위다.
void open_url(const wchar_t *url)
{
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) <=
        32)
        throw std::runtime_error("Cannot open the link");
}
} // namespace qm::platform
