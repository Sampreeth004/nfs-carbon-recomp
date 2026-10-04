param(
    [int]$Seconds = 60,
    [string]$Exe = "",
    [string]$GameDataRoot = "",
    [string]$ExtraArgs = "",
    [int]$ScreenshotEvery = 0
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Exe) {
    $Exe = Join-Path $ProjectRoot "out\build\win-amd64-release\nfscarbon.exe"
}
if (-not $GameDataRoot) {
    $GameDataRoot = Join-Path $ProjectRoot "game"
}
if (-not (Test-Path -LiteralPath $Exe)) { throw "Executable not found: $Exe" }

$logDir = Join-Path (Split-Path -Parent $Exe) "logs"
if (Test-Path -LiteralPath $logDir) {
    Remove-Item -LiteralPath "$logDir\*" -Force -ErrorAction SilentlyContinue
}

$outLog = Join-Path $ProjectRoot "run_stdout.log"
$errLog = Join-Path $ProjectRoot "run_stderr.log"

$script:shotCount = 0
function Save-Screenshot {
    param([string]$Tag)
    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
        $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
        $bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
        $shotDir = Join-Path $ProjectRoot "logs"
        New-Item -ItemType Directory -Force -Path $shotDir | Out-Null
        $script:shotCount++
        $shot = Join-Path $shotDir ("screen_{0}_{1:D2}.png" -f $Tag, $script:shotCount)
        $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose()
        $bmp.Dispose()
        Write-Output "screenshot: $shot"
    } catch {
        Write-Output "screenshot failed: $_"
    }
}

$p = Start-Process -FilePath $Exe -WorkingDirectory $ProjectRoot -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru

$stamp = Get-Date -Format "HHmmss"
$elapsed = 0
$nextShot = $ScreenshotEvery
while ($elapsed -lt $Seconds) {
    if ($p.HasExited) { break }
    Start-Sleep -Seconds 5
    $elapsed += 5
    if ($ScreenshotEvery -gt 0 -and $elapsed -ge $nextShot) {
        Save-Screenshot -Tag $stamp
        $nextShot += $ScreenshotEvery
    }
}

$exited = $p.HasExited
if (-not $exited) {
    if ($ScreenshotEvery -gt 0) { Save-Screenshot -Tag $stamp }
    $p | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Write-Output "STILL RUNNING after ${Seconds}s -> killed (treat as boot success for this stage)"
} else {
    Write-Output "EXITED early (code $($p.ExitCode))"
}

$latest = Get-ChildItem $logDir -Filter "*.log" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latest) {
    Write-Output "===== $($latest.Name) ====="
    Get-Content $latest.FullName
}

foreach ($crashLog in @("crash_diag.log", "crash.log")) {
    $crashPath = Join-Path $ProjectRoot "logs\$crashLog"
    if (Test-Path -LiteralPath $crashPath) {
        Write-Output "===== $crashLog ====="
        Get-Content $crashPath -Tail 80
    }
}
