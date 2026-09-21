# 배포 폴더를 읽기만 하며, 필수 파일 누락/예상 밖 파일/디렉터리 링크를 검사한다.
# 성공하면 검증한 파일 목록을 반환한다. 설치기 생성은 이 목록을 입력으로 사용한다.
param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference = 'Stop'
$Directory = (Get-Item -LiteralPath $Directory -Force).FullName.TrimEnd('\')
$required = @('QuietMic.exe','quietmic_cli.exe','slint_cpp.dll','msvcp140.dll',
    'msvcp140_atomic_wait.dll',
    'vcruntime140.dll','vcruntime140_1.dll','README.md','THIRD_PARTY_NOTICES.md',
    'components/usbip-drivers/usbip2_filter.inf','components/usbip-drivers/usbip2_filter.sys',
    'components/usbip-drivers/usbip2_filter.cat','components/usbip-drivers/usbip2_ude.inf',
    'components/usbip-drivers/usbip2_ude.sys','components/usbip-drivers/usbip2_ude.cat','licenses/NSIS-COPYING.txt',
    'licenses/RNNoise-COPYING.txt','licenses/Slint-LICENSE.md','licenses/Slint-Royalty-free-2.0.md',
    'licenses/Slint-THIRDPARTY.md',
    'components/usbip/usbip.exe','components/usbip/libusbip.dll','components/usbip/resources.dll',
    'components/usbip/LICENSE.txt','components/usbip/msvcp140.dll','components/usbip/vcruntime140.dll',
    'components/usbip/vcruntime140_1.dll','licenses/USBIP-BSD-2-Clause.txt','licenses/VirtualCables-BSD-2-Clause.txt',
    'assets/icons/idle.png','assets/icons/running.png','assets/icons/muted.png','assets/icons/error.png',
    'assets/icons/idle.ico','assets/icons/running.ico','assets/icons/muted.ico','assets/icons/error.ico',
    'assets/i18n/en.lang','assets/i18n/ko.lang','assets/i18n/ja.lang','assets/i18n/zh-Hans.lang',
    'assets/i18n/zh-Hant.lang','assets/i18n/es.lang','assets/i18n/fr.lang','assets/i18n/de.lang')
$allowed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
# required는 반드시 있어야 하는 파일, allowed는 있어도 되는 파일까지 포함한 집합이다.
# Windows 파일 이름 비교와 맞추기 위해 대소문자는 구별하지 않는다.
foreach ($name in ($required + @('docs/images/microphone.png','docs/images/filter.png','docs/images/settings.png'))) {
    [void]$allowed.Add($name)
}
$files = [Collections.Generic.List[object]]::new()
$pending = [Collections.Generic.Queue[string]]::new()
$pending.Enqueue($Directory)
# 폴더를 직접 큐로 순회한다. 링크를 먼저 검사한 후에만 하위 폴더를 큐에 넣어
# 배포 폴더 밖으로 연결되는 junction/symlink를 따라가지 않도록 한다.
while ($pending.Count) {
    $folder = Get-Item -LiteralPath $pending.Dequeue() -Force
    if (-not $folder.PSIsContainer -or ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Payload must use ordinary directories, without reparse points: $($folder.FullName)"
    }
    # Inspect directory links before descent; -Recurse alone misses junction contents on some PowerShell versions.
    foreach ($item in (Get-ChildItem -LiteralPath $folder.FullName -Force)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Payload contains a reparse point: $($item.FullName)" }
        if ($item.PSIsContainer) { $pending.Enqueue($item.FullName); continue }
        $relative = $item.FullName.Substring($Directory.Length + 1).Replace('\','/')
        # 절대 경로에서 배포 루트 부분을 제거해 플랫폼과 무관한 상대 이름으로 비교한다.
        if (-not $allowed.Contains($relative)) { throw "Unexpected file in payload: $relative" }
        if ($item.Length -eq 0) { throw "Empty payload file: $relative" }
        $files.Add($item)
    }
}
foreach ($name in $required) {
    # '예상 밖 파일 없음'만으로는 충분하지 않다. 필요한 파일이 실제로 있는지도 확인한다.
    if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) { throw "Required payload file missing: $name" }
}
$files | Sort-Object FullName
