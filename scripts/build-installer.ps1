# 검증한 정식 배포 파일로 설치기를 컴파일한다. 이 스크립트 자체는 장치를 설치하지 않는다.
param([string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $PackageDirectory) {
    $PackageDirectory = Join-Path $projectRoot 'build/package/QuietMic-windows-x64'
}
$PackageDirectory = (Resolve-Path -LiteralPath $PackageDirectory).Path.TrimEnd('\')
$files = @(& (Join-Path $PSScriptRoot 'verify-payload.ps1') -Directory $PackageDirectory)
# 설치 목록은 임의의 폴더 검색 결과가 아니라 화이트리스트 검증 결과로만 만든다.
$compiler = Join-Path $projectRoot '.deps/nsis/nsis-3.12/makensis.exe'
if (-not (Test-Path -LiteralPath $compiler)) { & (Join-Path $PSScriptRoot 'prepare-installer.ps1') }
& (Join-Path $PackageDirectory 'quietmic_cli.exe') --verify-driver (Join-Path $PackageDirectory 'components/usbip-drivers')
if ($LASTEXITCODE) { throw 'Release installer requires a Windows-trusted driver package.' }
& (Join-Path $PSScriptRoot 'verify-transport-release.ps1') -Directory (Join-Path $PackageDirectory 'components/usbip-drivers')
$generated = Join-Path $projectRoot 'build/installer'
New-Item -ItemType Directory -Path $generated -Force | Out-Null
$installLines = [Collections.Generic.List[string]]::new()
$removeLines = [Collections.Generic.List[string]]::new()
$directories = [Collections.Generic.HashSet[string]]::new()
function Escape-Nsis([string]$value) { $value.Replace('$','$$').Replace('"','$\"') }
# NSIS에서 특별한 의미를 갖는 $와 따옴표를 경로 데이터로 취급하도록 이스케이프한다.
foreach ($file in $files) {
    # 같은 파일 목록으로 설치 File/제거 Delete 명령을 함께 생성한다.
    # 제거 시 사용자 폴더 전체가 아니라 설치했던 파일만 지정해서 삭제하기 위해서다.
    $relative = $file.FullName.Substring($PackageDirectory.Length+1)
    $directory = Split-Path $relative -Parent
    $installLines.Add('SetOutPath "$TargetRoot\' + (Escape-Nsis $directory) + '"')
    $installLines.Add('File "' + (Escape-Nsis $file.FullName) + '"')
    $removeLines.Add('ClearErrors')
    $removeLines.Add('Delete /REBOOTOK "$INSTDIR\' + (Escape-Nsis $relative) + '"')
    $removeLines.Add('IfErrors uninstall_failed')
    while ($directory) { [void]$directories.Add($directory); $directory=Split-Path $directory -Parent }
}
foreach ($directory in ($directories | Sort-Object Length -Descending)) { $removeLines.Add('RMDir "$INSTDIR\' + (Escape-Nsis $directory) + '"') }
# 하위 폴더부터 제거해야 상위 폴더가 비어 RMDir을 적용할 수 있다.
$installList = Join-Path $generated 'payload-install.nsh'
$removeList = Join-Path $generated 'payload-remove.nsh'
[IO.File]::WriteAllLines($installList,$installLines,[Text.UTF8Encoding]::new($true))
[IO.File]::WriteAllLines($removeList,$removeLines,[Text.UTF8Encoding]::new($true))
$output = Join-Path $projectRoot 'dist/QuietMic-Setup.exe'
# 새로 클론한 저장소에는 dist 폴더가 없으므로, NSIS가 출력 파일을 열기 전에
# 빌드 스크립트가 출력 위치를 직접 준비한다. 이전 빌드 결과가 남아 있는 개발 PC와
# 빈 GitHub Actions 작업 공간에서 같은 동작을 보장하기 위한 단계다.
$outputDirectory = Split-Path $output -Parent
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$arguments = @('/V2',"/DOUTPUT_FILE=$output","/DPAYLOAD_INSTALL=$installList","/DPAYLOAD_REMOVE=$removeList")
& $compiler @arguments (Join-Path $projectRoot 'installer/QuietMic.nsi')
# 생성한 파일 목록은 include로 전달하고 설치 순서는 QuietMic.nsi에서 정의한다.
if ($LASTEXITCODE) { throw 'Installer compilation failed.' }
Write-Output "Installer: $output"
