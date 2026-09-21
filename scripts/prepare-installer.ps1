# NSIS 컴파일러를 프로젝트 안에 준비한다. 운영체제에 NSIS를 설치하지 않는다.
param()
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$target=Join-Path $projectRoot '.deps/nsis'
$compiler=Join-Path $target 'nsis-3.12/makensis.exe'
if (Test-Path -LiteralPath $compiler) { Write-Output "Using local NSIS: $compiler"; return }
$downloadRoot=Join-Path $projectRoot 'build/downloads'
New-Item -ItemType Directory -Path $downloadRoot,$target -Force | Out-Null
$archive=Join-Path $downloadRoot 'nsis-3.12.zip'
$url='https://downloads.sourceforge.net/project/nsis/NSIS%203/3.12/nsis-3.12.zip'
$expected='56581F90DB321581C5381193D796FFFCF2D24B2F8FED2160A6C6A3BAA67F2C4F'
Invoke-WebRequest -Uri $url -OutFile $archive
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    # 다운로드 대신 HTML이 왔으면 지정 주소의 리디렉션만 해석해 재요청한다.
    # 이후에도 고정 해시와 일치해야 하므로 임의 HTML/ZIP을 실행하지 않는다.
    # SourceForge sometimes returns its download page before the signed download URL.
    $html=[IO.File]::ReadAllText($archive)
    $redirect=[Net.WebUtility]::HtmlDecode([regex]::Match($html,'http-equiv="refresh" content="\d+; url=([^\"]+)').Groups[1].Value)
    if (-not $redirect.StartsWith($url+'?', [StringComparison]::Ordinal)) { throw 'Unexpected NSIS download response.' }
    Invoke-WebRequest -Uri $redirect -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'NSIS archive SHA256 mismatch.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead($archive)
try {
    $root=[IO.Path]::GetFullPath($target).TrimEnd('\')+'\'
    foreach ($entry in $zip.Entries) {
        # ../ 같은 ZIP 항목으로 .deps/nsis 밖에 파일을 쓰지 못하도록 절대 경로를 검사한다.
        if (-not [IO.Path]::GetFullPath((Join-Path $root $entry.FullName)).StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'NSIS archive path is outside the dependency directory.'
        }
    }
} finally { $zip.Dispose() }
[IO.Compression.ZipFile]::ExtractToDirectory($archive,$target)
if (-not (Test-Path -LiteralPath $compiler)) { throw 'NSIS compiler missing after extraction.' }
Write-Output "Prepared local NSIS: $compiler"
