// 트레이 전용 스레드와 숨은 Win32 창으로 셸 메시지를 처리한다. UI 명령은 콜백으로 응용 계층에 전달한다.
#include "tray.hpp"
#include "resource_ids.h"
#include "text_encoding.hpp"

#include <windows.h>
#include <shellapi.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace qm
{
namespace
{
// WM_APP 이후 번호는 이 앱이 사용하는 사용자 메시지다. 셸 이벤트/상태 갱신/창 표시 요청을 구분한다.
constexpr UINT TrayEvent = WM_APP + 1;
constexpr UINT Update = WM_APP + 2;
constexpr UINT ShowWindowRequest = WM_APP + 3;
constexpr UINT UpdateText = WM_APP + 4;
constexpr UINT TrayIconId = 1;
constexpr wchar_t ClassName[] = L"QuietMicTrayWindow";

// 취소와 알 수 없는 메뉴 ID는 동작으로 변환하지 않는다.
std::optional<TrayCommand> tray_command(UINT id)
{
    switch (id)
    {
    case IDM_TRAY_SHOW:
        return TrayCommand::Show;
    case IDM_TRAY_VOLUME:
        return TrayCommand::ShowVolume;
    case IDM_TRAY_MUTE:
        return TrayCommand::ToggleMute;
    case IDM_TRAY_PROCESSING:
        return TrayCommand::ToggleProcessing;
    case IDM_TRAY_QUIT:
        return TrayCommand::Quit;
    case IDM_TRAY_DENOISE:
        return TrayCommand::ToggleDenoise;
    case IDM_TRAY_STARTUP:
        return TrayCommand::ToggleStartup;
    default:
        return std::nullopt;
    }
}

// 배열 순서와 일치하는 아이콘 상태다. Count는 상태가 아니라 배열 크기를 계산하기 위한 끝값이다.
enum class IconState : size_t
{
    Idle,
    Running,
    Muted,
    Error,
    Count
};
constexpr std::array<const wchar_t *, static_cast<size_t>(IconState::Count)> IconFiles{
    L"idle.ico", L"running.ico", L"muted.ico", L"error.ico"};
} // namespace

// 외부에 드러나지 않는 실제 트레이 구현. 스레드와 OS 핸들의 소유권을 이 객체가 관리한다.
struct Tray::Impl
{
    struct State
    {
        bool running = false;
        bool muted = false;
        bool denoise = true;
        bool startup = false;
        bool error = false;
        TrayText text;
        bool operator==(const State &) const = default;

        // 오류→음소거→실행→대기 순으로 아이콘을 선택한다. 여러 플래그가 참일 때 앞 상태가 우선한다.
        IconState icon() const
        {
            return error     ? IconState::Error
                   : muted   ? IconState::Muted
                   : running ? IconState::Running
                             : IconState::Idle;
        }
    };

    // 파일 경로를 준비하고 전용 메시지 스레드를 시작한다. 초기화 성공/실패가 확정될 때까지 생성자는 기다린다.
    Impl(const std::filesystem::path &asset_directory, TrayText text, Callback callback)
        : callback_(std::move(callback))
    {
        state_.text = std::move(text);
        for (size_t i = 0; i < icon_paths_.size(); ++i)
            icon_paths_[i] = asset_directory / L"icons" / IconFiles[i];
        thread_ = std::thread(
            [this]
            {
                loop();
            });
        std::unique_lock lock(mutex_);
        // 대기 중에는 mutex를 놓고, 알림 뒤 다시 잠근다. 조건식을 검사하므로 이유 없는 깨움에도 안전하다.
        ready_.wait(lock,
                    [this]
                    {
                        return ready_flag_;
                    });
        // 초기화 실패라면 잠금을 푼 뒤 스레드를 합류하고 예외를 던진다. 생성 실패 시 joinable 스레드를 남기지 않는다.
        if (!window_.load())
        {
            lock.unlock();
            thread_.join();
            throw std::runtime_error("Cannot create system tray or load its resources");
        }
    }

    // 자원 소유 스레드에 닫기 메시지를 보내고 종료될 때까지 합류한다. 다른 스레드에서 창을 직접 파괴하지 않는다.
    ~Impl()
    {
        if (const auto window = window_.load())
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (thread_.joinable())
            thread_.join();
    }

    // 상태 사본은 mutex로 보호한다. 이전 상태와 같으면 갱신 메시지를 생략한다.
    void update(bool running, bool muted, bool denoise, bool startup, bool error)
    {
        {
            std::lock_guard lock(mutex_);
            auto state = state_;
            state.running = running;
            state.muted = muted;
            state.denoise = denoise;
            state.startup = startup;
            state.error = error;
            if (state == state_)
                return;
            state_ = std::move(state);
        }
        if (const auto window = window_.load())
            // 잠금 범위를 벗어난 뒤 메시지만 게시한다. 실제 셸 아이콘 수정은 트레이 스레드에서 수행한다.
            PostMessageW(window, Update, 0, 0);
    }

    void update_text(TrayText text)
    {
        {
            std::lock_guard lock(mutex_);
            if (state_.text == text)
                return;
            state_.text = std::move(text);
        }
        if (const auto window = window_.load())
            PostMessageW(window, UpdateText, 0, 0);
    }

    TrayText text()
    {
        return snapshot().text;
    }

    // 창 핸들과 아이콘 ID로 셸에 등록된 아이콘 사각형을 조회하여 존재 여부를 확인한다.
    bool present() const
    {
        NOTIFYICONIDENTIFIER id{sizeof(id)};
        id.hWnd = window_.load();
        id.uID = TrayIconId;
        RECT rect{};
        return id.hWnd && SUCCEEDED(Shell_NotifyIconGetRect(&id, &rect));
    }

    // 창 표시 요청을 큐에 넣었는지 반환한다. 이 함수 자체가 Slint 창을 표시하는 것은 아니다.
    bool request_show() const
    {
        const auto window = window_.load();
        return window && PostMessageW(window, ShowWindowRequest, 0, 0) != FALSE;
    }

  private:
    std::thread thread_;
    // 창 핸들은 서로 다른 스레드에서 읽으므로 atomic을 사용한다. 세부 표시 상태는 아래 mutex로 보호한다.
    std::atomic<HWND> window_{nullptr};
    // File-loaded HICON handles are owned by this tray thread.
    std::array<HICON, IconFiles.size()> icons_{};
    std::array<std::filesystem::path, IconFiles.size()> icon_paths_;
    HMENU menu_ = nullptr;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool ready_flag_ = false;
    State state_;
    UINT taskbar_ = 0;
    Callback callback_;

    // mutex를 잡고 상태를 복사한 뒤 잠금을 해제한다. 이후 메뉴/아이콘 API 호출 동안 잠금을 유지하지 않는다.
    State snapshot()
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

    // 아이콘은 외부 ICO 파일에서, 메뉴 구조는 실행 파일 리소스에서 읽는다. 일부만 로드돼도 loop의 정리 코드가 회수한다.
    bool load_resources(HINSTANCE instance)
    {
        for (size_t i = 0; i < icons_.size(); ++i)
        {
            // LR_LOADFROMFILE로 얻은 아이콘은 직접 소유하므로 나중에 DestroyIcon으로 각각 해제해야 한다.
            icons_[i] = static_cast<HICON>(LoadImageW(nullptr, icon_paths_[i].c_str(), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXSMICON),
                                                      GetSystemMetrics(SM_CYSMICON), LR_LOADFROMFILE));
            if (!icons_[i])
                return false;
        }
        menu_ = LoadMenuW(instance, MAKEINTRESOURCEW(IDR_TRAY_MENU));
        return menu_ != nullptr;
    }

    // 현재 상태의 아이콘/툴팁을 셸에 반영한다. 기존 아이콘 수정이 실패하면 새로 등록을 시도한다.
    void install()
    {
        const auto state = snapshot();
        const auto icon = static_cast<size_t>(state.icon());
        NOTIFYICONDATAW data{sizeof(data)};
        data.hWnd = window_.load();
        data.uID = TrayIconId;
        // 셸에 메시지 수신, 아이콘, 툴팁 사용을 알리고 콜백 메시지 번호를 지정한다.
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = TrayEvent;
        data.hIcon = icons_[icon];
        const std::array tooltips{state.text.idle_tip, state.text.running_tip, state.text.muted_tip,
                                  state.text.error_tip};
        const auto tooltip = wide(tooltips[icon]);
        // 툴팁 버퍼보다 긴 문자열은 잘라서 넣는다. 버퍼 크기를 넘는 쓰기를 방지한다.
        wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
        if (!Shell_NotifyIconW(NIM_MODIFY, &data))
            Shell_NotifyIconW(NIM_ADD, &data);
        // 버전 4 이벤트 형식을 요청한다. 아래 procedure는 그 형식의 LOWORD 이벤트 코드를 해석한다.
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }

    // 셸 우클릭 콜백 안에서 표준 메뉴 선택이 끝날 때까지 처리한다.
    // 선택 후 UI 이벤트 루프로 볼륨 창 표시를 요청하므로 셸 넘침 창과 포커스를 다투지 않는다.
    void menu()
    {
        const auto state = snapshot();
        const auto popup = GetSubMenu(menu_, 0);
        if (!popup)
            return;
        const auto replace = [&](UINT id, const std::string &label)
        {
            const auto converted = wide(label);
            ModifyMenuW(popup, id, MF_BYCOMMAND | MF_STRING, id, converted.c_str());
        };
        replace(IDM_TRAY_SHOW, state.text.show);
        replace(IDM_TRAY_VOLUME, state.text.volume);
        replace(IDM_TRAY_MUTE, state.muted ? state.text.unmute : state.text.mute);
        replace(IDM_TRAY_PROCESSING, state.running ? state.text.stop : state.text.start);
        replace(IDM_TRAY_DENOISE, state.text.denoise);
        replace(IDM_TRAY_STARTUP, state.text.startup);
        replace(IDM_TRAY_QUIT, state.text.quit);
        CheckMenuItem(popup, IDM_TRAY_DENOISE, MF_BYCOMMAND | (state.denoise ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(popup, IDM_TRAY_STARTUP, MF_BYCOMMAND | (state.startup ? MF_CHECKED : MF_UNCHECKED));
        SetMenuDefaultItem(popup, IDM_TRAY_SHOW, FALSE);
        POINT position{};
        GetCursorPos(&position);
        const auto window = window_.load();
        SetForegroundWindow(window);
        const UINT id = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, position.x,
                                       position.y, 0, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        if (const auto command = tray_command(id))
            callback_(*command);
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto self = reinterpret_cast<Impl *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        // 창 생성 시 전달한 this를 GWLP_USERDATA에 저장한다. 이후 메시지는 같은 객체로 라우팅된다.
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self)
        {
            // Explorer 재시작의 TaskbarCreated 메시지 또는 자체 Update 요청이면 아이콘을 다시 등록/갱신한다.
            if ((message == self->taskbar_ && self->taskbar_) || message == Update)
            {
                self->install();
                return 0;
            }
            if (message == UpdateText)
            {
                self->install();
                return 0;
            }
            // 중복 실행 프로세스나 내부 요청이 보내는 표시 명령이다. 실제 UI 처리는 콜백 수신자에게 맡긴다.
            if (message == ShowWindowRequest)
            {
                self->callback_(TrayCommand::Show);
                return 0;
            }
            if (message == TrayEvent)
            {
                // 트레이 이벤트의 하위 16비트가 이벤트 종류다. 우클릭 메뉴와 마우스/키보드 선택을 구별한다.
                const auto event = LOWORD(lp);

                if (event == WM_CONTEXTMENU)
                    self->menu();
                else if (event == NIN_SELECT || event == NIN_KEYSELECT)
                    self->callback_(TrayCommand::Show);
                return 0;
            }
            // 닫기 요청은 소유 스레드에서 DestroyWindow를 호출하고, 이어지는 WM_DESTROY가 셸 등록과 루프를 정리한다.
            if (message == WM_CLOSE)
            {
                DestroyWindow(window);
                return 0;
            }
            // 셸 아이콘을 삭제하고 공유 창 핸들을 비운 다음 WM_QUIT를 게시하여 메시지 루프를 끝낸다.
            if (message == WM_DESTROY)
            {
                NOTIFYICONDATAW data{sizeof(data)};
                data.hWnd = window;
                data.uID = TrayIconId;
                Shell_NotifyIconW(NIM_DELETE, &data);
                self->window_.store(nullptr);
                PostQuitMessage(0);
                return 0;
            }
        }
        // 직접 처리하지 않은 메시지는 Windows 기본 창 처리기로 전달한다.
        return DefWindowProcW(window, message, wp, lp);
    }

    // 전용 스레드의 본문: 자원/창 생성→준비 알림→메시지 처리→자원 해제 순서다.
    void loop()
    {
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = procedure;
        window_class.hInstance = instance;
        window_class.lpszClassName = ClassName;
        RegisterClassW(&window_class);
        // Explorer가 재시작되어 트레이 등록이 사라졌을 때 받을 표준 알림 번호를 등록한다.
        taskbar_ = RegisterWindowMessageW(L"TaskbarCreated");
        // 자원 로드가 성공한 경우에만 숨은 창을 만든다. this는 WM_NCCREATE에서 객체 연결에 사용된다.
        const auto window = load_resources(instance) ? CreateWindowW(ClassName, L"QuietMic Tray", 0, 0, 0, 0,
                                                                     0, nullptr, nullptr, instance, this)
                                                     : nullptr;
        {
            std::lock_guard lock(mutex_);
            window_.store(window);
        }
        if (window)
            install();
        {
            std::lock_guard lock(mutex_);
            // 성공 여부와 무관하게 준비 완료를 알린다. 실패해도 생성자가 영원히 기다리지 않게 한다.
            ready_flag_ = true;
        }
        ready_.notify_one();
        if (window)
        {
            MSG message{};
            // 메시지가 있으면 변환/전달하고, WM_QUIT(0) 또는 오류(음수)면 루프를 빠져나간다.
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        // 파일 로드 아이콘과 메뉴는 트레이 스레드가 소유하므로 같은 스레드에서 마지막에 해제한다.
        if (menu_)
            DestroyMenu(menu_);
        for (const auto icon : icons_)
            if (icon)
                DestroyIcon(icon);
    }
};

// 외부 래퍼는 Impl을 단독 소유한다. 소멸 시 Impl이 스레드 합류와 OS 자원 해제를 수행한다.
Tray::Tray(const std::filesystem::path &asset_directory, TrayText text, Callback callback)
    : impl_(std::make_unique<Impl>(asset_directory, std::move(text), std::move(callback)))
{
}
Tray::~Tray() = default;

// 응용 계층의 여러 불리언 값을 내부 상태 사본으로 묶어 전달한다.
void Tray::update(bool running, bool muted, bool denoise, bool startup, bool error)
{
    impl_->update(running, muted, denoise, startup, error);
}

void Tray::update_text(TrayText text)
{
    impl_->update_text(std::move(text));
}

TrayText Tray::text() const
{
    return impl_->text();
}

// 셸에 아이콘이 실제 등록됐는지 비공개 구현에 질의한다.
bool Tray::present() const
{
    return impl_->present();
}

// 트레이 메시지 경로를 통해 표시 명령을 요청한다.
bool Tray::request_show() const
{
    return impl_->request_show();
}

// 클래스 이름으로 다른 실행 인스턴스의 트레이 창을 찾고 비동기 표시 메시지를 보낸다.
bool Tray::show_existing()
{
    if (const auto window = FindWindowW(ClassName, nullptr))
    {
        PostMessageW(window, ShowWindowRequest, 0, 0);
        return true;
    }
    return false;
}
} // namespace qm
