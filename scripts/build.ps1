# 제품 빌드 → 배포 폴더 구성 → 배포물 자체 검사 순서로 실행한다.
param(
    [Parameter(Mandatory=$true)][string]$SlintSdk,
    [string]$BuildDirectory,
    [string]$CertificateThumbprint
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot -Parent
# 정식 배포는 프로젝트에 고정된 원본 Microsoft 서명 USB/IP 패키지를 사용한다.
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $sourceRoot 'build/product' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$SlintSdk = (Resolve-Path -LiteralPath $SlintSdk).Path
if (-not (Test-Path -LiteralPath (Join-Path $SlintSdk 'lib/cmake/Slint/SlintConfig.cmake'))) {
    throw 'Use the official Slint 1.17.1 win64 MSVC SDK directory.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
# 사용자의 VS 설치 위치를 추측하지 않고 공식 조회 도구가 반환한 경로를 사용한다.
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
if (-not $vs) { throw 'Install Visual Studio C++ Desktop development and Windows SDK.' }
$vs = @($vs)[0]
$cmake = Join-Path $vs.installationPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$major = [int]($vs.installationVersion.Split('.')[0])
$generator = if ($major -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
# -S는 소스, -B는 생성 파일 위치다. 구성에 성공해야 실제 컴파일을 시작한다.
# 외부 프로그램 실패는 PowerShell 예외와 별개이므로 매번 LASTEXITCODE도 확인한다.
& $cmake -S $sourceRoot -B $BuildDirectory -G $generator -A x64 "-DSlint_DIR=$SlintSdk/lib/cmake/Slint" -DQUIETMIC_BUILD_APP=ON
if ($LASTEXITCODE) { throw 'CMake configure failed.' }
& $cmake --build $BuildDirectory --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed.' }
$release = Join-Path $BuildDirectory 'Release'
$component = Join-Path $release 'components/usbip-drivers'
& (Join-Path $release 'quietmic_cli.exe') --verify-driver $component
# 앱 코드 서명과 커널 드라이버 신뢰 검사는 서로 다른 작업이다.
# 여기서는 실제 설치 없이 카탈로그 서명과 파일 멤버십을 검사한다.
if ($LASTEXITCODE) { throw 'The release requires a Windows-trusted driver package.' }
if ($CertificateThumbprint) {
    & (Join-Path $PSScriptRoot 'sign.ps1') -Directory $release -CertificateThumbprint $CertificateThumbprint
}
$packageRoot = Join-Path $sourceRoot 'build/package'
$dist = Join-Path $packageRoot 'QuietMic-windows-x64'
# 이 아래부터는 실행에 필요한 파일을 배포 폴더로 모으는 단계다.
# 이미지도 실제 파일로 복사하며 C++ 소스나 바이트 배열로 변환하지 않는다.
# 이전 빌드의 폐기된 파일이 새 패키지에 섞이지 않도록 이 스크립트가 관리하는
# 정확한 배포 폴더만 비운 뒤 현재 빌드 결과로 다시 구성한다.
if (Test-Path -LiteralPath $dist) {
    $resolvedDist = [IO.Path]::GetFullPath($dist).TrimEnd('\')
    $resolvedDistRoot = [IO.Path]::GetFullPath($packageRoot).TrimEnd('\')
    if (-not $resolvedDist.StartsWith($resolvedDistRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Distribution directory escaped the project dist root.'
    }
    Remove-Item -LiteralPath $resolvedDist -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $dist | Out-Null
foreach ($name in @('QuietMic.exe','quietmic_cli.exe','slint_cpp.dll')) {
    Copy-Item -LiteralPath (Join-Path $release $name) -Destination $dist -Force
}
$runtimeAssetNames = @('idle.png','running.png','muted.png','error.png',
    'idle.ico','running.ico','muted.ico','error.ico')
$assetDestination = Join-Path $dist 'assets/icons'
New-Item -ItemType Directory -Force -Path $assetDestination | Out-Null
foreach ($name in $runtimeAssetNames) {
    Copy-Item -LiteralPath (Join-Path $release "assets/icons/$name") -Destination $assetDestination -Force
}
$languageDestination = Join-Path $dist 'assets/i18n'
New-Item -ItemType Directory -Force -Path $languageDestination | Out-Null
foreach ($name in @('en.lang','ko.lang','ja.lang','zh-Hans.lang','zh-Hant.lang','es.lang','fr.lang','de.lang')) {
    Copy-Item -LiteralPath (Join-Path $release "assets/i18n/$name") -Destination $languageDestination -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $component 'usbip2_ude.inf'))) {
    throw 'Missing signed USB/IP transport package.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $dist 'components') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist 'components/usbip-drivers') | Out-Null
foreach ($name in @('usbip2_filter.inf','usbip2_filter.sys','usbip2_filter.cat','usbip2_ude.inf','usbip2_ude.sys','usbip2_ude.cat')) {
    Copy-Item -LiteralPath (Join-Path $component $name) -Destination (Join-Path $dist 'components/usbip-drivers') -Force
}
$usbipDestination = Join-Path $dist 'components/usbip'
New-Item -ItemType Directory -Force -Path $usbipDestination | Out-Null
foreach ($name in @('usbip.exe','libusbip.dll','resources.dll','LICENSE.txt',
    'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    Copy-Item -LiteralPath (Join-Path $release "components/usbip/$name") -Destination $usbipDestination -Force
}
$redistVersion = Get-ChildItem (Join-Path $vs.installationPath 'VC/Redist/MSVC') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$crt = Get-ChildItem (Join-Path $redistVersion.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
if (-not $crt) { throw 'MSVC x64 CRT redistributable folder not found.' }
# Copy only imported VC runtime libraries, including their transitive imports.
$toolsVersion = Get-ChildItem (Join-Path $vs.installationPath 'VC/Tools/MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$dumpbin = Join-Path $toolsVersion.FullName 'bin/Hostx64/x64/dumpbin.exe'
$pending = [Collections.Generic.Queue[string]]::new()
$runtimeNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
# 실행 파일/DLL의 의존성을 큐에 넣고 하나씩 추적한다. 발견한 런타임 DLL도 다시
# 검사하므로 간접 의존 DLL을 놓치지 않는다. HashSet은 중복 복사와 순환 탐색을 막는다.
foreach ($name in @('QuietMic.exe','quietmic_cli.exe','slint_cpp.dll')) { $pending.Enqueue((Join-Path $dist $name)) }
while ($pending.Count) {
    $imports = & $dumpbin /dependents $pending.Dequeue()
    if ($LASTEXITCODE) { throw 'Cannot inspect runtime dependencies.' }
    foreach ($line in $imports) {
        $name = $line.Trim()
        if ($name -notmatch '^(msvcp|vcruntime|concrt|vccorlib)[\w.]*\.dll$') { continue }
        if ($runtimeNames.Add($name)) {
            $runtime = Join-Path $crt.FullName $name
            if (-not (Test-Path -LiteralPath $runtime)) { throw "Required runtime not found: $name" }
            Copy-Item -LiteralPath $runtime -Destination $dist -Force
            $pending.Enqueue($runtime)
        }
    }
}
foreach ($name in @('README.md','THIRD_PARTY_NOTICES.md')) { Copy-Item (Join-Path $sourceRoot $name) $dist -Force }
# 사용자에게 필요한 README 화면 자료만 명시적으로 복사한다.
$documentationDestination = Join-Path $dist 'docs'
$screenshotDestination = Join-Path $documentationDestination 'images'
New-Item -ItemType Directory -Force -Path $screenshotDestination | Out-Null
foreach ($name in @('microphone.png','filter.png','settings.png')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot "docs/images/$name") -Destination $screenshotDestination -Force
}
Copy-Item (Join-Path $sourceRoot 'licenses') $dist -Recurse -Force
if ($CertificateThumbprint) {
    $bad = Get-ChildItem -LiteralPath $dist -File | Where-Object {$_.Extension -in '.exe','.dll'} | ForEach-Object {
        $signature=Get-AuthenticodeSignature -LiteralPath $_.FullName
        if($signature.Status -ne 'Valid') { $_.Name }
    }
    if($bad) { throw "Untrusted binaries in release package: $($bad -join ', ')" }
}
& (Join-Path $PSScriptRoot 'verify-payload.ps1') -Directory $dist | Out-Null
# 빌드 폴더의 CLI가 아니라 방금 복사한 CLI를 실행해야 누락된 런타임도 드러난다.
# The packaged helper must have all of its app-local VC runtimes before it is executed.
& (Join-Path $dist 'quietmic_cli.exe') --verify-driver (Join-Path $dist 'components/usbip-drivers')
if ($LASTEXITCODE) { throw 'Copied release driver failed verification.' }
& (Join-Path $PSScriptRoot 'verify-transport-release.ps1') -Directory (Join-Path $dist 'components/usbip-drivers')
Write-Output "Package: $dist"
