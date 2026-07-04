# Build and export OpenNova Godot applications for Windows.
#
# Produces (version read from godot/project.godot):
#   dist\opennova-runtime-windows-v<version>.zip    (Runtime exe + GDExtension DLL)
#   dist\opennova-modtools-windows-v<version>.zip   (Mod Tools exe + GDExtension DLL)
#
# Requires:
#   - MSVC toolchain + cmake (preinstalled on windows-latest)
#   - Git submodules already initialised (third_party/godot-cpp)
#
# Usage:
#   pwsh -File scripts\package_godot_windows.ps1 [-Target all|editor|runtime]

param(
    [ValidateSet("all", "editor", "runtime")]
    [string]$Target = "all",
    # CI builds the GDExtension once (the build-gdextension-windows job) and downloads
    # the DLLs into godot\bin; -SkipBuild then skips the per-job rebuild. Local runs
    # omit it and build normally. See .github/workflows/ci.yml build-gdextension-windows.
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # keeps Invoke-WebRequest fast on large files

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

$PackageEditor = $Target -in @("all", "editor")
$PackageRuntime = $Target -in @("all", "runtime")
Write-Host "=== Godot package target: $Target ==="

# Read Godot apps' component version from project.godot
$projectGodot = Get-Content "$ROOT\godot\project.godot" -Raw
if ($projectGodot -notmatch '(?m)^config/version\s*=\s*"([^"]+)"') {
    throw "Could not extract config/version from godot/project.godot"
}
$Version = $Matches[1]
Write-Host "=== Godot apps version: $Version ==="

# Pad to 4 numeric parts for Windows VERSIONINFO (Godot requires major.minor.patch.build)
$versionParts = @($Version.Split("."))
while ($versionParts.Count -lt 4) { $versionParts += "0" }
$WinVersion = $versionParts -join "."

# Stamp version into both export presets so Godot embeds it in the .exe VERSIONINFO
$presetsPath = "$ROOT\godot\export_presets.cfg"
$presets = Get-Content $presetsPath -Raw
$presets = $presets -replace 'application/file_version="[^"]*"',    "application/file_version=`"$WinVersion`""
$presets = $presets -replace 'application/product_version="[^"]*"', "application/product_version=`"$WinVersion`""
Set-Content -Path $presetsPath -Value $presets -NoNewline
Write-Host "    file_version / product_version -> $WinVersion"

$GODOT_VERSION = "4.6.1-stable"
$GODOT_VERSION_PLAIN = "4.6.1"
$GODOT_ZIP_URL = "https://github.com/godotengine/godot/releases/download/$GODOT_VERSION/Godot_v${GODOT_VERSION}_win64.exe.zip"
$TEMPLATES_URL = "https://github.com/godotengine/godot/releases/download/$GODOT_VERSION/Godot_v${GODOT_VERSION}_export_templates.tpz"
$TEMPLATES_DIR = "$env:APPDATA\Godot\export_templates\${GODOT_VERSION_PLAIN}.stable"

$GODOT_DIR = "$ROOT\.godot-bin"
$GODOT_EXE = "$GODOT_DIR\Godot_v${GODOT_VERSION}_win64.exe"

# ---------------------------------------------------------------------------
# 1. Install Godot editor (skip if already present)
# ---------------------------------------------------------------------------
if (-not (Test-Path $GODOT_EXE)) {
    Write-Host "=== Downloading Godot $GODOT_VERSION ==="
    New-Item -ItemType Directory -Force -Path $GODOT_DIR | Out-Null
    $tempZip = "$GODOT_DIR\godot.zip"
    Invoke-WebRequest -Uri $GODOT_ZIP_URL -OutFile $tempZip
    Expand-Archive -Path $tempZip -DestinationPath $GODOT_DIR -Force
    Remove-Item $tempZip
}
if (-not (Test-Path $GODOT_EXE)) {
    throw "Godot executable missing after extraction: $GODOT_EXE"
}

# ---------------------------------------------------------------------------
# 2. Install export templates (skip if already present)
# ---------------------------------------------------------------------------
if (-not (Test-Path "$TEMPLATES_DIR\version.txt")) {
    Write-Host "=== Downloading export templates ==="
    New-Item -ItemType Directory -Force -Path $TEMPLATES_DIR | Out-Null
    $tempTpz = "$GODOT_DIR\templates.zip"
    Invoke-WebRequest -Uri $TEMPLATES_URL -OutFile $tempTpz
    $extractDir = "$GODOT_DIR\templates-extract"
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
    Expand-Archive -Path $tempTpz -DestinationPath $extractDir -Force
    # .tpz archives contain a "templates" root folder
    Copy-Item "$extractDir\templates\*" $TEMPLATES_DIR -Recurse -Force
    Remove-Item $tempTpz
    Remove-Item $extractDir -Recurse -Force
}

# ---------------------------------------------------------------------------
# 3. Build GDExtension binaries
# ---------------------------------------------------------------------------
function Invoke-GDExtensionBuild {
    param(
        [string]$GodotCppTarget,
        [string]$BuildDir,
        [string]$Config,
        [string]$ExpectedDll
    )

    Write-Host "=== Building $GodotCppTarget GDExtension ==="
    cmake -S godot/engine -B $BuildDir "-DGODOTCPP_TARGET=$GodotCppTarget"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    cmake --build $BuildDir --config $Config --target opennova
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    if (-not (Test-Path $ExpectedDll)) {
        throw "GDExtension DLL missing after $GodotCppTarget build: $ExpectedDll"
    }
}

$DEBUG_DLL = "$ROOT\godot\bin\libopennova.windows.template_debug.x86_64.dll"
$RELEASE_DLL = "$ROOT\godot\bin\libopennova.windows.template_release.x86_64.dll"

# The Godot editor loads the debug/editor library while scanning scripts for an
# export. The release export template then needs the release library for the
# packaged app. Fresh CI runners must have both.
if ($SkipBuild) {
    Write-Host "=== -SkipBuild: using prebuilt GDExtension DLLs in godot\bin ==="
    if (-not (Test-Path $DEBUG_DLL))   { throw "Expected prebuilt debug DLL missing: $DEBUG_DLL" }
    if (-not (Test-Path $RELEASE_DLL)) { throw "Expected prebuilt release DLL missing: $RELEASE_DLL" }
}
else {
    Invoke-GDExtensionBuild `
        -GodotCppTarget "template_debug" `
        -BuildDir "build-godot-debug" `
        -Config "Debug" `
        -ExpectedDll $DEBUG_DLL

    Invoke-GDExtensionBuild `
        -GodotCppTarget "template_release" `
        -BuildDir "build-godot-release" `
        -Config "Release" `
        -ExpectedDll $RELEASE_DLL
}

# ---------------------------------------------------------------------------
# 4. Run headless exports
# ---------------------------------------------------------------------------
$DIST = "$ROOT\dist"
New-Item -ItemType Directory -Force -Path $DIST | Out-Null

$RUNTIME_EXE = "$DIST\opennova.exe"
$MODTOOLS_EXE = "$DIST\opennova-modtools.exe"
$RUNTIME_ZIP = "$DIST\opennova-runtime-windows-v$Version.zip"
$MODTOOLS_ZIP = "$DIST\opennova-modtools-windows-v$Version.zip"

function Invoke-GodotExport {
    param([string]$PresetName, [string]$OutputPath)
    # Pass as a single command-line string so spaces in $PresetName survive.
    # Start-Process -Wait blocks until Godot exits; plain `&` has shown to return
    # before Godot finishes writing the .exe under some output-redirection setups.
    $arguments = "--headless --path godot --export-release `"$PresetName`" `"$OutputPath`""
    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()

    try {
        $proc = Start-Process `
            -FilePath $GODOT_EXE `
            -ArgumentList $arguments `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $stdout = Get-Content $stdoutLog -Raw
        $stderr = Get-Content $stderrLog -Raw
        $combinedOutput = "$stdout`n$stderr"

        if ($stdout) { Write-Host $stdout.TrimEnd() }
        if ($stderr) { Write-Host $stderr.TrimEnd() }

        if ($proc.ExitCode -ne 0) {
            throw "Godot export of '$PresetName' exited with code $($proc.ExitCode)"
        }

        if ($combinedOutput -match "SCRIPT ERROR: Parse Error|GDExtension dynamic library not found|No loader found for resource|Failed to load script") {
            throw "Godot export of '$PresetName' logged script or GDExtension load errors"
        }
    }
    finally {
        Remove-Item $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
    }

    if (-not (Test-Path $OutputPath)) {
        throw "Godot export of '$PresetName' produced no file at $OutputPath"
    }
}

# Boot the exported exe headless for a few frames. This is the guard the
# packaging pipeline was missing: the export step only validates export-time
# logs, so a package whose main scene cannot load (e.g. a missing per-product
# run/main_scene feature override in project.godot) still shipped, crashing on
# first launch. Requires the GDExtension DLL beside the exe, exactly like the
# shipped zip layout.
function Test-GodotAppBoot {
    param([string]$PackageName, [string]$ExePath)

    Write-Host "=== Boot smoke: $PackageName ==="
    $exeDir = Split-Path $ExePath -Parent
    $dllBeside = Join-Path $exeDir (Split-Path $RELEASE_DLL -Leaf)
    if (-not (Test-Path $dllBeside)) {
        Copy-Item -LiteralPath $RELEASE_DLL -Destination $dllBeside -Force
    }

    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()
    try {
        $proc = Start-Process `
            -FilePath $ExePath `
            -ArgumentList "--headless --quit-after 120 --verbose" `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $combinedOutput = "$(Get-Content $stdoutLog -Raw)`n$(Get-Content $stderrLog -Raw)"

        if ($proc.ExitCode -ne 0) {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke for '$PackageName' exited with code $($proc.ExitCode)"
        }
        if ($combinedOutput -match "Failed loading scene|Cannot open file 'res://|SCRIPT ERROR|GDExtension dynamic library not found|Failed to load script") {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke for '$PackageName' logged load errors"
        }
    }
    finally {
        Remove-Item $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
    }
}

if ($PackageEditor) {
    Write-Host "=== Exporting opennova-modtools.exe ==="
    Invoke-GodotExport -PresetName "OpenNova Mod Tools" -OutputPath $MODTOOLS_EXE
    Test-GodotAppBoot -PackageName "opennova-modtools" -ExePath $MODTOOLS_EXE
}

if ($PackageRuntime) {
    Write-Host "=== Exporting opennova.exe ==="
    Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE
    Test-GodotAppBoot -PackageName "opennova-runtime" -ExePath $RUNTIME_EXE
}

function New-GodotAppZip {
    param(
        [string]$PackageName,
        [string]$ExePath,
        [string]$ZipPath
    )

    if (-not (Test-Path $ExePath)) {
        throw "Cannot package '$PackageName'; exe is missing: $ExePath"
    }
    if (-not (Test-Path $RELEASE_DLL)) {
        throw "Cannot package '$PackageName'; release DLL is missing: $RELEASE_DLL"
    }

    $stageDir = Join-Path $DIST ".stage-$PackageName"
    if (Test-Path $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    Copy-Item -LiteralPath $ExePath -Destination (Join-Path $stageDir (Split-Path $ExePath -Leaf)) -Force
    Copy-Item -LiteralPath $RELEASE_DLL -Destination (Join-Path $stageDir (Split-Path $RELEASE_DLL -Leaf)) -Force

    Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $ZipPath -Force
    Remove-Item -LiteralPath $stageDir -Recurse -Force

    if (-not (Test-Path $ZipPath)) {
        throw "Package '$PackageName' produced no zip at $ZipPath"
    }
}

$ProducedZips = @()

if ($PackageEditor) {
    Write-Host "=== Packaging $(Split-Path $MODTOOLS_ZIP -Leaf) ==="
    New-GodotAppZip -PackageName "opennova-modtools" -ExePath $MODTOOLS_EXE -ZipPath $MODTOOLS_ZIP
    $ProducedZips += $MODTOOLS_ZIP
}

if ($PackageRuntime) {
    Write-Host "=== Packaging $(Split-Path $RUNTIME_ZIP -Leaf) ==="
    New-GodotAppZip -PackageName "opennova-runtime" -ExePath $RUNTIME_EXE -ZipPath $RUNTIME_ZIP
    $ProducedZips += $RUNTIME_ZIP
}

# ---------------------------------------------------------------------------
# 5. Done
# ---------------------------------------------------------------------------
Write-Host "=== Done ==="
Get-Item -LiteralPath $ProducedZips | Select-Object Name, Length
