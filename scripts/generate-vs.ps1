# CMake 정의로부터 제품용 Visual Studio 솔루션을 만든다. 여기서는 앱을 설치하지 않는다.
param(
    [string]$SlintSdk,
    [switch]$Open
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path $PSScriptRoot -Parent
$buildRelative = 'build/product'
# 생성된 vcxproj/Slint 코드 등은 이 폴더 아래에만 두고 직접 관리하는 소스와 구분한다.
$buildRoot = Join-Path $sourceRoot $buildRelative
if (-not $SlintSdk) { $SlintSdk = Join-Path $sourceRoot '.deps/slint' }
$SlintSdk = (Resolve-Path -LiteralPath $SlintSdk).Path
$slintConfig = Join-Path $SlintSdk 'lib/cmake/Slint'
if (-not (Test-Path -LiteralPath (Join-Path $slintConfig 'SlintConfig.cmake'))) {
    throw 'Slint C++ SDK not found. Pass -SlintSdk with the Slint 1.18.0 MSVC SDK directory.'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
# C++ 도구가 설치된 VS를 조회한 뒤 그 VS에 포함된 CMake를 선택한다.
$installations = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)
if (-not $installations.Count) { throw 'Visual Studio with Desktop development with C++ is required.' }
$vs = $installations[0]
$cmake = Join-Path $vs.installationPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Install C++ CMake tools for Windows in Visual Studio Installer.' }
$generator = if ([int]($vs.installationVersion.Split('.')[0]) -ge 18) { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
& $cmake -S $sourceRoot -B $buildRoot -G $generator -A x64 "-DSlint_DIR=$slintConfig" -DQUIETMIC_BUILD_APP=ON
if ($LASTEXITCODE) { throw 'Visual Studio project generation failed. See the CMake error above.' }

# Keep CMake-generated projects under build/product, with a root product solution.
# Paths in Project and BuildDependency elements are relative to the solution.
if (Test-Path -LiteralPath (Join-Path $buildRoot 'QuietMic.slnx')) {
    # 최신 slnx는 XML이다. 솔루션만 루트로 옮기므로 내부 프로젝트/의존성 경로에
    # build/product 접두사를 붙여 새 솔루션 위치에서도 같은 파일을 가리키게 한다.
    [xml]$solution = Get-Content -LiteralPath (Join-Path $buildRoot 'QuietMic.slnx') -Raw
    foreach ($project in $solution.SelectNodes('//Project[@Path]')) {
        $project.SetAttribute('Path', $buildRelative + '/' + $project.GetAttribute('Path'))
    }
    foreach ($dependency in $solution.SelectNodes('//BuildDependency[@Project]')) {
        $dependency.SetAttribute('Project', $buildRelative + '/' + $dependency.GetAttribute('Project'))
    }
    $solutionPath = Join-Path $sourceRoot 'QuietMic.slnx'
    $writerSettings = [Xml.XmlWriterSettings]::new()
    $writerSettings.Indent = $true
    $writerSettings.Encoding = [Text.UTF8Encoding]::new($false)
    $writer = [Xml.XmlWriter]::Create($solutionPath, $writerSettings)
    try { $solution.Save($writer) } finally { $writer.Dispose() }
} else {
    # 구형 sln 형식에서는 프로젝트 경로가 포함된 줄만 바꾼다.
    # 나머지 구성/플랫폼 정보는 CMake가 생성한 내용을 그대로 유지한다.
    $generatedSolution = Join-Path $buildRoot 'QuietMic.sln'
    if (-not (Test-Path -LiteralPath $generatedSolution)) { throw 'CMake did not generate a Visual Studio solution.' }
    $lines = Get-Content -LiteralPath $generatedSolution
    $relocated = foreach ($line in $lines) {
        if ($line -match '^(Project\("[^\"]+"\) = "[^\"]+", ")([^\"]+\.vcxproj)(", .*)$') {
            $Matches[1] + ($buildRelative.Replace('/', '\') + '\') + $Matches[2] + $Matches[3]
        } else { $line }
    }
    $solutionPath = Join-Path $sourceRoot 'QuietMic.sln'
    [IO.File]::WriteAllLines($solutionPath, [string[]]$relocated, [Text.UTF8Encoding]::new($true))
}
Write-Output "Visual Studio solution: $solutionPath"
Write-Output 'Select Debug | x64 or Release | x64. Build with Ctrl+Shift+B; start QuietMic with F5.'
if ($Open) { Start-Process -FilePath (Join-Path $vs.installationPath 'Common7/IDE/devenv.exe') -ArgumentList ('"' + $solutionPath + '"') }
