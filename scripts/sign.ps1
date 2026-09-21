# 기존 인증서로 앱/DLL에 Authenticode 서명을 붙인다. 드라이버/앱 제어 정책 승인은 별개다.
param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$CertificateThumbprint,
    [string]$TimestampServer = 'http://timestamp.digicert.com'
)
$ErrorActionPreference = 'Stop'
$Directory = (Resolve-Path -LiteralPath $Directory).Path
$thumbprint = $CertificateThumbprint.Replace(' ','')
$certificate = Get-ChildItem Cert:/CurrentUser/My -CodeSigningCert |
    Where-Object { $_.Thumbprint -eq $thumbprint } | Select-Object -First 1
if (-not $certificate -or -not $certificate.HasPrivateKey) {
    # 고유 지문으로 선택한 인증서와 개인 키가 모두 있어야 파일을 서명할 수 있다.
    throw 'The selected CurrentUser code signing certificate and private key are required.'
}
# Sign every unsigned/untrusted PE file in this build, including the Slint runtime.
# Preserve existing valid vendor signatures. Never change Windows trust or app-control policies.
foreach ($file in (Get-ChildItem -LiteralPath $Directory -File | Where-Object {$_.Extension -in '.exe','.dll'})) {
    $path = $file.FullName
    $name = $file.Name
    $existing = Get-AuthenticodeSignature -LiteralPath $path
    if ($existing.Status -eq 'Valid') { continue }
    # 이미 유효한 공급자 서명은 보존한다. 새 서명은 타임스탬프를 요청하고 결과도 검증한다.
    $result = Set-AuthenticodeSignature -LiteralPath $path -Certificate $certificate -HashAlgorithm SHA256 -TimestampServer $TimestampServer
    if ($result.Status -ne 'Valid') { throw "Signing failed for ${name}: $($result.StatusMessage)" }
    Write-Output "$name signed and signature validated. Application-control policy acceptance is separate."
}
