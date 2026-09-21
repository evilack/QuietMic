param([switch] $All)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path $PSScriptRoot -Parent)).TrimEnd('\')

# 기본 실행은 패키징 및 컴파일 결과만 지운다. -All은 CMake 생성물도 지우므로
# 이후 scripts/generate-vs.ps1을 실행해 루트 Visual Studio 솔루션을 다시 만들어야 한다.
$relativeTargets = [Collections.Generic.List[string]]::new()
foreach ($path in @(
    'build\downloads',
    'build\installer',
    'build\package',
    'build\product\submission',
    'build\product\Debug',
    'build\product\Release',
    'build\product\MinSizeRel',
    'build\product\RelWithDebInfo',
    'build\product\x64',
    '.vs',
    'x64'
    ))
{
    $relativeTargets.Add($path)
}
if ($All)
{
    foreach ($path in @('build\product', 'QuietMic.sln', 'QuietMic.slnx'))
    {
        $relativeTargets.Add($path)
    }
}

foreach ($relative in $relativeTargets)
{
    $target = [IO.Path]::GetFullPath((Join-Path $projectRoot $relative)).TrimEnd('\')
    if (-not $target.StartsWith($projectRoot + '\', [StringComparison]::OrdinalIgnoreCase))
    {
        throw "Cleanup target escaped the QuietMic project: $target"
    }
    if (Test-Path -LiteralPath $target)
    {
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Output "Removed: $target"
    }
}
