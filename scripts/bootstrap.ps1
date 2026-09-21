# 새로 복제한 저장소에 고정 버전 빌드 도구를 준비하고 Visual Studio 솔루션을 생성한다.
param([switch] $DependenciesOnly)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')
$dependencyRoot = Join-Path $projectRoot '.deps'
$downloadRoot = Join-Path $projectRoot 'build\downloads'
$slintRoot = Join-Path $dependencyRoot 'slint'
$slintConfig = Join-Path $slintRoot 'lib\cmake\Slint\SlintConfig.cmake'
$slintVersion = '1.17.1'
$slintAsset = "Slint-cpp-$slintVersion-win64-MSVC-AMD64.exe"
$slintUrl = "https://github.com/slint-ui/slint/releases/download/v$slintVersion/$slintAsset"
$slintSha256 = 'F5B537DA448C1E3D72A24A774E19518AE412B9706B8EF49BDEE64B62B878FE56'

# 설정 파일이 있더라도 다른 SDK가 남아 있을 수 있으므로 버전 파일의 정확한 값을 확인한다.
$versionFile = Join-Path $slintRoot 'lib\cmake\Slint\SlintConfigVersion.cmake'
$slintReady = (Test-Path -LiteralPath $slintConfig -PathType Leaf) -and
    (Test-Path -LiteralPath $versionFile -PathType Leaf) -and
    ((Get-Content -LiteralPath $versionFile -Raw) -match 'set\(PACKAGE_VERSION "1\.17\.1"\)')

if (-not $slintReady)
{
    [IO.Directory]::CreateDirectory($downloadRoot) | Out-Null
    [IO.Directory]::CreateDirectory($dependencyRoot) | Out-Null
    $installer = Join-Path $downloadRoot $slintAsset

    # 다운로드가 중간에 끊겼거나 다른 파일로 바뀐 경우를 막기 위해 실행 전에 항상 SHA-256을 확인한다.
    $downloadRequired = -not (Test-Path -LiteralPath $installer -PathType Leaf)
    if (-not $downloadRequired)
    {
        $downloadRequired = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $slintSha256
    }
    if ($downloadRequired)
    {
        Invoke-WebRequest -Uri $slintUrl -OutFile $installer
    }
    $actualHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    if ($actualHash -ne $slintSha256)
    {
        throw "Slint SDK SHA256 mismatch. Expected $slintSha256, received $actualHash."
    }

    # Slint의 공식 Windows 패키지는 NSIS 형식이다. /S와 /D를 사용하면 시스템 영역을
    # 변경하지 않고 저장소의 .deps/slint에 자동으로 풀 수 있다.
    if (Test-Path -LiteralPath $slintRoot)
    {
        $resolvedTarget = [IO.Path]::GetFullPath($slintRoot).TrimEnd('\')
        if (-not $resolvedTarget.StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase))
        {
            throw "Dependency target escaped the QuietMic project: $resolvedTarget"
        }
        [IO.Directory]::Delete($resolvedTarget, $true)
    }
    $process = Start-Process -FilePath $installer -ArgumentList @('/S', "/D=$slintRoot") `
        -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $slintConfig -PathType Leaf))
    {
        throw "Slint SDK installation failed with exit code $($process.ExitCode)."
    }
    Write-Output "Prepared Slint C++ SDK ${slintVersion}: $slintRoot"
}
else
{
    Write-Output "Using Slint C++ SDK ${slintVersion}: $slintRoot"
}

# 설치 파일 생성에 필요한 NSIS도 고정 버전과 해시를 사용하는 기존 준비 스크립트에 맡긴다.
& (Join-Path $PSScriptRoot 'prepare-installer.ps1')
if ($LASTEXITCODE)
{
    throw 'NSIS dependency preparation failed.'
}

if (-not $DependenciesOnly)
{
    # 생성된 솔루션은 루트 QuietMic.slnx에 배치되고 실제 CMake 프로젝트는 build/product에 둔다.
    & (Join-Path $PSScriptRoot 'generate-vs.ps1') -SlintSdk $slintRoot
    if ($LASTEXITCODE)
    {
        throw 'Visual Studio solution generation failed.'
    }
}

Write-Output 'QuietMic development environment is ready.'
