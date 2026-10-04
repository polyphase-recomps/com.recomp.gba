# Configures and builds a game package's Native/ (a CMakeLists.txt calling agb_add_game)
# with clang from Visual Studio:
#   -Guest native  32-bit Windows program, game code linked in as x86 (build\<Config>)
#   -Guest wasm    64-bit Windows program, game code through wasm2c (build\wasm-<Config>)
#
#   build_game.ps1 -GameDir <pkg>\Native [-Guest native|wasm] [-Config RelWithDebInfo] [-Target x] [-KeepGoing] [-Define K=V,...]
param(
    [Parameter(Mandatory = $true)][string]$GameDir,
    [ValidateSet('native', 'wasm')][string]$Guest = 'native',
    [string]$Config = 'RelWithDebInfo',
    [string]$Target = '',
    [switch]$KeepGoing,
    [string[]]$Define = @()
)
$ErrorActionPreference = 'Continue'
$GameDir = (Resolve-Path $GameDir -ErrorAction Stop).Path
if ($Guest -eq 'wasm') {
    $BuildDir = Join-Path $GameDir "build\wasm-$Config"
    $arch = 'x64'; $vcvarsArch = 'x64'; $triple = 'x86_64-pc-windows-msvc'
} else {
    $BuildDir = Join-Path $GameDir "build\$Config"
    $arch = 'x86'; $vcvarsArch = 'x64_x86'; $triple = 'i686-pc-windows-msvc'
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
if ($env:VSCMD_ARG_TGT_ARCH -ne $arch) {
    # Call cmd.exe by full path: other toolchains (devkitPro msys2) put a `cmd` shim on PATH.
    $vars = & "$env:SystemRoot\System32\cmd.exe" /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" $vcvarsArch >nul 2>&1 && set"
    if (-not $vars) { throw "could not import the Visual Studio $arch environment" }
    foreach ($line in $vars) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}
$llvm = "$vs\VC\Tools\Llvm\x64\bin"
$cmake = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

if ($Define.Count -gt 0 -or -not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    $defs = @($Define | ForEach-Object { "-D$_" })
    & $cmake -S $GameDir -B $BuildDir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_C_COMPILER=$llvm\clang.exe" "-DCMAKE_C_COMPILER_TARGET=$triple" "-DAGB_GUEST=$Guest" `
        "-DCMAKE_BUILD_TYPE=$Config" "-DPython3_EXECUTABLE=$((Get-Command python).Source)" @defs
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
}
$buildArgs = @('--build', $BuildDir)
if ($Target) { $buildArgs += @('--target', $Target) }
if ($KeepGoing) { $buildArgs += @('--', '-k', '0') }
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
