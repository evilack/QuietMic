#include "device_watcher.hpp"
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl.h>
#include <stdexcept>

namespace qm
{
namespace
{
// 구독 중에는 콜백 객체를 보관하고, 구독 해제 후에만 마지막 참조를 해제한다.
class EndpointEvents final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IMMNotificationClient>
{
    const std::function<void()> changed_;
    HRESULT notify() noexcept
    {
        try
        {
            // 이 콜백 안에서 장치 열거, 구독 해제 또는 UI 접근을 하지 않는다.
            changed_();
            return S_OK;
        }
        catch (...)
        {
            // C++ 예외를 Windows COM 호출 경계 밖으로 전달하지 않는다.
            return E_FAIL;
        }
    }

  public:
    explicit EndpointEvents(std::function<void()> changed) : changed_(std::move(changed))
    {
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
    {
        return notify();
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        return notify();
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        return notify();
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override
    {
        return notify();
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override
    {
        return notify();
    }
};
struct ComScope
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComScope()
    {
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error("Cannot initialize audio device notifications");
    }
    ~ComScope()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};
} // namespace
struct DeviceWatcher::Impl
{
    // 멤버 역순 파괴: 알림 객체와 열거기를 해제한 뒤 COM을 정리한다.
    ComScope com;
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    Microsoft::WRL::ComPtr<EndpointEvents> events;
    explicit Impl(std::function<void()> changed)
    {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator))))
            throw std::runtime_error("Cannot create audio device watcher");
        events = Microsoft::WRL::Make<EndpointEvents>(std::move(changed));
        if (!events || FAILED(enumerator->RegisterEndpointNotificationCallback(events.Get())))
            throw std::runtime_error("Cannot subscribe to audio device changes");
    }
    ~Impl()
    {
        // Windows 콜백이 아닌 소유 스레드에서 해제하여 교착을 피한다.
        enumerator->UnregisterEndpointNotificationCallback(events.Get());
    }
};
DeviceWatcher::DeviceWatcher(std::function<void()> changed)
    : impl_(std::make_unique<Impl>(std::move(changed)))
{
}
DeviceWatcher::~DeviceWatcher() = default;
} // namespace qm
