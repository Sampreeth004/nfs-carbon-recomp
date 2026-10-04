param(
    [string]$Preset = "win-amd64-release",
    [switch]$Configure,
    [string]$Target = "",
    [string]$ToolsRoot = "",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ToolsRoot) {
    $ToolsRoot = Join-Path (Split-Path -Parent $ProjectRoot) "tools"
}
if (-not (Test-Path -LiteralPath $ToolsRoot)) {
    throw "Tools root not found: $ToolsRoot (pass -ToolsRoot)"
}

$LlvmBin = Join-Path $ToolsRoot "llvm\clang+llvm-23.1.2-x86_64-pc-windows-msvc\bin"
$CmakeBin = Join-Path $ToolsRoot "cmake\bin"
$NinjaBin = Join-Path $ToolsRoot "ninja"
$SdkRoot = Join-Path $ToolsRoot "rexglue-sdk\win-amd64"

foreach ($p in @($LlvmBin, $CmakeBin, $NinjaBin, $SdkRoot)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Missing tool path: $p" }
}

# MSVC environment (Windows SDK + MSVC runtime libs) for clang.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere not found; install VS 2022 Build Tools" }
$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation found" }
$devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$env:PATH = "$LlvmBin;$CmakeBin;$NinjaBin;$SdkRoot\bin;$env:PATH"

$cmake = Join-Path $CmakeBin "cmake.exe"
$buildDir = Join-Path $ProjectRoot "out\build\$Preset"

if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
    Remove-Item -LiteralPath $buildDir -Recurse -Force
}

Push-Location $ProjectRoot
try {
    if ($Configure) {
        & $cmake --preset $Preset "-DCMAKE_PREFIX_PATH=$SdkRoot"
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    if ($Target) {
        & $cmake --build --preset $Preset --target $Target
    } else {
        & $cmake --build --preset $Preset
    }
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
