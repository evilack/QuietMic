// 작업 표시줄 알림 영역의 아이콘/메뉴를 제공한다. 네이티브 Windows 구현은 Impl 뒤로 숨긴다.
#pragma once
#include <functional>
#include <filesystem>
#include <memory>

namespace qm
{
// 외부 언어 카탈로그에서 만든 네이티브 메뉴/툴팁 문구다. 트레이 스레드는 이 값의 사본만 사용한다.
struct TrayText
{
    std::string show, volume, mute, unmute, start, stop, denoise, startup, quit;
    std::string idle_tip, running_tip, muted_tip, error_tip;
    bool operator==(const TrayText &) const = default;
};

// 숫자 메뉴 리소스 ID를 응용 계층 명령으로 바꾼 열거형이다.
enum class TrayCommand
{
    Show,
    ToggleMute,
    ToggleProcessing,
    Quit,
    ToggleDenoise,
    ToggleStartup,
    ShowVolume
};

class Tray
{
  public:
    // 트레이 전용 스레드에서 호출되는 콜백이다. 수신자는 UI를 바꾸기 전에 UI 이벤트 루프로 넘겨야 한다.
    using Callback = std::function<void(TrayCommand)>;

    Tray(const std::filesystem::path &asset_directory, TrayText text, Callback callback);
    ~Tray();
    // 네이티브 창/아이콘/스레드를 소유하므로 객체 복제를 금지한다.
    Tray(const Tray &) = delete;
    Tray &operator=(const Tray &) = delete;

    // 표시 상태 변경을 트레이 스레드에 전달한다. present는 셸 등록 확인, request_show는 창 표시 명령 요청이다.
    void update(bool running, bool muted, bool denoise, bool startup, bool error);
    void update_text(TrayText text);
    TrayText text() const;
    bool present() const;
    bool request_show() const;
    // 이미 실행된 프로세스의 숨은 트레이 창을 찾아 표시 메시지를 보낸다.
    static bool show_existing();

  private:
    // unique_ptr가 비공개 구현을 단독 소유하여 트레이 파괴 시 스레드와 네이티브 자원을 정리한다.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace qm
