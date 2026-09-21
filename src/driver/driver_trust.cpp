#include "driver_trust.hpp"
#include <windows.h>
#include <wintrust.h>
#include <mscat.h>
#include <softpub.h>
#include <bcrypt.h>
#include <array>
#include <stdexcept>
namespace qm
{
// 관리자가 실행한 설치 도우미도 외부 파일을 그대로 신뢰하지 않는다.
// 서명된 카탈로그와 실제 INF/SYS의 관계를 각각 검증한 뒤에만 설치 단계로 넘어간다.
void verify_native_driver(const std::filesystem::path &directory)
{
    verify_catalog_driver(directory, L"QuietMicDriver", false);
}
void verify_catalog_driver(const std::filesystem::path &directory, std::wstring_view basename,
                           bool require_microsoft_root)
{
    // 경로 구성 요소를 입력으로 받지 않는다. 검증할 패키지 이름만 전달해야 한다.
    if (basename.empty() ||
        basename.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
            std::wstring_view::npos)
        throw std::runtime_error("Invalid driver package name");
    const std::wstring base(basename);
    auto catalog = (directory / (base + L".cat")).wstring();
    if (!std::filesystem::is_regular_file(catalog))
        throw std::runtime_error("서명된 드라이버 카탈로그가 없습니다.");
    GUID policy = DRIVER_ACTION_VERIFY;
    // 일반 문서 서명이 아니라 Windows 드라이버 검증 정책과 SHA-256 카탈로그 문맥을 사용한다.
    HCATADMIN admin = nullptr;
    if (!CryptCATAdminAcquireContext2(&admin, &policy, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
        throw std::runtime_error("Windows driver catalog verification unavailable");
    struct Context
    {
        HCATADMIN h;
        // RAII: 아래 반복문에서 예외가 나도 카탈로그 검증 자원은 반환된다.
        ~Context()
        {
            CryptCATAdminReleaseContext(h, 0);
        }
    } context{admin};
    for (const auto &name : {base + L".inf", base + L".sys"})
    {
        auto member = (directory / name).wstring();
        HANDLE file = CreateFileW(member.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Driver package is incomplete");
        struct Close
        {
            HANDLE h;
            // 이번 파일의 검증 성공/실패와 관계없이 OS 파일 핸들을 닫는다.
            ~Close()
            {
                CloseHandle(h);
            }
        } close{file};
        DWORD length = 32;
        std::array<BYTE, 32> hash{};
        // 경로 이름이 아닌 열린 파일 내용으로 해시를 만든다. 서명된 다른 파일로 바꿔치기한
        // 패키지는 이후 카탈로그 구성원 검증을 통과할 수 없다.
        if (!CryptCATAdminCalcHashFromFileHandle2(admin, file, &length, hash.data(), 0) || length != 32)
            throw std::runtime_error("Driver catalog hash calculation failed");
        std::wstring tag;
        // 카탈로그가 구성원을 찾는 키는 해시의 대문자 16진수 문자열이다.
        for (auto byte : hash)
        {
            tag += L"0123456789ABCDEF"[byte >> 4];
            tag += L"0123456789ABCDEF"[byte & 15];
        }
        WINTRUST_CATALOG_INFO info{};
        // 검증 대상 CAT, 구성원 경로, 열린 핸들, 계산한 해시를 한 요청에 연결한다.
        info.cbStruct = sizeof(info);
        info.pcwszCatalogFilePath = catalog.c_str();
        info.pcwszMemberFilePath = member.c_str();
        info.pcwszMemberTag = tag.c_str();
        info.hMemberFile = file;
        info.pbCalculatedFileHash = hash.data();
        info.cbCalculatedFileHash = length;
        info.hCatAdmin = admin;
        WINTRUST_DATA data{};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        data.dwUnionChoice = WTD_CHOICE_CATALOG;
        data.pCatalog = &info;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        // 검증 UI를 띄우지 않고 결과 코드를 받는다. 체인 전체의 폐기 여부도 정책에 포함된다.
        LONG result = WinVerifyTrust(nullptr, &policy, &data);
        // 드라이버 정책으로 검증된 바로 그 서명 체인을 검사한다. 인증서 표시 이름을
        // 문자열 비교하는 방식은 위조 가능한 이름을 신뢰하게 되므로 사용하지 않는다.
        bool microsoft = !require_microsoft_root;
        if (result == ERROR_SUCCESS && require_microsoft_root)
        {
            auto provider = WTHelperProvDataFromStateData(data.hWVTStateData);
            auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            if (signer && signer->pChainContext)
            {
                CERT_CHAIN_POLICY_PARA parameters{sizeof(parameters)};
                CERT_CHAIN_POLICY_STATUS status{sizeof(status)};
                microsoft = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT,
                                                             signer->pChainContext, &parameters, &status) &&
                            status.dwError == ERROR_SUCCESS;
            }
        }
        // WinVerifyTrust가 만든 상태는 실패했을 때도 CLOSE 호출로 해제해야 한다.
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &policy, &data);
        if (result != ERROR_SUCCESS || !microsoft)
            // 여기서는 인증서를 설치하거나 서명 검사를 끄지 않는다. 검증 실패는 설치 중단이다.
            throw std::runtime_error("Windows 드라이버 신뢰 또는 Microsoft 루트 검증에 실패했습니다. "
                                     "드라이버를 설치하지 않았습니다. (trust status " +
                                     std::to_string(static_cast<unsigned long>(result)) + ")");
    }
}
} // namespace qm
