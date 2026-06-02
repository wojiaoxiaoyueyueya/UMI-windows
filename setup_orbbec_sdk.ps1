# setup_orbbec_sdk.ps1 - Download OrbbecSDK v2 Windows binaries
# Run this script to set up the Orbbec SDK for the Manual_gripper project
# Requires: PowerShell 5.1+, internet connection

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$libDir = Join-Path $projectRoot "lib\orbbec"
$tempDir = Join-Path $projectRoot "temp_sdk"

Write-Host "=== Orbbec SDK v2 Windows Setup ===" -ForegroundColor Cyan
Write-Host "Project root: $projectRoot"
Write-Host "Lib directory: $libDir"

# Check if SDK DLLs already exist
$existingDll = Get-ChildItem -Path $libDir -Filter "*.dll" -Recurse -ErrorAction SilentlyContinue
if ($existingDll) {
    Write-Host "[OK] Orbbec SDK DLLs already found in lib directory" -ForegroundColor Green
    $existingDll | ForEach-Object { Write-Host "  $($_.FullName)" }
    exit 0
}

# Method 1: Try to find pyorbbecsdk's bundled DLLs (if installed via pip)
Write-Host "`n[1] Checking for pyorbbecsdk Python package..." -ForegroundColor Yellow
$pyorbbecPaths = @(
    "$env:LOCALAPPDATA\Programs\Python\*\Lib\site-packages\pyorbbecsdk",
    "$env:USERPROFILE\miniconda3\Lib\site-packages\pyorbbecsdk",
    "$env:USERPROFILE\anaconda3\Lib\site-packages\pyorbbecsdk"
)

foreach ($path in $pyorbbecPaths) {
    $resolved = Resolve-Path $path -ErrorAction SilentlyContinue
    if ($resolved) {
        Write-Host "  Found pyorbbecsdk at: $resolved" -ForegroundColor Green
        $dllSource = Join-Path $resolved.ProviderPath "libs"
        if (Test-Path $dllSource) {
            Write-Host "  Copying DLLs from pyorbbecsdk..." -ForegroundColor Green
            $win64Dir = Join-Path $libDir "lib\win64"
            New-Item -ItemType Directory -Force -Path $win64Dir | Out-Null
            Copy-Item -Path "$dllSource\*.dll" -Destination $win64Dir -Force -ErrorAction SilentlyContinue
            Write-Host "  Done! DLLs copied to $win64Dir" -ForegroundColor Green
            exit 0
        }
    }
}

# Method 2: Try pip install pyorbbecsdk and extract DLLs
Write-Host "`n[2] Trying to install pyorbbecsdk via pip..." -ForegroundColor Yellow
try {
    $pipResult = & python -m pip show pyorbbecsdk 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  Installing pyorbbecsdk..." -ForegroundColor Yellow
        & python -m pip install pyorbbecsdk 2>&1 | Out-Null
    }

    # Find where pyorbbecsdk was installed
    $pyResult = & python -c "import pyorbbecsdk; import os; print(os.path.dirname(pyorbbecsdk.__file__))" 2>&1
    if ($LASTEXITCODE -eq 0) {
        $pyorbbecDir = $pyResult.Trim()
        Write-Host "  pyorbbecsdk installed at: $pyorbbecDir" -ForegroundColor Green

        $dllSourceDir = Join-Path $pyorbbecDir "libs"
        if (Test-Path $dllSourceDir) {
            $win64Dir = Join-Path $libDir "lib\win64"
            New-Item -ItemType Directory -Force -Path $win64Dir | Out-Null

            $dlls = Get-ChildItem -Path $dllSourceDir -Filter "*.dll"
            foreach ($dll in $dlls) {
                Copy-Item $dll.FullName -Destination $win64Dir -Force
                Write-Host "  Copied: $($dll.Name)" -ForegroundColor Green
            }

            # Also copy extensions
            $extDir = Join-Path $pyorbbecDir "libs\extensions"
            if (Test-Path $extDir) {
                $destExtDir = Join-Path $libDir "extensions"
                Copy-Item -Path $extDir -Destination $destExtDir -Recurse -Force
            }

            Write-Host "`n[OK] Orbbec SDK DLLs installed successfully!" -ForegroundColor Green
            Write-Host "  DLLs location: $win64Dir" -ForegroundColor Green
            exit 0
        }
    }
} catch {
    Write-Host "  pip method failed: $_" -ForegroundColor Red
}

# Method 3: Download from GitHub releases
Write-Host "`n[3] Downloading OrbbecSDK v2 from GitHub releases..." -ForegroundColor Yellow
Write-Host "  Please download manually from: https://github.com/orbbec/OrbbecSDK_v2/releases" -ForegroundColor Cyan
Write-Host "  Extract the Windows package and copy the following to lib\orbbec\:" -ForegroundColor Cyan
Write-Host "    - lib\win64\libobsensor.dll" -ForegroundColor White
Write-Host "    - lib\win64\OrbbecSDK.lib (optional for linking)" -ForegroundColor White
Write-Host "    - extensions\*.dll (depth engine, filters)" -ForegroundColor White
Write-Host ""
Write-Host "  Or install via pip: pip install pyorbbecsdk" -ForegroundColor Cyan
Write-Host "  Then re-run this script to copy the DLLs." -ForegroundColor Cyan
