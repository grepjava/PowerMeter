param(
    [string] $Version = "1.2.0",
    [string] $SCRoot  = "C:\SierraChart",
    [switch] $SkipPackage
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir

if (-not $SkipPackage) {
    Write-Host "=== Step 1/2: Build release bundle via package.ps1 ===" -ForegroundColor Cyan
    & "$repoRoot\package.ps1" -Version $Version -SCRoot $SCRoot
    if ($LASTEXITCODE -ne 0) { Write-Error "package.ps1 failed."; exit 1 }
} else {
    Write-Host "Step 1/2: Skipped package.ps1" -ForegroundColor DarkGray
}

$bundleRoot = "$repoRoot\dist\PowerMeter_v$Version"
if (-not (Test-Path $bundleRoot)) {
    Write-Error "Bundle folder not found: $bundleRoot"
    exit 1
}

$isccCandidates = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
)
$iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($cmd) { $iscc = $cmd.Source }
}

if (-not $iscc) {
    Write-Error "Inno Setup compiler (ISCC.exe) not found. Install Inno Setup 6 first."
    exit 1
}

Write-Host "=== Step 2/2: Compile installer (Inno Setup) ===" -ForegroundColor Cyan
$issPath = "$scriptDir\PowerMeter.iss"
& $iscc "/DAppVersion=$Version" "/DBundleRoot=$bundleRoot" $issPath
if ($LASTEXITCODE -ne 0) {
    Write-Error "Installer build failed."
    exit 1
}

Write-Host ""
Write-Host "Installer created under: $repoRoot\dist" -ForegroundColor Green
