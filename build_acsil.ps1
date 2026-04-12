param(
    [string] $SCRoot    = "C:\SierraChart",
    [string] $OnlyStudy = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$acsSource = "$SCRoot\ACS_Source"
$dataDir   = "$SCRoot\Data"
$tempDir   = "$env:TEMP\PowerMeterACSILBuild"

foreach ($dir in @($dataDir, $tempDir)) {
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
}

# ---- Locate vcvarsall.bat ------------------------------------------------
function Find-VcVarsAll {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -requires Microsoft.VisualCpp.Tools.HostX64.TargetX64 -property installationPath 2>$null
        if ($vsPath) {
            $c = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $c) { return $c }
        }
        $vsPath = & $vswhere -latest -property installationPath 2>$null
        if ($vsPath) {
            $c = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
            if (Test-Path $c) { return $c }
        }
    }
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    )
    foreach ($p in $candidates) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

$vcvarsall = Find-VcVarsAll
if ($null -eq $vcvarsall) {
    Write-Error "Could not find vcvarsall.bat. Install Desktop development with C++ from the Visual Studio Installer."
    exit 1
}
Write-Host "MSVC : $vcvarsall" -ForegroundColor DarkGray

# ---- Compiler / linker flags (match VisualCCompile.Bat exactly) ----------
$clFlags = '/Zc:wchar_t /GS /GL /W3 /O2 /Zc:inline /D "NDEBUG" /D "_WINDOWS" /D "_USRDLL" /D "_WINDLL" /Oy /Gd /Gy /Oi /GR- /GF /Ot /fp:precise /MT /std:c++17 /LD /EHa /WX- /nologo'
$lnFlags = 'Gdi32.lib User32.lib /DLL /DYNAMICBASE /INCREMENTAL:NO /OPT:REF /OPT:ICF /MACHINE:X64'

# ---- Study definitions ---------------------------------------------------
$studies = @(
    [PSCustomObject]@{
        Tag  = "Feed"
        Name = "PowerMeterFeed"
        Src  = "$acsSource\PowerMeterFeed.cpp"
        Out  = "$dataDir\PowerMeterFeed_64.dll"
    },
    [PSCustomObject]@{
        Tag  = "FeedJS"
        Name = "PowerMeterFeedJS"
        Src  = "$acsSource\PowerMeterFeedJS.cpp"
        Out  = "$dataDir\PowerMeterFeedJS_64.dll"
    }
)

if ($OnlyStudy -ne "") {
    $studies = @($studies | Where-Object { $_.Tag -eq $OnlyStudy })
    if ($studies.Count -eq 0) {
        Write-Error "Unknown -OnlyStudy '$OnlyStudy'. Valid values: Feed, FeedJS"
        exit 1
    }
}

# ---- Build loop ----------------------------------------------------------
$allOk = $true

foreach ($s in $studies) {
    if (-not (Test-Path $s.Src)) {
        Write-Warning "Source not found, skipping: $($s.Src)"
        Write-Warning "Copy the .cpp file to $acsSource first."
        $allOk = $false
        continue
    }

    Write-Host ""
    Write-Host "Building $($s.Name) ..." -ForegroundColor Cyan
    Write-Host "  src : $($s.Src)"
    Write-Host "  out : $($s.Out)"

    # Build a temporary .bat file to avoid PowerShell/cmd quoting edge cases
    $batPath = "$tempDir\build_$($s.Name).bat"
    $line1 = '@echo off'
    $line2 = "call `"$vcvarsall`" amd64"
    $line3 = 'if errorlevel 1 exit /b 1'
    $line4 = "pushd `"$tempDir`""
    $line5 = "cl $clFlags `"$($s.Src)`" /link $lnFlags /OUT:`"$($s.Out)`""
    $line6 = 'set CLERR=%ERRORLEVEL%'
    $line7 = 'popd'
    $line8 = 'exit /b %CLERR%'
    $batContent = "$line1`r`n$line2`r`n$line3`r`n$line4`r`n$line5`r`n$line6`r`n$line7`r`n$line8`r`n"
    [System.IO.File]::WriteAllText($batPath, $batContent, [System.Text.Encoding]::ASCII)

    $output   = cmd /c "`"$batPath`"" 2>&1
    $exitCode = $LASTEXITCODE

    $output | ForEach-Object { Write-Host "  $_" }

    if ($exitCode -ne 0) {
        Write-Host "  FAILED (exit $exitCode)" -ForegroundColor Red
        if ($output -match 'LNK1104') {
            Write-Host "  HINT: A DLL lock error occurred. Close Sierra Chart (or remove the study" -ForegroundColor Yellow
            Write-Host "        from your chart) to release the file lock, then run this script again." -ForegroundColor Yellow
        }
        $allOk = $false
    } else {
        Write-Host "  OK => $($s.Out)" -ForegroundColor Green
    }
}

# ---- Summary -------------------------------------------------------------
Write-Host ""
if ($allOk) {
    Write-Host "=== All studies built successfully ===" -ForegroundColor Green
    Write-Host ""
    Write-Host "Next steps:" -ForegroundColor Yellow
    Write-Host "  1. Ensure PowerMeter.exe is running."
    Write-Host "  2. Sierra Chart: Analysis > Add Custom Study"
    Write-Host "     Search for 'PowerMeter Feed' or 'PowerMeter Feed JS'."
    Write-Host "  3. Apply to a chart with DOM data enabled."
    Write-Host "  4. The overlay connects automatically via shared memory."
} else {
    Write-Host "=== One or more builds FAILED - see output above ===" -ForegroundColor Red
    exit 1
}
