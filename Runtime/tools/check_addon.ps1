# Compiles the addon (../Source) with the package CMakeLists as a quick check, outside the
# editor. The editor's addon builder remains the real build.
param([string]$BuildDir = (Join-Path $PSScriptRoot '..\build\addon-check'))
$ErrorActionPreference = 'Continue'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$vars = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul 2>&1 && set"
foreach ($line in $vars) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
& $cmake -S (Join-Path $PSScriptRoot '..\..') -B $BuildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" `
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl | Out-Null
& $cmake --build $BuildDir
exit $LASTEXITCODE
