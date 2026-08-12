<#
.SYNOPSIS
    Build script for M5 PY32BIN Tools GUI (Windows)
    
.DESCRIPTION
    Uses PyInstaller to package the Python GUI script into a standalone EXE.
    Note: PyInstaller does NOT support cross-compilation. 
    To build for Linux, you must run PyInstaller on a Linux environment.

.NOTES
    Run this script from PowerShell.
#>

$ErrorActionPreference = "Stop"

# Get the directory where this script resides
$ScriptDir = $PSScriptRoot
$SourceFile = Join-Path $ScriptDir "m5_py32bin_tools_gui.py"
$DistDir = Join-Path $ScriptDir "dist"
$BuildDir = Join-Path $ScriptDir "build"
$AppName = "m5_py32bin_tools_gui"

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "  M5 PY32BIN Tools GUI - Builder" -ForegroundColor Cyan
Write-Host "========================================================"
Write-Host "Source: $SourceFile"
Write-Host "Output: $DistDir\$AppName.exe"
Write-Host ""

# 1. Find suitable Python (avoid free-threaded versions with 't' suffix)
$PythonCmd = $null

# Try to find Python 3.12 first (most stable)
$Python312 = Get-Command "C:\Program Files\Python312\python.exe" -ErrorAction SilentlyContinue
if ($Python312) {
    $PythonCmd = "C:\Program Files\Python312\python.exe"
    Write-Host "Using Python 3.12: $PythonCmd" -ForegroundColor Green
} else {
    # Check all available python versions
    $AllPythons = Get-Command python*.exe -ErrorAction SilentlyContinue | Where-Object {
        $_.Name -notmatch "3\.14t" -and $_.Name -notmatch "_d\.exe" -and $_.Name -match "^python\.exe$"
    }
    
    if ($AllPythons) {
        $PythonCmd = $AllPythons[0].Source
        Write-Host "Using Python: $PythonCmd" -ForegroundColor Yellow
    } else {
        Write-Error "No suitable Python found. Please install Python 3.12 or a standard (non-free-threaded) Python 3.14."
        exit 1
    }
}

# Verify Python version
$PyVersion = & $PythonCmd --version
Write-Host "Python version: $PyVersion" -ForegroundColor Cyan
Write-Host ""

# 2. Check/Install PyInstaller and dependencies
Write-Host "Checking PyInstaller..." -ForegroundColor Yellow
try {
    & $PythonCmd -c "import PyInstaller" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "PyInstaller not found. Installing..." -ForegroundColor Yellow
        & $PythonCmd -m pip install pyinstaller
    } else {
        Write-Host "PyInstaller is already installed." -ForegroundColor Green
    }
} catch {
    Write-Host "Failed to check PyInstaller. Attempting install..."
    & $PythonCmd -m pip install pyinstaller
}

# Check required packages
Write-Host "Checking required packages (PyQt6, pyserial)..." -ForegroundColor Yellow
& $PythonCmd -m pip install --upgrade pyqt6 pyserial
Write-Host ""

# 3. Run Build
Write-Host "Starting PyInstaller build..." -ForegroundColor Yellow

# --noconsole: No terminal window
# --onefile: Single .exe file
# --clean: Clean cache and rebuild from scratch
# --distpath: Output directory
# --workpath: Temporary build directory
# --specpath: Where to save the .spec file
# --collect-all: Collect all submodules and data files for packages
# --hidden-import: Explicitly include modules that PyInstaller might miss

& $PythonCmd -m PyInstaller --noconsole --onefile --clean `
    --name $AppName `
    --distpath $DistDir `
    --workpath $BuildDir `
    --specpath $ScriptDir `
    --collect-all PyQt6 `
    --collect-all serial `
    --hidden-import=PyQt6.QtCore `
    --hidden-import=PyQt6.QtGui `
    --hidden-import=PyQt6.QtWidgets `
    --hidden-import=PyQt6.sip `
    --hidden-import=serial `
    --hidden-import=serial.tools `
    --hidden-import=serial.tools.list_ports `
    $SourceFile

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "Build Success!" -ForegroundColor Green
    Write-Host "Executable created at: $DistDir\$AppName.exe" -ForegroundColor Green
} else {
    Write-Host ""
    Write-Error "Build Failed."
}

# 4. Linux Cross-Compilation Note
Write-Host ""
Write-Host "----------------------------------------------------------------" -ForegroundColor Gray
Write-Host "NOTE regarding Linux x86 build:" -ForegroundColor White
Write-Host "PyInstaller does NOT support cross-compilation (building Linux apps on Windows)." -ForegroundColor Red
Write-Host "To generate a Linux executable, you must run the following command on a Linux environment:" -ForegroundColor White
Write-Host "  pip install pyinstaller pyqt6 pyserial" -ForegroundColor Gray
Write-Host "  python -m PyInstaller --noconsole --onefile --clean --name $AppName m5_py32bin_tools_gui.py" -ForegroundColor Gray
Write-Host "----------------------------------------------------------------" -ForegroundColor Gray
