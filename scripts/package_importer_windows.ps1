# Build and package the standalone OpenNova Importer for Windows.
#
# Produces:
#   dist\onimport-v<version>.exe   (version read from pyproject.toml)
#
# Run on a windows-latest GitHub Actions runner or any Windows machine with
# Python 3.11, uv, and CMake available.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts\package_importer_windows.ps1

$ErrorActionPreference = "Stop"

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT
if (-not $env:UV_CACHE_DIR) {
    $env:UV_CACHE_DIR = Join-Path $env:TEMP "opennova-uv-cache"
}

# Read importer component version from its source file
$pyproject = Get-Content "$ROOT\pyproject.toml" -Raw
if ($pyproject -notmatch '(?m)^version\s*=\s*"([^"]+)"') {
    throw "Could not extract version from pyproject.toml"
}
$Version = $Matches[1]
Write-Host "=== Importer version: $Version ==="

# Locate Python 3.11 (bpy 5.x requires exactly 3.11)
$PY = (& py -3.11 -c "import sys; print(sys.executable)" 2>$null)
if (-not $PY) {
    Write-Error "Python 3.11 not found. Run: py install 3.11"
    exit 1
}
Write-Host "Using Python: $PY"

# ---------------------------------------------------------------------------
# Bootstrap uv into the 3.11 interpreter
# ---------------------------------------------------------------------------
Write-Host "=== Installing uv ==="
& $PY -m pip install --upgrade pip uv --quiet

# ---------------------------------------------------------------------------
# Install project dependencies via uv (invoked through the same Python)
# ---------------------------------------------------------------------------
Write-Host "=== Installing project dependencies ==="
& $PY -m uv sync --frozen --python $PY --group dev --group blender

Write-Host "=== Checking bpy version ==="
& $PY -m uv run --frozen --group dev --group blender python -c "import bpy; v = bpy.app.version; assert (5, 0, 0) <= v < (6, 0, 0), f'Expected bpy 5.x, got {bpy.app.version_string}'; print(f'Using bpy {bpy.app.version_string}')"

# ---------------------------------------------------------------------------
# Build opennova.dll (needed by the FFI layer at runtime)
# Must be built before PyInstaller and explicitly bundled via --add-binary.
# ---------------------------------------------------------------------------
Write-Host "=== Building opennova.dll ==="
cmake -S . -B build-pkg -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIB=ON
cmake --build build-pkg --target opennova_shared --config Release
$NativeLibDir = "pyopennova\lib\windows-x64"
New-Item -ItemType Directory -Force -Path $NativeLibDir | Out-Null
Copy-Item build-pkg\Release\opennova.dll "$NativeLibDir\opennova.dll"
& $PY -m uv run --frozen --group dev --group blender python -c "from pyopennova._native import _lib_path, load_lib; print(_lib_path()); load_lib()"

# ---------------------------------------------------------------------------
# Build with PyInstaller (single-file exe)
# ---------------------------------------------------------------------------
Write-Host "=== Building onimport.exe ==="
$distPath = "$ROOT\dist"
$ExeName = "onimport-v$Version"
& $PY -m uv run --frozen --group dev --group blender python -m PyInstaller apps\onimport\__main__.py `
    --name $ExeName `
    --onefile `
    --collect-all bpy `
    --collect-all numpy `
    --collect-all PySide6 `
    --add-binary "$NativeLibDir\opennova.dll;$NativeLibDir" `
    --distpath $distPath `
    --noconfirm `
    --clean

Write-Host "=== Verifying packaged native DLL ==="
& "$distPath\$ExeName.exe" scan --dir fixtures/def
if ($LASTEXITCODE -ne 0) {
    throw "Packaged scan verification failed with exit code $LASTEXITCODE"
}

Write-Host "=== Done ==="
Write-Host "  -> dist\$ExeName.exe"
Get-Item "$distPath\$ExeName.exe" | Select-Object Name, Length
