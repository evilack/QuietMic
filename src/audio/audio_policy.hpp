// 출력 장치 표시 이름을 Windows 오디오 정책 서비스에 전달하는 COM 인터페이스 선언이다.
// 구현은 Windows가 제공하며 audio.cpp의 name_output이 이 선언으로 호출한다.
#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
namespace qm
{
// Windows 10/11 audio policy COM ABI. Not part of the public Windows SDK;
// keep it isolated and verify the resulting endpoint through MMDevice.
// 여기 나열된 가상 함수의 순서가 COM의 메서드 슬롯(ABI)이다. 일부 메서드만
// 사용하더라도 앞의 선언을 삭제하거나 순서를 바꾸면 전혀 다른 함수가 호출될 수 있다.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) AudioPolicy : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, INT64 *, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};
// 위 인터페이스를 구현하는 Windows COM 객체를 생성할 때 사용하는 클래스 ID다.
inline constexpr GUID AudioPolicyClass = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
} // namespace qm
