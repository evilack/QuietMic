# 배포 빌드에서 Windows SDK의 커널 정책(/kp)으로 원본 패키지 구성원을 확인한다.
# 사용자 설치 시에는 SDK가 필요하지 않으며, C++ 도우미가 고정 해시/체인/카탈로그를 검사한다.
param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference = 'Stop'
$Directory = (Resolve-Path -LiteralPath $Directory).Path
$kitRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/bin'
$versions = Get-ChildItem -LiteralPath $kitRoot -Directory |
    Where-Object { $_.Name -match '^10\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending
$signtool = $versions | ForEach-Object { Join-Path $_.FullName 'x64/signtool.exe' } |
    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $signtool) { throw 'Windows SDK x64 SignTool is required to verify the release driver.' }
foreach ($base in @('usbip2_filter','usbip2_ude')) {
    foreach ($extension in @('inf','sys')) {
        & $signtool verify /kp /v /c (Join-Path $Directory "$base.cat") (Join-Path $Directory "$base.$extension")
        if ($LASTEXITCODE) { throw "Kernel signature verification failed: $base.$extension" }
    }
}
