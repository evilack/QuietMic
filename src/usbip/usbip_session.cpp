#include "usbip_session.hpp"
#include "usb_audio_device.hpp"
#include "audio.hpp"
#include <setupapi.h>
#include <devpkey.h>
#include <objbase.h>
#include <sstream>
#include <stdexcept>

namespace qm::usbip
{
namespace
{
using namespace std::chrono_literals;
struct Handle
{
    HANDLE value = nullptr;
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
// 동일 USB 식별자는 사용자/세션에 관계없이 하나만 소유한다. 기본 보안 정책은 바꾸지 않는다.
struct SessionOwnership
{
    Handle mutex;
    bool acquired = false;
    void acquire()
    {
        mutex.value = CreateMutexW(nullptr, FALSE, wire::ownership_mutex);
        if (!mutex.value)
            throw std::runtime_error("QuietMic 가상 마이크 소유권을 확보할 수 없습니다: " +
                                     std::to_string(GetLastError()));
        const auto result = WaitForSingleObject(mutex.value, 0);
        acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        if (!acquired)
            throw std::runtime_error("다른 QuietMic 프로세스가 가상 마이크를 사용 중입니다.");
    }
    ~SessionOwnership()
    {
        if (acquired)
            ReleaseMutex(mutex.value);
    }
};
// 셸/UAC 재실행을 사용하지 않는다. 지정한 실행 파일 하나의 출력과 수명만 관리한다.
std::string execute(const std::filesystem::path &exe, const std::wstring &arguments,
                    std::stop_token stop = {})
{
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    Handle read, write, input, process, thread;
    if (!CreatePipe(&read.value, &write.value, &attributes, 0) ||
        !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("USB/IP 출력 파이프 생성 실패");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                              OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("USB/IP 표준 입력 생성 실패");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = write.value;
    startup.StartupInfo.hStdInput = input.value;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size))
        throw std::runtime_error("USB/IP 프로세스 속성 생성 실패");
    struct Attributes
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Attributes()
        {
            DeleteProcThreadAttributeList(value);
        }
    } cleanup{startup.lpAttributeList};
    HANDLE handles[] = {write.value, input.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof(handles), nullptr, nullptr))
        throw std::runtime_error("USB/IP 상속 핸들 제한 실패");
    PROCESS_INFORMATION info{};
    auto command = L"\"" + exe.wstring() + L"\" " + arguments;
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, exe.parent_path().c_str(),
                        &startup.StartupInfo, &info))
        throw std::runtime_error("USB/IP 구성 요소 실행 실패: " + std::to_string(GetLastError()));
    process.value = info.hProcess;
    thread.value = info.hThread;
    CloseHandle(write.value);
    write.value = nullptr;
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;)
    {
        DWORD available = 0;
        while (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char data[4096];
            DWORD bytes = 0;
            if (!ReadFile(read.value, data, std::min<DWORD>(available, sizeof(data)), &bytes, nullptr))
                break;
            output.append(data, bytes);
            if (output.size() > 1024 * 1024)
            {
                TerminateProcess(process.value, 1);
                WaitForSingleObject(process.value, 5000);
                throw std::runtime_error("USB/IP 응답 길이 초과");
            }
        }
        if (WaitForSingleObject(process.value, 20) == WAIT_OBJECT_0)
        {
            // 마지막 프로세스 종료 직전의 파이프 출력도 회수한다.
            while (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available)
            {
                char data[4096];
                DWORD bytes = 0;
                if (!ReadFile(read.value, data, std::min<DWORD>(available, sizeof(data)), &bytes, nullptr))
                    break;
                output.append(data, bytes);
            }
            break;
        }
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(process.value, ERROR_CANCELLED);
            WaitForSingleObject(process.value, 5000);
            throw std::runtime_error("USB/IP 연결 작업 취소 또는 시간 초과");
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process.value, &code);
    if (code)
        throw std::runtime_error("USB/IP 연결 실패 (" + std::to_string(code) + "): " + output);
    return output;
}
// 포트 목록의 같은 블록 안에 정확한 URI가 있는 경우만 반환한다.
unsigned owned_port(const std::string &listing, const std::string &uri)
{
    std::istringstream lines(listing);
    std::string line;
    unsigned port = 0;
    while (std::getline(lines, line))
    {
        if (line.starts_with("Port "))
        {
            std::istringstream value(line.substr(5));
            value >> port;
        }
        if (port && line.find("-> " + uri) != std::string::npos)
        {
            const auto begin = line.find("-> ") + 3;
            auto actual = line.substr(begin);
            while (!actual.empty() && (actual.back() == '\r' || actual.back() == ' '))
                actual.pop_back();
            if (actual == uri)
                return port;
        }
    }
    return 0;
}
} // namespace

std::string device_container()
{
    HDEVINFO list = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (list == INVALID_HANDLE_VALUE)
        return {};
    struct Close
    {
        HDEVINFO h;
        ~Close()
        {
            SetupDiDestroyDeviceInfoList(h);
        }
    } close{list};
    SP_DEVINFO_DATA device{sizeof(device)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(list, i, &device); ++i)
    {
        wchar_t instance[512];
        if (!SetupDiGetDeviceInstanceIdW(list, &device, instance, 512, nullptr) ||
            _wcsicmp(instance, wire::instance_id) != 0)
            continue;
        GUID container{};
        DEVPROPTYPE type = 0;
        if (!SetupDiGetDevicePropertyW(list, &device, &DEVPKEY_Device_ContainerId, &type,
                                       reinterpret_cast<BYTE *>(&container), sizeof(container), nullptr, 0) ||
            type != DEVPROP_TYPE_GUID)
            return {};
        wchar_t text[40];
        StringFromGUID2(container, text, 40);
        return utf8(text);
    }
    return {};
}
UsbMicSession::UsbMicSession(std::filesystem::path runtime) : runtime_(std::move(runtime))
{
    server_.set_import_lost_handler(
        [this]
        {
            std::lock_guard lock(mutex_);
            import_lost_ = true;
            changed_.notify_all();
        });
}
UsbMicSession::~UsbMicSession()
{
    stop();
}
SessionSnapshot UsbMicSession::snapshot() const
{
    std::lock_guard lock(mutex_);
    return snapshot_;
}
void UsbMicSession::publish(SessionSnapshot value, std::stop_token stop)
{
    std::function<void()> handler;
    {
        std::lock_guard lock(mutex_);
        // 열거/이름 확인 중 발생한 연결 상실이나 취소 뒤 Ready가 다시 게시되지 않게 한다.
        // 이벤트 기록과 상태 확정을 같은 잠금 안에서 처리하고 알림은 잠금 밖에서 실행한다.
        if (value.state == SessionState::Ready)
        {
            if (stop.stop_requested())
                throw std::runtime_error("가상 마이크 연결을 취소했습니다.");
            if (import_lost_)
                throw std::runtime_error("가상 마이크 USB/IP 연결이 끊어졌습니다. 다시 시작하세요.");
        }
        snapshot_ = std::move(value);
        handler = handler_;
    }
    if (handler)
        handler();
}
void UsbMicSession::set_changed_handler(std::function<void()> handler)
{
    std::lock_guard lock(mutex_);
    handler_ = std::move(handler);
}
std::shared_ptr<AudioFrameSink> UsbMicSession::sink() const
{
    return buffer_;
}
void UsbMicSession::start()
{
    stop();
    {
        std::lock_guard lock(mutex_);
        import_lost_ = false;
    }
    publish({SessionState::Preparing, {}, {}});
    try
    {
        worker_ = std::jthread(
            [this](std::stop_token stop)
            {
                run(stop);
            });
    }
    catch (...)
    {
        publish({});
        throw;
    }
}
void UsbMicSession::stop()
{
    if (worker_.joinable())
    {
        {
            std::lock_guard lock(mutex_);
            worker_.request_stop();
        }
        changed_.notify_all();
        worker_.join();
    }
}
void UsbMicSession::run(std::stop_token stop)
{
    std::string uri, error;
    const auto cli = runtime_ / L"usbip.exe";
    bool server_started = false;
    SessionOwnership ownership;
    try
    {
        if (stop.stop_requested())
            throw std::runtime_error("가상 마이크 연결을 취소했습니다.");
        ownership.acquire();
        for (const auto *name : {L"usbip.exe", L"libusbip.dll", L"resources.dll"})
            if (!std::filesystem::is_regular_file(runtime_ / name))
                throw std::runtime_error(
                    "QuietMic USB/IP 구성 요소가 없습니다. 앱 빌드 또는 설치를 확인하세요.");
        server_.start();
        server_started = true;
        const auto port = server_.port();
        uri = "usbip://127.0.0.1:" + std::to_string(port) + "/1-1";
        execute(cli, L"-t " + std::to_wstring(port) + L" attach -r 127.0.0.1 -b 1-1 --once", stop);
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        std::string endpoint;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
        {
            {
                std::lock_guard lock(mutex_);
                if (import_lost_)
                    throw std::runtime_error("가상 마이크 USB/IP 연결이 끊어졌습니다. 다시 시작하세요.");
            }
            const auto container = device_container();
            if (!container.empty())
                for (const auto &device : devices())
                    if (device.capture && device.native && device.container == container)
                        endpoint = device.id;
            if (!endpoint.empty())
                break;
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, 50ms,
                              [&]
                              {
                                  return stop.stop_requested() || import_lost_;
                              });
        }
        if (stop.stop_requested())
            throw std::runtime_error("가상 마이크 연결을 취소했습니다.");
        if (endpoint.empty())
            throw std::runtime_error("QuietMic 가상 마이크가 활성화되지 않았습니다.");
        name_output(endpoint);
        publish({SessionState::Ready, endpoint, {}}, stop);
        // 가져오기 연결 상실과 종료 요청이 깨운다. 준비 후 주기 조회는 하지 않는다.
        std::unique_lock lock(mutex_);
        changed_.wait(lock,
                      [&]
                      {
                          return stop.stop_requested() || import_lost_;
                      });
        if (import_lost_ && !stop.stop_requested())
            throw std::runtime_error("가상 마이크 USB/IP 연결이 끊어졌습니다. 다시 시작하세요.");
    }
    catch (const std::exception &e)
    {
        if (!stop.stop_requested())
            error = e.what();
    }
    // 전송 오류는 정리 전에 알려 캡처를 즉시 중지한다.
    if (!error.empty())
        publish({SessionState::Failed, {}, error});
    if (server_started)
    {
        try
        {
            const auto port = owned_port(execute(cli, L"port"), uri);
            if (port)
                execute(cli, L"detach -p " + std::to_wstring(port));
        }
        catch (const std::exception &e)
        {
            if (!error.empty())
                error += "\n";
            error += "가상 마이크 연결 정리 실패: ";
            error += e.what();
        }
        server_.stop();
    }
    if (stop.stop_requested() && error.empty())
        publish({});
    else
        publish({SessionState::Failed, {}, error});
}
} // namespace qm::usbip
