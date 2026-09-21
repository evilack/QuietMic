#include "transport_setup.hpp"
#include "driver_trust.hpp"
#include "usb_audio_device.hpp"
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <sddl.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <string>
#include <cwchar>
namespace qm::usbip
{
namespace
{
constexpr wchar_t hardware[] = L"ROOT\\USBIP_WIN2\\UDE\0";
struct Payload
{
    const wchar_t *name;
    const char *sha256;
};
// upstream v.0.9.8.0 원본의 핀이다. 핀과 Windows 서명을 모두 통과해야 한다.
constexpr Payload payload[] = {
    {L"usbip2_filter.cat", "BB2183D204B64EAA52C6EAC86C8CFB859AD0A5A5001AF5E6B197BF04CC49AC2E"},
    {L"usbip2_filter.inf", "A1F07BBF5CFA7734A52046534DD66EE2333D34AA53912356179A6EAF25D1D60E"},
    {L"usbip2_filter.sys", "671110E3FE09628D3D7F416B5EE35172ECF5046F90BE6E8735631105D38FDEC5"},
    {L"usbip2_ude.cat", "99C6807A2D05FD01A689389BE81D5088BB4777831ADB3C7E119B14A1F5B088C7"},
    {L"usbip2_ude.inf", "D975AC1AE14246611824518CEB05FA45227176D8E366366220F58A7AB977C379"},
    {L"usbip2_ude.sys", "D4D98DB62D78A5B6D32EEC3F8F1F9F6B0D2EDA925A56C96367C5B7985AB52CD8"}};
void require(BOOL ok, const char *operation)
{
    if (!ok)
        throw std::runtime_error(std::string(operation) + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}
struct Handle
{
    HANDLE value;
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct ServiceHandle
{
    SC_HANDLE value;
    ~ServiceHandle()
    {
        if (value)
            CloseServiceHandle(value);
    }
};
struct DeviceSet
{
    HDEVINFO value;
    ~DeviceSet()
    {
        if (value != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(value);
    }
};

// Windows에 기본 포함된 PnPUtil을 숨김 상태로 실행해 특정 장치 인스턴스만 제거한다.
//
// UDE 컨트롤러의 PnP 제거가 Windows 내부에서 반환되지 않는 상태가 생겨도 제거
// 프로그램 전체를 붙잡지 않도록 별도 프로세스에서 실행하고 제한 시간을 둔다.
// 호출자는 정확한 하드웨어 ID와 빈 자식 트리를 확인한 장치 ID만 전달한다.
bool remove_device_with_pnputil(const std::wstring &instance_id)
{
    if (instance_id.empty() || instance_id.find_first_of(L"\"\r\n") != std::wstring::npos)
        throw std::runtime_error("USB/IP 장치 인스턴스 식별자가 올바르지 않습니다.");

    wchar_t system_directory[MAX_PATH]{};
    require(GetSystemDirectoryW(system_directory, MAX_PATH) != 0, "Read Windows system directory");
    const std::filesystem::path executable = std::filesystem::path(system_directory) / L"pnputil.exe";

    // CreateProcess는 수정 가능한 명령줄 버퍼를 요구한다. 실행 파일 경로와 장치 ID를
    // 각각 따옴표로 감싸고, 위에서 따옴표와 줄바꿈을 거부해 인수 삽입을 차단한다.
    std::wstring command = L"\"" + executable.wstring() + L"\" /remove-device \"" +
                           instance_id + L"\" /subtree";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process),
            "Start Windows device removal");
    Handle process_handle{process.hProcess};
    Handle thread_handle{process.hThread};

    constexpr DWORD removal_timeout_ms = 30'000;
    const DWORD wait = WaitForSingleObject(process_handle.value, removal_timeout_ms);
    if (wait == WAIT_TIMEOUT)
    {
        // 패키지 삭제는 시작하지 않는다. 설치 파일과 등록을 보존해 재부팅 후 같은
        // 언인스톨러로 안전하게 다시 시도할 수 있게 한다.
        TerminateProcess(process_handle.value, ERROR_TIMEOUT);
        WaitForSingleObject(process_handle.value, 5'000);
        throw std::runtime_error(
            "USB/IP 호스트 컨트롤러 제거가 30초 안에 끝나지 않았습니다. 재부팅 후 다시 제거하세요.");
    }
    require(wait == WAIT_OBJECT_0, "Wait for Windows device removal");

    DWORD exit_code = ERROR_GEN_FAILURE;
    require(GetExitCodeProcess(process_handle.value, &exit_code), "Read Windows device removal result");
    if (exit_code != ERROR_SUCCESS && exit_code != ERROR_SUCCESS_REBOOT_REQUIRED)
        throw std::runtime_error("Windows가 USB/IP 호스트 컨트롤러를 제거하지 못했습니다. 종료 코드 " +
                                 std::to_string(exit_code) + "입니다.");
    return exit_code == ERROR_SUCCESS_REBOOT_REQUIRED;
}
std::string file_hash(const std::filesystem::path &file)
{
    const auto attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        throw std::runtime_error("USB/IP 패키지 파일이 없거나 일반 파일이 아닙니다.");
    Handle handle{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    require(handle.value != INVALID_HANDLE_VALUE, "Open transport payload");
    LARGE_INTEGER length{};
    require(GetFileSizeEx(handle.value, &length), "Read payload size");
    if (length.QuadPart <= 0 || length.QuadPart > 64 * 1024 * 1024)
        throw std::runtime_error("USB/IP 패키지 파일 크기가 올바르지 않습니다.");
    std::vector<UCHAR> bytes(static_cast<size_t>(length.QuadPart));
    DWORD read = 0;
    require(ReadFile(handle.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr),
            "Read transport payload");
    if (read != bytes.size())
        throw std::runtime_error("USB/IP 패키지 파일을 끝까지 읽지 못했습니다.");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA-256 provider unavailable");
    std::array<UCHAR, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(), read, digest.data(),
                                   static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        throw std::runtime_error("SHA-256 verification failed");
    std::string result;
    for (auto byte : digest)
    {
        result += "0123456789ABCDEF"[byte >> 4];
        result += "0123456789ABCDEF"[byte & 15];
    }
    return result;
}
struct ServiceState
{
    bool exists = false;
    bool running = false;
    std::filesystem::path directory;
};
ServiceState service_state(const wchar_t *name)
{
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    require(manager.value != nullptr, "Open service manager");
    ServiceHandle service{OpenServiceW(manager.value, name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS)};
    if (!service.value)
    {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            return {};
        require(FALSE, "Read USB/IP service");
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    require(QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status),
                                 sizeof(status), &needed),
            "Read USB/IP service state");
    QueryServiceConfigW(service.value, nullptr, 0, &needed);
    require(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "Read USB/IP service configuration size");
    std::vector<BYTE> buffer(needed);
    auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
    require(QueryServiceConfigW(service.value, config, needed, &needed), "Read USB/IP service configuration");
    if (config->dwServiceType != SERVICE_KERNEL_DRIVER)
        throw std::runtime_error("USB/IP 서비스가 커널 드라이버 서비스가 아닙니다.");
    std::wstring path = config->lpBinaryPathName;
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
        path = path.substr(1, path.size() - 2);
    wchar_t windows[MAX_PATH]{};
    require(GetWindowsDirectoryW(windows, MAX_PATH) != 0, "Read Windows directory");
    if (path.starts_with(L"\\SystemRoot\\"))
        path = std::wstring(windows) + path.substr(11);
    else if (path.starts_with(L"\\??\\"))
        path.erase(0, 4);
    else if (path.starts_with(L"System32\\") || path.starts_with(L"system32\\"))
        path = std::wstring(windows) + L"\\" + path;
    const DWORD expanded_size = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    require(expanded_size != 0, "Expand driver path");
    std::wstring expanded(expanded_size, L'\0');
    require(ExpandEnvironmentStringsW(path.c_str(), expanded.data(), expanded_size) == expanded_size,
            "Expand driver path");
    expanded.resize(expanded_size - 1);
    const std::filesystem::path binary(expanded);
    if (!binary.is_absolute() || _wcsicmp(binary.filename().c_str(), (std::wstring(name) + L".sys").c_str()))
        throw std::runtime_error("설치된 USB/IP 드라이버 경로가 예상과 다릅니다.");
    return {true, status.dwCurrentState == SERVICE_RUNNING, binary.parent_path()};
}

// 실행 중이던 커널 드라이버 서비스는 패키지를 정상 제거해도 서비스 객체가 즉시
// 사라지지 않고 DeleteFlag=1로 다음 부팅까지 남을 수 있다. 이 상태는 제거 실패가
// 아니라 Windows가 재부팅 시 마지막 참조를 정리하겠다는 뜻이므로 3010으로 전달한다.
bool service_delete_pending(const wchar_t *name)
{
    const std::wstring path = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + name;
    HKEY raw = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_QUERY_VALUE, &raw);
    if (opened == ERROR_FILE_NOT_FOUND)
        return false;
    if (opened != ERROR_SUCCESS)
        throw std::runtime_error("USB/IP 서비스 제거 상태를 읽지 못했습니다. Windows error " +
                                 std::to_string(opened));
    struct RegistryKey
    {
        HKEY value;
        ~RegistryKey()
        {
            RegCloseKey(value);
        }
    } key{raw};

    DWORD value = 0, type = 0, size = sizeof(value);
    const LSTATUS queried = RegQueryValueExW(key.value, L"DeleteFlag", nullptr, &type,
                                             reinterpret_cast<BYTE *>(&value), &size);
    if (queried == ERROR_FILE_NOT_FOUND)
        return false;
    if (queried != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value))
        throw std::runtime_error("USB/IP 서비스 DeleteFlag를 읽지 못했습니다.");
    return value == 1;
}
void verify_installed(const ServiceState &state, const wchar_t *name, const std::array<unsigned, 4> &minimum)
{
    if (!state.exists)
        throw std::runtime_error("USB/IP 드라이버가 없습니다. QuietMic 설치 프로그램을 실행하세요.");
    verify_catalog_driver(state.directory, name, true);
    // 기존 최신 버전을 보존한다. 제품 버전이 아닌 서명된 INF DriverVer를 비교한다.
    HINF inf = SetupOpenInfFileW((state.directory / (std::wstring(name) + L".inf")).c_str(), nullptr,
                                 INF_STYLE_WIN4, nullptr);
    require(inf != INVALID_HANDLE_VALUE, "Read installed driver version");
    struct CloseInf
    {
        HINF value;
        ~CloseInf()
        {
            SetupCloseInfFile(value);
        }
    } close{inf};
    INFCONTEXT context{};
    wchar_t version[128]{}, date[128]{};
    require(SetupFindFirstLineW(inf, L"Version", L"DriverVer", &context) &&
                SetupGetStringFieldW(&context, 1, date, 128, nullptr) &&
                SetupGetStringFieldW(&context, 2, version, 128, nullptr),
            "Read signed DriverVer");
    std::array<unsigned, 4> found{};
    wchar_t trailing = 0;
    unsigned month = 0, day = 0, year = 0;
    const bool valid_date = swscanf_s(date, L"%u/%u/%u%c", &month, &day, &year, &trailing, 1u) == 3;
    const std::array<unsigned, 3> found_date{year, month, day};
    constexpr std::array<unsigned, 3> minimum_date{2026, 8, 26};
    if (!valid_date ||
        swscanf_s(version, L"%u.%u.%u.%u%c", &found[0], &found[1], &found[2], &found[3], &trailing, 1u) !=
            4 ||
        found_date < minimum_date || (found_date == minimum_date && found < minimum))
        throw std::runtime_error(
            "기존 USB/IP 드라이버가 지원 버전보다 오래되었습니다. 기존 드라이버를 자동 변경하지 않았습니다.");
}
bool root_registered(bool require_healthy)
{
    // 드라이버 연결에 실패한 Unknown 클래스 노드도 찾아 중복 등록을 막는다.
    DeviceSet set{SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES)};
    require(set.value != INVALID_HANDLE_VALUE, "Enumerate USB/IP root devices");
    unsigned matches = 0;
    bool healthy = false;
    for (DWORD i = 0;; ++i)
    {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(set.value, i, &device))
        {
            require(GetLastError() == ERROR_NO_MORE_ITEMS, "Enumerate USB/IP device");
            break;
        }
        wchar_t ids[8192]{};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set.value, &device, SPDRP_HARDWAREID, &type,
                                               reinterpret_cast<BYTE *>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr) ||
            type != REG_MULTI_SZ)
            continue;
        bool ours = false;
        for (auto id = ids; *id; id += wcslen(id) + 1)
            ours |= transport_hardware_id(id);
        if (!ours)
            continue;
        ++matches;
        ULONG status = 0, problem = 0;
        wchar_t service[256]{};
        healthy = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0) == CR_SUCCESS &&
                  (status & DN_STARTED) && !(status & DN_HAS_PROBLEM) && !problem &&
                  SetupDiGetDeviceRegistryPropertyW(set.value, &device, SPDRP_SERVICE, &type,
                                                    reinterpret_cast<BYTE *>(service),
                                                    sizeof(service) - sizeof(wchar_t), nullptr) &&
                  type == REG_SZ && _wcsicmp(service, L"usbip2_ude") == 0;
    }
    if (matches > 1)
        throw std::runtime_error(
            "USB/IP 호스트 컨트롤러가 여러 개 있습니다. 중복 장치를 자동 변경하지 않았습니다.");
    return matches == 1 && (!require_healthy || healthy);
}
struct Stage
{
    std::filesystem::path directory;
    Stage()
    {
        PWSTR root = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &root)))
            throw std::runtime_error("ProgramData unavailable");
        std::filesystem::path base(root);
        CoTaskMemFree(root);
        GUID id{};
        if (FAILED(CoCreateGuid(&id)))
            throw std::runtime_error("Setup staging ID unavailable");
        wchar_t guid[40]{};
        StringFromGUID2(id, guid, 40);
        directory = base / (std::wstring(L"QuietMic-Transport-") + guid);
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        // 매 시도마다 새 보호 폴더를 만든다. 일반 사용자는 읽기만 가능하다.
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1, &descriptor,
                    nullptr),
                "Setup staging permissions");
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
        BOOL created = CreateDirectoryW(directory.c_str(), &security);
        DWORD error = GetLastError();
        LocalFree(descriptor);
        SetLastError(error);
        require(created, "Create protected transport staging");
    }
    ~Stage()
    {
        std::error_code error;
        for (const auto &file : payload)
            std::filesystem::remove(directory / file.name, error);
        std::filesystem::remove(directory, error);
    }
};
} // namespace
bool transport_hardware_id(std::wstring_view id)
{
    constexpr std::wstring_view expected = L"ROOT\\USBIP_WIN2\\UDE";
    return id.size() == expected.size() && _wcsnicmp(id.data(), expected.data(), expected.size()) == 0;
}
void verify_transport_package(const std::filesystem::path &directory)
{
    for (const auto &file : payload)
        if (file_hash(directory / file.name) != file.sha256)
            throw std::runtime_error("USB/IP 배포 파일의 SHA-256이 승인된 0.9.8.0 패키지와 다릅니다.");
    verify_catalog_driver(directory, L"usbip2_filter", true);
    verify_catalog_driver(directory, L"usbip2_ude", true);
}
void require_transport_ready()
{
    const auto filter = service_state(L"usbip2_filter");
    const auto ude = service_state(L"usbip2_ude");
    verify_installed(filter, L"usbip2_filter", {23, 56, 30, 686});
    verify_installed(ude, L"usbip2_ude", {23, 56, 48, 757});
    if (!filter.running || !ude.running || !root_registered(true))
        throw std::runtime_error("USB/IP 드라이버가 준비되지 않았습니다. 설치 결과의 재부팅 안내 또는 기존 "
                                 "드라이버 상태를 확인하세요.");
}
unsigned setup_transport(const std::filesystem::path &directory)
{
    verify_transport_package(directory);
    Handle mutex{CreateMutexW(nullptr, FALSE, L"Global\\QuietMic.DeviceSetup")};
    require(mutex.value != nullptr, "Transport setup lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("다른 QuietMic 장치 설정이 진행 중입니다.");
    const auto filter = service_state(L"usbip2_filter");
    const auto ude = service_state(L"usbip2_ude");
    if (filter.exists)
        verify_installed(filter, L"usbip2_filter", {23, 56, 30, 686});
    if (ude.exists)
        verify_installed(ude, L"usbip2_ude", {23, 56, 48, 757});
    const bool registered = root_registered(false);
    if (ude.exists && registered && !root_registered(true))
        throw std::runtime_error(
            "기존 USB/IP 호스트 컨트롤러가 정상 시작되지 않았습니다. 기존 장치를 자동 변경하지 않았습니다.");
    if (filter.exists && ude.exists && registered)
    {
        require_transport_ready();
        return ERROR_SUCCESS;
    }
    if (!IsUserAnAdmin())
        throw std::runtime_error("USB/IP 드라이버 설치에는 관리자 권한이 필요합니다.");
    // 실행 파일 이름만으로는 외부 호스트 안에서 실행 중인 세션을 찾을 수 없다.
    // 실제 세션의 전역 식별자 잠금을 획득한 동안에만 장치를 변경한다.
    Handle ownership{CreateMutexW(nullptr, FALSE, wire::ownership_mutex)};
    require(ownership.value != nullptr, "Open USB/IP session ownership lock");
    const auto acquired = WaitForSingleObject(ownership.value, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        throw std::runtime_error("QuietMic 가상 마이크가 사용 중입니다. 세션을 종료한 뒤 설치하세요.");
    struct ReleaseOwnership
    {
        HANDLE value;
        ~ReleaseOwnership()
        {
            ReleaseMutex(value);
        }
    } release{ownership.value};
    Stage stage;
    for (const auto &file : payload)
        require(CopyFileW((directory / file.name).c_str(), (stage.directory / file.name).c_str(), TRUE),
                "Stage signed USB/IP payload");
    verify_transport_package(stage.directory);
    bool reboot = false, created = false;
    DeviceSet set{INVALID_HANDLE_VALUE};
    SP_DEVINFO_DATA device{sizeof(device)};
    try
    {
        if (!filter.exists)
        {
            BOOL needed = FALSE;
            require(DiInstallDriverW(nullptr, (stage.directory / L"usbip2_filter.inf").c_str(), 0, &needed),
                    "Install USB/IP extension filter");
            reboot |= needed != FALSE;
        }
        if (!ude.exists || !registered)
        {
            GUID guid{};
            wchar_t class_name[MAX_CLASS_NAME_LEN]{};
            // 이전 시도의 롤백으로 서비스만 남았으면 기존 서명 패키지로 누락된 노드만
            // 다시 만든다. 더 최신인 기존 드라이버를 번들 버전으로 내리지 않는다.
            const auto inf = (ude.exists ? ude.directory : stage.directory) / L"usbip2_ude.inf";
            require(SetupDiGetINFClassW(inf.c_str(), &guid, class_name, MAX_CLASS_NAME_LEN, nullptr),
                    "Read USB/IP device class");
            set.value = SetupDiCreateDeviceInfoList(&guid, nullptr);
            require(set.value != INVALID_HANDLE_VALUE, "Create USB/IP device list");
            if (!registered)
            {
                require(SetupDiCreateDeviceInfoW(set.value, class_name, &guid, nullptr, nullptr,
                                                 DICD_GENERATE_ID, &device),
                        "Create USB/IP root device");
                require(SetupDiSetDeviceRegistryPropertyW(set.value, &device, SPDRP_HARDWAREID,
                                                          reinterpret_cast<const BYTE *>(hardware),
                                                          sizeof(hardware)),
                        "Set USB/IP hardware ID");
                require(SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.value, &device),
                        "Register USB/IP root device");
                created = true;
                SP_DEVINSTALL_PARAMS_W parameters{sizeof(parameters)};
                require(SetupDiGetDeviceInstallParamsW(set.value, &device, &parameters),
                        "Read USB/IP registration flags");
                reboot |= (parameters.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
            }
            BOOL needed = FALSE;
            require(UpdateDriverForPlugAndPlayDevicesW(nullptr, hardware, inf.c_str(),
                                                       INSTALLFLAG_NONINTERACTIVE, &needed),
                    "Install USB/IP emulated controller");
            reboot |= needed != FALSE;
        }
        if (reboot)
            return ERROR_SUCCESS_REBOOT_REQUIRED;
        // 오디오 끝점은 사용자 세션이 만든다. 설치에서는 전송 장치의 준비 상태만 기다린다.
        for (unsigned attempt = 0; attempt < 20; ++attempt)
        {
            const auto filter_now = service_state(L"usbip2_filter");
            const auto ude_now = service_state(L"usbip2_ude");
            if (filter_now.running && ude_now.running && root_registered(true))
            {
                require_transport_ready();
                return ERROR_SUCCESS;
            }
            Sleep(250);
        }
        require_transport_ready();
        return ERROR_SUCCESS;
    }
    catch (const std::exception &failure)
    {
        bool rollback_failed = false, rollback_reboot = false;
        if (created)
        {
            BOOL needed = FALSE;
            rollback_failed = !DiUninstallDevice(nullptr, set.value, &device, 0, &needed);
            rollback_reboot = needed != FALSE;
        }
        // 필터는 실제 USB 루트 허브에도 연결될 수 있다. 타 프로그램이 사용하지 않는다는
        // 증거가 없으므로 공유 필터/DriverStore 패키지를 강제 제거하지 않는다.
        throw std::runtime_error(
            std::string(failure.what()) + (rollback_failed ? " 새 USB/IP 장치의 롤백도 실패했습니다." : "") +
            (rollback_reboot ? " 새 장치 제거를 마치려면 재부팅이 필요합니다." : "") +
            " 공유 USB/IP 필터와 DriverStore 패키지는 보존했습니다. 설치 상태를 확인한 뒤 다시 시도하세요.");
    }
}
unsigned remove_transport()
{
    if (!IsUserAnAdmin())
        throw std::runtime_error("USB/IP 드라이버 제거에는 관리자 권한이 필요합니다.");

    Handle setup_mutex{CreateMutexW(nullptr, FALSE, L"Global\\QuietMic.DeviceSetup")};
    require(setup_mutex.value != nullptr, "Transport removal lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        throw std::runtime_error("다른 QuietMic 장치 설정이 진행 중입니다.");

    // 실행 중인 QuietMic 세션이 장치를 다시 연결하지 못하도록 설치와 같은 소유권 잠금을 잡는다.
    Handle ownership{CreateMutexW(nullptr, FALSE, wire::ownership_mutex)};
    require(ownership.value != nullptr, "Open USB/IP session ownership lock");
    const auto acquired = WaitForSingleObject(ownership.value, 0);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        throw std::runtime_error("QuietMic 가상 마이크가 사용 중입니다. 앱 종료 후 다시 제거하세요.");
    struct ReleaseOwnership
    {
        HANDLE value;
        ~ReleaseOwnership()
        {
            ReleaseMutex(value);
        }
    } release{ownership.value};

    const auto filter = service_state(L"usbip2_filter");
    const auto ude = service_state(L"usbip2_ude");
    DeviceSet set{SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES)};
    require(set.value != INVALID_HANDLE_VALUE, "Enumerate USB/IP root devices for removal");

    SP_DEVINFO_DATA target{sizeof(target)};
    std::wstring target_instance_id;
    unsigned matches = 0;
    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(set.value, index, &device))
        {
            require(GetLastError() == ERROR_NO_MORE_ITEMS, "Enumerate USB/IP removal device");
            break;
        }

        wchar_t ids[8192]{};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set.value, &device, SPDRP_HARDWAREID, &type,
                                               reinterpret_cast<BYTE *>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr) ||
            type != REG_MULTI_SZ)
            continue;

        bool ours = false;
        for (auto id = ids; *id; id += wcslen(id) + 1)
            ours |= transport_hardware_id(id);
        if (!ours)
            continue;

        target = device;
        wchar_t instance_id[MAX_DEVICE_ID_LEN]{};
        require(SetupDiGetDeviceInstanceIdW(set.value, &device, instance_id, MAX_DEVICE_ID_LEN, nullptr),
                "Read USB/IP controller instance ID");
        target_instance_id = instance_id;
        ++matches;
    }

    if (matches > 1)
        throw std::runtime_error("USB/IP 호스트 컨트롤러가 여러 개라 안전하게 제거할 수 없습니다.");

    bool reboot = false;
    if (matches == 1)
    {
        // 빈 컨트롤러도 USB 루트 허브 하나는 가진다. 그 허브 아래 장치가 있거나 예상하지 못한
        // 직접 자식이 있으면 다른 USB/IP 연결로 보고 아무것도 제거하지 않는다.
        DEVINST child = 0;
        CONFIGRET child_result = CM_Get_Child(&child, target.DevInst, 0);
        while (child_result == CR_SUCCESS)
        {
            wchar_t instance[MAX_DEVICE_ID_LEN]{};
            if (CM_Get_Device_IDW(child, instance, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
                throw std::runtime_error("USB/IP 자식 장치 식별자를 읽지 못했습니다.");

            constexpr std::wstring_view root_hub = L"USB\\ROOT_HUB";
            const std::wstring_view id(instance);
            const bool is_root_hub = id.size() >= root_hub.size() &&
                                     _wcsnicmp(id.data(), root_hub.data(), root_hub.size()) == 0;
            DEVINST attached = 0;
            const CONFIGRET attached_result = is_root_hub ? CM_Get_Child(&attached, child, 0) : CR_SUCCESS;
            if (!is_root_hub || attached_result == CR_SUCCESS)
                throw std::runtime_error(
                    "다른 USB/IP 장치가 연결되어 있어 공유 전송 드라이버를 제거하지 않았습니다.");
            if (attached_result != CR_NO_SUCH_DEVNODE)
                throw std::runtime_error("USB/IP 연결 상태를 확인하지 못했습니다.");

            child_result = CM_Get_Sibling(&child, child, 0);
        }
        if (child_result != CR_NO_SUCH_DEVNODE)
            throw std::runtime_error("USB/IP 컨트롤러 장치 트리를 확인하지 못했습니다.");

        reboot |= remove_device_with_pnputil(target_instance_id);
    }

    const auto remove_package = [&](const ServiceState &state, const wchar_t *name)
    {
        if (!state.exists)
            return;
        BOOL needed = FALSE;
        require(DiUninstallDriverW(nullptr, (state.directory / (std::wstring(name) + L".inf")).c_str(),
                                   0, &needed),
                "Remove USB/IP Driver Store package");
        reboot |= needed != FALSE;
    };

    // 컨트롤러 패키지를 먼저 제거한 다음 루트 허브에 연결되는 확장 필터를 제거한다.
    remove_package(ude, L"usbip2_ude");
    remove_package(filter, L"usbip2_filter");
    reboot |= service_delete_pending(L"usbip2_ude");
    reboot |= service_delete_pending(L"usbip2_filter");
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}
} // namespace qm::usbip
