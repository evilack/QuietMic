#include "setup.hpp"
#include "driver_trust.hpp"
#include <array>
#include <fstream>
#include <set>
#include <stdexcept>
#include <bcrypt.h>
#include <setupapi.h>
#include <newdev.h>
#include <sddl.h>
#include <shlobj.h>
#include <initguid.h>
#include <devpkey.h>
#include <tlhelp32.h>
namespace qm
{
// 끝의 명시적 NUL과 문자열 자체의 NUL이 합쳐져 REG_MULTI_SZ의 이중 종료를 만든다.
static constexpr wchar_t hardware[] = L"ROOT\\QuietMic\0";
static const GUID media_class = {
    0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
static constexpr const wchar_t *payload[] = {L"QuietMicDriver.inf", L"QuietMicDriver.sys",
                                             L"QuietMicDriver.cat"};
// 표시 이름 대신 하드웨어 ID 전체를 비교한다. 접두사만 같은 다른 장치를 제거하지 않기 위해
// 길이까지 같아야 하며 Windows ID 비교 관례에 따라 영문 대소문자만 무시한다.
bool quietmic_hardware_id(std::wstring_view id)
{
    constexpr std::wstring_view expected = L"ROOT\\QuietMic";
    return id.size() == expected.size() && _wcsnicmp(id.data(), expected.data(), expected.size()) == 0;
}
// 현재 사용자 창만 확인하지 않고 프로세스 스냅샷 전체에서 앱 이름을 찾는다.
// 다른 Windows 세션의 앱이 드라이버를 사용 중일 수 있기 때문이다.
void require_app_stopped()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        throw std::runtime_error("실행 중인 앱 목록을 확인하지 못했습니다.");
    struct CloseSnapshot
    {
        HANDLE h;
        // 예외가 나거나 검사 루프가 끝나면 스냅샷 핸들을 자동 반환한다.
        ~CloseSnapshot()
        {
            CloseHandle(h);
        }
    } close{snapshot};
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot, &entry))
        throw std::runtime_error("실행 중인 앱 목록을 읽지 못했습니다.");
    do
    {
        if (_wcsicmp(entry.szExeFile, L"QuietMic.exe") == 0)
            throw std::runtime_error("모든 Windows 사용자 세션에서 QuietMic을 종료한 후 다시 시도하세요.");
    } while (Process32NextW(snapshot, &entry));
}
// 설치/제거 프로세스가 만든 전역 뮤텍스의 존재로 충돌을 감지한다.
// 이 함수는 잠금을 새로 만들거나 기다리는 함수가 아니라 앱 실행 전 확인 함수이다.
void require_no_installer()
{
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\QuietMic.InstallOperation");
    const DWORD error = GetLastError();
    if (handle)
        CloseHandle(handle);
    // '없음' 이외의 오류도 안전하게 중단한다. 접근 실패를 '설치 작업 없음'으로 간주하지 않는다.
    if (handle || error != ERROR_FILE_NOT_FOUND)
        throw std::runtime_error("QuietMic 설치 또는 제거가 진행 중입니다. 완료 후 실행하세요.");
}
// BOOL을 반환하는 Win32 호출의 실패를 예외로 바꾸어 작업명과 원래 오류 번호를 전달한다.
static void require(BOOL ok, const char *operation)
{
    if (!ok)
        throw std::runtime_error(std::string(operation) + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
}
// QuietMic이 소리를 '내보내는' 대상은 다른 앱 입장에서는 입력 마이크(capture)이다.
// native 표식이 있는 캡처 끝점만 허용하고 서로 다른 끝점이 둘 이상이면 사용자가 확인하게 한다.
std::string select_output(const std::vector<Device> &list)
{
    std::string result;
    for (const auto &d : list)
    {
        if (!d.capture || !d.native)
            continue;
        if (!result.empty() && result != d.id)
            throw std::runtime_error(
                "QuietMic 마이크가 여러 개 감지되었습니다. 중복 장치 상태를 확인하세요.");
        result = d.id;
    }
    return result;
}
// 호출자는 설치 세부 정책을 알 필요 없이 이 한 관문으로 패키지 검증을 요청한다.
void verify_driver_package(const std::filesystem::path &directory)
{
    verify_native_driver(directory);
}
// SetupAPI 장치 정보 목록의 소유권을 묶는다. 아래 함수가 중간에 실패해도 목록이 누수되지 않는다.
struct DeviceSet
{
    HDEVINFO h;
    ~DeviceSet()
    {
        if (h != INVALID_HANDLE_VALUE)
            SetupDiDestroyDeviceInfoList(h);
    }
};
// Windows에 이미 등록된 동일 하드웨어 ID가 있으면 기존 장치를 갱신하고 중복 장치를 만들지 않는다.
static bool registered()
{
    DeviceSet set{SetupDiGetClassDevsW(&media_class, nullptr, nullptr, 0)};
    require(set.h != INVALID_HANDLE_VALUE, "Enumerate installed audio devices");
    SP_DEVINFO_DATA data{sizeof(data)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set.h, i, &data); i++)
    {
        wchar_t ids[8192]{};
        // 하드웨어 ID는 여러 문자열이 NUL로 이어지는 REG_MULTI_SZ이다.
        // 버퍼를 0으로 시작하고 끝의 두 문자 공간을 남겨 순회 시 종료 표식을 확보한다.
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, &type,
                                               reinterpret_cast<PBYTE>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr) ||
            type != REG_MULTI_SZ)
            continue;
        for (auto p = ids; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, hardware) == 0)
                return true;
    }
    return false;
}
// 검증된 파일을 보호된 임시 폴더에 고정한 뒤 Windows PnP 설치 API에 넘긴다.
// create_device가 false이면 기존 장치의 갱신이며, 실패 시 기존 장치를 임의로 제거하지 않는다.
static bool install(const std::filesystem::path &package, bool create_device)
{
    // Stage immutable, verified files in an administrator-writable directory.
    PWSTR root = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &root)))
        throw std::runtime_error("ProgramData unavailable");
    std::filesystem::path base = root;
    CoTaskMemFree(root);
    GUID id;
    if (FAILED(CoCreateGuid(&id)))
        throw std::runtime_error("Setup directory ID unavailable");
    wchar_t guid[40];
    StringFromGUID2(id, guid, 40);
    auto stage = base / (std::wstring(L"QuietMic-Setup-") + guid);
    // 매 시도마다 별도 폴더를 쓰고 SYSTEM/관리자만 수정하게 한다. 일반 사용자는 읽기/실행만
    // 허용하므로 복사 후 재검증한 파일을 비관리자가 설치 도중 교체할 수 없도록 한다.
    PSECURITY_DESCRIPTOR sd = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)", SDDL_REVISION_1, &sd, nullptr),
            "Setup permissions");
    SECURITY_ATTRIBUTES attrs{sizeof(attrs), sd, FALSE};
    BOOL created = CreateDirectoryW(stage.c_str(), &attrs);
    DWORD error = GetLastError();
    // 보안 설명자를 해제하는 호출이 오류 값을 바꿀 수 있어 CreateDirectory 결과를 보존한다.
    LocalFree(sd);
    SetLastError(error);
    require(created, "Create protected setup directory");
    struct Cleanup
    {
        std::filesystem::path p;
        ~Cleanup()
        {
            // 알려진 세 파일과 그 폴더만 정리한다. 소멸자에서는 새 예외를 던지지 않도록
            // error_code를 사용해 원래 설치 실패 원인을 덮어쓰지 않는다.
            std::error_code ec;
            for (auto n : payload)
                std::filesystem::remove(p / n, ec);
            std::filesystem::remove(p, ec);
        }
    } cleanup{stage};
    for (auto name : payload)
        require(CopyFileW((package / name).c_str(), (stage / name).c_str(), TRUE), "Copy driver payload");
    // 원본 검증 이후 복사 과정에서 내용이 달라졌을 가능성까지 막기 위해 설치용 복사본을 재검증한다.
    verify_driver_package(stage);
    DeviceSet set{SetupDiCreateDeviceInfoList(&media_class, nullptr)};
    require(set.h != INVALID_HANDLE_VALUE, "Create audio device list");
    SP_DEVINFO_DATA data{sizeof(data)};
    if (create_device)
    {
        // 가상 장치는 물리 버스가 자동 발견하지 않으므로 ROOT 장치 노드를 명시적으로 등록한다.
        require(SetupDiCreateDeviceInfoW(set.h, L"MEDIA", &media_class, L"QuietMic Output", nullptr,
                                         DICD_GENERATE_ID, &data),
                "Create audio device");
        require(SetupDiSetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID,
                                                  reinterpret_cast<const BYTE *>(hardware), sizeof(hardware)),
                "Set audio hardware ID");
        require(SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set.h, &data), "Register virtual audio device");
    }
    BOOL reboot = FALSE;
    // INF를 기준으로 Windows가 장치에 드라이버를 연결하게 한다. 반환되는 재부팅 요구도 보존한다.
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, hardware, (stage / payload[0]).c_str(),
                                            INSTALLFLAG_NONINTERACTIVE, &reboot))
    {
        DWORD failure = GetLastError();
        // 롤백 대상은 이번 시도에서 새로 등록한 장치뿐이다. 기존 장치 갱신이 실패했다고
        // 사용자가 이미 쓰던 장치까지 지우지 않으며, 롤백 호출 뒤 원래 실패 코드를 복원한다.
        // Remove only the instance created by this attempt; leave all pre-existing devices alone.
        SP_REMOVEDEVICE_PARAMS remove{
            {sizeof(SP_CLASSINSTALL_HEADER), DIF_REMOVE}, DI_REMOVEDEVICE_GLOBAL, 0};
        if (create_device &&
            SetupDiSetClassInstallParamsW(set.h, &data, &remove.ClassInstallHeader, sizeof(remove)))
            SetupDiCallClassInstaller(DIF_REMOVE, set.h, &data);
        SetLastError(failure);
        require(FALSE, "Install virtual audio driver");
    }
    return reboot != FALSE;
}
// 관리자 도우미의 설치 진입점: 동시 실행 차단 → 서명 확인 → 등록/갱신 → 끝점 확인 순서이다.
unsigned setup_virtual_microphone(const std::filesystem::path &package)
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\QuietMic.DeviceSetup");
    require(mutex != nullptr, "Setup lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        throw std::runtime_error("가상 마이크 설정이 이미 진행 중입니다.");
    }
    struct Lock
    {
        HANDLE h;
        ~Lock()
        {
            // 정상 반환과 예외 양쪽에서 뮤텍스 소유권과 핸들을 함께 반납한다.
            ReleaseMutex(h);
            CloseHandle(h);
        }
    } lock{mutex};
    auto list = devices();
    auto render = select_output(list);
    verify_driver_package(package);
    bool reboot = install(package, !registered());
    // PnP 등록 성공과 오디오 서비스의 끝점 공개는 동시에 끝나지 않으므로 짧게 반복 확인한다.
    // 이 대기는 관리자 도우미 안에서 이루어지며 오디오 실시간 처리 루프의 대기가 아니다.
    for (int attempt = 0; attempt < 40; attempt++)
    {
        list = devices();
        render = select_output(list);
        if (!render.empty())
        {
            auto capture = configured_output(list, render);
            name_output(capture);
            // 이름을 설정한 뒤 목록을 다시 읽어 의도한 QuietMic 끝점으로 인식되는지 확인한다.
            for (const auto &d : devices())
                if (d.id == capture && quietmic_output(d))
                    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
        }
        Sleep(500);
    }
    if (reboot)
        return ERROR_SUCCESS_REBOOT_REQUIRED;
    throw std::runtime_error(
        "장치는 등록되었지만 오디오 연결을 확인하지 못했습니다. QuietMic에서 다시 설정하세요.");
}
// 제거 진입점은 앱 중지부터 확인한다. 장치 열거 중 목록을 바꾸면 항목을 건너뛸 수 있으므로
// 먼저 정확한 대상들을 수집한 뒤 별도 반복문에서 삭제한다.
unsigned remove_virtual_microphone()
{
    require_app_stopped();
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\QuietMic.DeviceSetup");
    require(mutex != nullptr, "Setup lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        throw std::runtime_error("QuietMic 장치 설정이 이미 진행 중입니다.");
    }
    struct Lock
    {
        HANDLE h;
        ~Lock()
        {
            ReleaseMutex(h);
            CloseHandle(h);
        }
    } lock{mutex};
    DeviceSet set{SetupDiGetClassDevsW(&media_class, nullptr, nullptr, 0)};
    require(set.h != INVALID_HANDLE_VALUE, "Enumerate QuietMic devices");
    std::vector<SP_DEVINFO_DATA> targets;
    std::set<std::wstring> packages;
    for (DWORD index = 0;; ++index)
    {
        SP_DEVINFO_DATA data{sizeof(data)};
        if (!SetupDiEnumDeviceInfo(set.h, index, &data))
        {
            require(GetLastError() == ERROR_NO_MORE_ITEMS, "Enumerate device");
            break;
        }
        wchar_t ids[8192]{};
        DWORD type = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set.h, &data, SPDRP_HARDWAREID, &type,
                                               reinterpret_cast<PBYTE>(ids),
                                               sizeof(ids) - 2 * sizeof(wchar_t), nullptr) ||
            type != REG_MULTI_SZ)
            continue;
        bool ours = false;
        // REG_MULTI_SZ의 각 ID를 전체 비교한다. 이름에 QuietMic이 들어간다는 이유로 고르지 않는다.
        for (auto p = ids; *p; p += wcslen(p) + 1)
            ours |= quietmic_hardware_id(p);
        if (!ours)
            continue;
        wchar_t service[256]{}, inf[256]{};
        // 장치는 하드웨어 ID로 고르되, 삭제할 패키지 수집은 서비스 이름까지 일치할 때만 한다.
        DEVPROPTYPE property_type = 0;
        if (SetupDiGetDeviceRegistryPropertyW(set.h, &data, SPDRP_SERVICE, &type,
                                              reinterpret_cast<PBYTE>(service),
                                              sizeof(service) - sizeof(wchar_t), nullptr) &&
            type == REG_SZ && _wcsicmp(service, L"QuietMicDriver") == 0 &&
            SetupDiGetDevicePropertyW(set.h, &data, &DEVPKEY_Device_DriverInfPath, &property_type,
                                      reinterpret_cast<PBYTE>(inf), sizeof(inf) - sizeof(wchar_t), nullptr,
                                      0) &&
            property_type == DEVPROP_TYPE_STRING)
        {
            std::wstring name = inf;
            // Windows가 발행한 oem숫자.inf 형태만 허용해 임의 경로나 다른 INF를 전달하지 않는다.
            // Only Windows-published OEM file names may reach SetupUninstallOEMInf.
            if (name.starts_with(L"oem") && name.ends_with(L".inf") && name.size() > 7 &&
                name.substr(3, name.size() - 7).find_first_not_of(L"0123456789") == std::wstring::npos)
                packages.insert(name);
        }
        targets.push_back(data);
    }
    bool reboot = false;
    for (auto &data : targets)
    {
        BOOL needed = FALSE;
        require(DiUninstallDevice(nullptr, set.h, &data, 0, &needed), "Remove QuietMic device");
        reboot |= needed != FALSE;
    }
    for (const auto &inf : packages)
    {
        // 강제 삭제 플래그를 쓰지 않는다. 다른 장치가 쓰는 INF이면 그대로 남겨야 한다.
        if (!SetupUninstallOEMInfW(inf.c_str(), 0, nullptr))
        {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_INF_IN_USE_BY_DEVICES)
                require(FALSE, "Remove unused QuietMic driver package");
        }
    }
    return reboot ? ERROR_SUCCESS_REBOOT_REQUIRED : ERROR_SUCCESS;
}
} // namespace qm
