# Build and package the standalone OpenNova Importer for Windows.
#
# Produces:
#   dist\onimport.exe
#
# Run on a windows-latest GitHub Actions runner or any Windows machine with
# Python 3.11 and CMake available.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\package_importer_windows.ps1

$ErrorActionPreference = "Stop"

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

# Locate Python 3.11 (bpy requires exactly 3.11)
$PY = (& py -3.11 -c "import sys; print(sys.executable)" 2>$null)
if (-not $PY) {
    Write-Error "Python 3.11 not found. Run: py install 3.11"
    exit 1
}
Write-Host "Using Python: $PY"

# ---------------------------------------------------------------------------
# Bootstrap tooling into the 3.11 interpreter
# ---------------------------------------------------------------------------
Write-Host "=== Installing Poetry ==="
& $PY -m pip install --upgrade pip poetry --quiet

# ---------------------------------------------------------------------------
# Install project dependencies via poetry (invoked through the same Python)
# ---------------------------------------------------------------------------
Write-Host "=== Installing project dependencies ==="
& $PY -m poetry env use $PY
& $PY -m poetry install --no-interaction

# ---------------------------------------------------------------------------
# Build opennova.dll (needed by the FFI layer at runtime)
# Must be built before PyInstaller and explicitly bundled via --add-binary.
# ---------------------------------------------------------------------------
Write-Host "=== Building opennova.dll ==="
cmake -S . -B build-pkg -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIB=ON
cmake --build build-pkg --target opennova_shared --config Release
New-Item -ItemType Directory -Force -Path blender\lib\windows-x64 | Out-Null
Copy-Item build-pkg\Release\opennova.dll blender\lib\windows-x64\opennova.dll

# ---------------------------------------------------------------------------
# Install PyInstaller inside the poetry venv so it has access to all deps
# ---------------------------------------------------------------------------
Write-Host "=== Installing PyInstaller into venv ==="
& $PY -m poetry run pip install pyinstaller --quiet

# ---------------------------------------------------------------------------
# Build with PyInstaller (single-file exe)
# ---------------------------------------------------------------------------
Write-Host "=== Building onimport.exe ==="
$distPath = "$ROOT\dist"
& $PY -m poetry run python -m PyInstaller apps\importer\__main__.py `
    --name onimport `
    --onefile `
    --collect-all bpy `
    --collect-all numpy `
    --add-binary "blender\lib\windows-x64\opennova.dll;blender\lib\windows-x64" `
    --distpath $distPath `
    --noconfirm `
    --clean

Write-Host "=== Done ==="
Write-Host "  -> dist\onimport.exe"
Get-Item "$distPath\onimport.exe" | Select-Object Name, Length
