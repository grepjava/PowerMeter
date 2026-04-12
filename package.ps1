<#
.SYNOPSIS
    Assembles the full PowerMeter distribution package (ZIP).

.DESCRIPTION
    1. Builds both ACSIL DLLs via build_acsil.ps1
    2. Builds PowerMeter.exe (Release x64) via MSBuild
    3. Copies all artefacts + documentation into dist\PowerMeter_v<Version>\
    4. Creates dist\PowerMeter_v<Version>.zip ready for distribution

.PARAMETER Version
    Package version string. Default: "1.0"

.PARAMETER SCRoot
    Sierra Chart root. Default: C:\SierraChart

.PARAMETER SkipACSILBuild
    Skip the ACSIL DLL build step (use previously built DLLs).

.PARAMETER SkipExeBuild
    Skip the PowerMeter.exe MSBuild step.

.EXAMPLE
    .\package.ps1
    .\package.ps1 -Version "1.1" -SCRoot "D:\SierraChart"
    .\package.ps1 -SkipACSILBuild  # repackage without rebuilding DLLs
#>
param(
    [string] $Version        = "1.0",
    [string] $SCRoot         = "C:\SierraChart",
    [switch] $SkipACSILBuild,
    [switch] $SkipExeBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$scriptDir = $PSScriptRoot
$distBase  = "$scriptDir\dist"
$distDir   = "$distBase\PowerMeter_v$Version"
$zipPath   = "$distBase\PowerMeter_v$Version.zip"

# ── Helper: find MSBuild ────────────────────────────────────────────────────
function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild `
                              -find MSBuild\**\Bin\MSBuild.exe 2>$null | Select-Object -First 1
        if ($msbuild -and (Test-Path $msbuild)) { return $msbuild }
    }
    # Fall back to PATH
    $inPath = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($inPath) { return $inPath.Source }
    return $null
}

# ── Step 1: Build ACSIL DLLs ───────────────────────────────────────────────
if (-not $SkipACSILBuild) {
    Write-Host ""
    Write-Host "=== Step 1/4: Building ACSIL DLLs ===" -ForegroundColor Cyan
    & "$scriptDir\build_acsil.ps1" -SCRoot $SCRoot
    if ($LASTEXITCODE -ne 0) { Write-Error "ACSIL build failed."; exit 1 }
} else {
    Write-Host "Step 1/4: Skipped ACSIL build." -ForegroundColor DarkGray
}

# ── Step 2: Build PowerMeter.exe (Release x64) ─────────────────────────────
if (-not $SkipExeBuild) {
    Write-Host ""
    Write-Host "=== Step 2/4: Building PowerMeter.exe (Release|x64) ===" -ForegroundColor Cyan
    $msbuild = Find-MSBuild
    if ($null -eq $msbuild) {
        Write-Error "MSBuild not found. Install Visual Studio Desktop C++ workload."
        exit 1
    }
    $vcxproj = "$scriptDir\PowerMeter.vcxproj"
    & $msbuild $vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) { Write-Error "PowerMeter.exe build failed."; exit 1 }
} else {
    Write-Host "Step 2/4: Skipped PowerMeter.exe build." -ForegroundColor DarkGray
}

# Locate PowerMeter.exe (VS may put it in either of these default paths)
$exeCandidates = @(
    "$scriptDir\PowerMeter\x64\Release\PowerMeter.exe",   # VS default with ProjectName subdir
    "$scriptDir\x64\Release\PowerMeter.exe"               # alternative default
)
$exePath = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exePath) {
    Write-Error "Could not find PowerMeter.exe after build. Expected one of:`n$($exeCandidates -join "`n")"
    exit 1
}

# ── Step 3: Assemble dist folder ────────────────────────────────────────────
Write-Host ""
Write-Host "=== Step 3/4: Assembling dist\PowerMeter_v$Version\ ===" -ForegroundColor Cyan

if (Test-Path $distDir) { Remove-Item $distDir -Recurse -Force }
foreach ($sub in @($distDir, "$distDir\ACSIL", "$distDir\ACSIL\src", "$distDir\docs")) {
    New-Item -ItemType Directory -Path $sub -Force | Out-Null
}

# PowerMeter overlay
Copy-Item $exePath "$distDir\PowerMeter.exe"

# Pre-built ACSIL DLLs
$dllFeed   = "$SCRoot\Data\PowerMeterFeed_64.dll"
$dllFeedJS = "$SCRoot\Data\PowerMeterFeedJS_64.dll"

foreach ($dll in @($dllFeed, $dllFeedJS)) {
    if (Test-Path $dll) {
        Copy-Item $dll "$distDir\ACSIL\"
    } else {
        Write-Warning "DLL not found, skipping: $dll"
    }
}

# ACSIL source files (for users who want to recompile inside SC)
Copy-Item "$scriptDir\PowerMeterFeed.cpp"   "$distDir\ACSIL\src\PowerMeterFeed.cpp"
Copy-Item "$scriptDir\PowerMeterFeedJS.cpp" "$distDir\ACSIL\src\PowerMeterFeedJS.cpp"

# Rebuild script (so recipients can rebuild without the full dev tree)
Copy-Item "$scriptDir\build_acsil.ps1" "$distDir\ACSIL\build_acsil.ps1"

# Documentation
Copy-Item "$scriptDir\docs\README.md"          "$distDir\README.md"
Copy-Item "$scriptDir\docs\ALGORITHM_NOTES.md" "$distDir\docs\ALGORITHM_NOTES.md"

Write-Host "  PowerMeter.exe  ✓"
Write-Host "  ACSIL DLLs      ✓"
Write-Host "  Source files    ✓"
Write-Host "  Documentation   ✓"

# ── Step 4: Create ZIP ──────────────────────────────────────────────────────
Write-Host ""
Write-Host "=== Step 4/4: Creating $zipPath ===" -ForegroundColor Cyan
if (Test-Path $zipPath) { Remove-Item $zipPath }
Compress-Archive -Path $distDir -DestinationPath $zipPath

Write-Host ""
Write-Host "=== Package ready ===" -ForegroundColor Green
Write-Host "  Folder : $distDir"
Write-Host "  ZIP    : $zipPath"
Write-Host ""
Write-Host "Distribute the ZIP. Recipients unzip and follow README.md." -ForegroundColor Yellow
