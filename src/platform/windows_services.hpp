// 경로, 명령행, 자동 시작, 설치 프로세스 등 Windows 기능을 응용 계층에 제공한다.
#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace qm::platform
{
// RAII 소유자: 객체 수명이 끝나면 HANDLE을 닫는다. 복사를 금지해 같은 핸들이 두 번 닫히지 않게 한다.
class UniqueHandle
{
    HANDLE value_ = nullptr;

  public:
    explicit UniqueHandle(HANDLE value = nullptr) : value_(value)
    {
    }
    ~UniqueHandle()
    {
        reset();
    }
    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;
    // 소유권을 넘기지 않고 API 호출에 사용할 핸들 값만 빌려 준다.
    HANDLE get() const
    {
        return value_;
    }
    // 기존 유효 핸들을 닫고 새 핸들을 소유한다. 인수 없이 호출하면 소유권을 비운다.
    void reset(HANDLE value = nullptr)
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }
};
// 실행 파일/설정/드라이버/이미지 디렉터리를 한 번 구해서 관련 구성요소에 전달한다.
struct ApplicationPaths
{
    std::filesystem::path executable, settings, driver_package, assets;
    static ApplicationPaths current();
};
// 설치 도우미 프로세스의 핸들을 소유한다. 핸들 해제는 프로세스 강제 종료와 다르다.
class SetupProcess
{
    UniqueHandle process_;

  public:
    // 아직 종료 결과를 회수하지 않은 프로세스 핸들이 있는지를 뜻한다. 실제 종료 여부는 poll이 확인한다.
    bool active() const
    {
        return process_.get() != nullptr;
    }
    void start(const std::filesystem::path &executable);
    // 완료하지 않았으면 nullopt, 완료했으면 Windows 종료 코드를 반환하는 비차단 조회다.
    std::optional<DWORD> poll();
};
std::vector<std::wstring> command_line();
void set_startup(bool enabled, const std::filesystem::path &executable);
void report_setup_failure(const std::string &error);
void open_url(const wchar_t *url);
} // namespace qm::platform
