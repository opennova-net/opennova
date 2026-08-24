# Build and export OpenNova Godot applications for Windows.
#
# Produces (version read from godot/project.godot) ONE zip:
#   dist\opennova-windows-v<version>.zip
#       opennova-modtools.exe   the OpenNova Editor (ONED)
#       opennova.exe            the game runtime
#       libopennova.windows.<flavour>.x86_64.dll   the GDExtension both load
#
# One zip on purpose: the editor launches the runtime from its OWN directory
# (ShellGameSession's PACKAGED_RUNTIME_CANDIDATES look for opennova.exe beside
# opennova-modtools.exe for F5/F6), so two zips only ever paired a fresh editor
# with a stale runtime and failed as an unexplained resource picker.
#
# Requires:
#   - MSVC toolchain + cmake (preinstalled on windows-latest)
#   - Git submodules already initialised (third_party/godot-cpp)
#
# Usage:
#   pwsh -File scripts\package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]

param(
    # CI builds the GDExtension once per flavour (the build-gdextension-windows job)
    # and downloads the DLLs into godot\bin; -SkipBuild then skips the per-job
    # rebuild. Local runs omit it and build normally. See
    # .github/workflows/ci.yml build-gdextension-windows.
    [switch]$SkipBuild,
    # Godot export mode. "release" (default; what the release workflow ships):
    # --export-release, and the packaged exe loads the template_release
    # GDExtension. "debug" (pull-request CI): --export-debug, and the packaged exe
    # loads the template_debug GDExtension — the flavour every editor session
    # runs — so a PR only has to compile one flavour. The Godot editor itself
    # always loads template_debug while it scans scripts for the export, so that
    # DLL is required in both modes.
    [ValidateSet("debug", "release")]
    [string]$ExportMode = "release"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # keeps Invoke-WebRequest fast on large files

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

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
    cmake -S godot/src -B $BuildDir "-DGODOTCPP_TARGET=$GodotCppTarget"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    cmake --build $BuildDir --config $Config --target opennova
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    if (-not (Test-Path $ExpectedDll)) {
        throw "GDExtension DLL missing after $GodotCppTarget build: $ExpectedDll"
    }
}

$DEBUG_DLL = "$ROOT\godot\bin\libopennova.windows.template_debug.x86_64.dll"
$RELEASE_DLL = "$ROOT\godot\bin\libopennova.windows.template_release.x86_64.dll"

# Two DLL roles. The Godot editor loads the debug/editor library (template_debug)
# while scanning scripts for ANY export, so it is always required. The packaged
# exe loads the library matching its export mode — a --export-release exe
# resolves the .gdextension windows.release entry (template_release), a
# --export-debug exe the windows.debug entry (template_debug) — and only that
# shipped library is boot-smoked and zipped.
$EDITOR_DLL = $DEBUG_DLL
if ($ExportMode -eq "debug") { $SHIPPED_DLL = $DEBUG_DLL } else { $SHIPPED_DLL = $RELEASE_DLL }
$NeedsReleaseDll = ($ExportMode -eq "release")
Write-Host "=== Export mode: $ExportMode (ships $(Split-Path $SHIPPED_DLL -Leaf)) ==="

if ($SkipBuild) {
    Write-Host "=== -SkipBuild: using prebuilt GDExtension DLLs in godot\bin ==="
    if (-not (Test-Path $EDITOR_DLL)) { throw "Expected prebuilt debug DLL missing: $EDITOR_DLL" }
    if ($NeedsReleaseDll -and -not (Test-Path $RELEASE_DLL)) {
        throw "Expected prebuilt release DLL missing: $RELEASE_DLL"
    }
}
else {
    # RelWithDebInfo, not Debug: the same optimized-plus-symbols flavour
    # scripts/build_godot.sh Dev and CI produce, so a debug-mode package ships
    # the DLL every editor session runs rather than an /Od build.
    Invoke-GDExtensionBuild `
        -GodotCppTarget "template_debug" `
        -BuildDir "build-godot-debug" `
        -Config "RelWithDebInfo" `
        -ExpectedDll $DEBUG_DLL

    if ($NeedsReleaseDll) {
        Invoke-GDExtensionBuild `
            -GodotCppTarget "template_release" `
            -BuildDir "build-godot-release" `
            -Config "Release" `
            -ExpectedDll $RELEASE_DLL
    }
}

# ---------------------------------------------------------------------------
# 4. Run headless exports
# ---------------------------------------------------------------------------
$DIST = "$ROOT\dist"
New-Item -ItemType Directory -Force -Path $DIST | Out-Null

$RUNTIME_EXE = "$DIST\opennova.exe"
$MODTOOLS_EXE = "$DIST\opennova-modtools.exe"
$APPS_ZIP = "$DIST\opennova-windows-v$Version.zip"

function Invoke-GodotExport {
    param([string]$PresetName, [string]$OutputPath)
    # Pass as a single command-line string so spaces in $PresetName survive.
    # Start-Process -Wait blocks until Godot exits; plain `&` has shown to return
    # before Godot finishes writing the .exe under some output-redirection setups.
    # --export-release / --export-debug picks the export template and, through
    # the .gdextension feature tags, which GDExtension flavour the exe loads.
    $arguments = "--headless --path godot --export-$ExportMode `"$PresetName`" `"$OutputPath`""
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
    $dllBeside = Join-Path $exeDir (Split-Path $SHIPPED_DLL -Leaf)
    if (-not (Test-Path $dllBeside)) {
        Copy-Item -LiteralPath $SHIPPED_DLL -Destination $dllBeside -Force
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

Write-Host "=== Exporting opennova-modtools.exe ==="
Invoke-GodotExport -PresetName "OpenNova Mod Tools" -OutputPath $MODTOOLS_EXE
Test-GodotAppBoot -PackageName "opennova-modtools" -ExePath $MODTOOLS_EXE

Write-Host "=== Exporting opennova.exe ==="
Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE
Test-GodotAppBoot -PackageName "opennova-runtime" -ExePath $RUNTIME_EXE

# ---------------------------------------------------------------------------
# 5. One zip: both exes side by side with the single DLL they share
# ---------------------------------------------------------------------------
function New-GodotAppsZip {
    param(
        [string[]]$ExePaths,
        [string]$ZipPath
    )

    foreach ($exe in $ExePaths) {
        if (-not (Test-Path $exe)) {
            throw "Cannot package; exe is missing: $exe"
        }
    }
    if (-not (Test-Path $SHIPPED_DLL)) {
        throw "Cannot package; shipped GDExtension DLL is missing: $SHIPPED_DLL"
    }

    $stageDir = Join-Path $DIST ".stage-opennova-windows"
    if (Test-Path $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

    foreach ($exe in $ExePaths) {
        Copy-Item -LiteralPath $exe -Destination (Join-Path $stageDir (Split-Path $exe -Leaf)) -Force
    }
    Copy-Item -LiteralPath $SHIPPED_DLL -Destination (Join-Path $stageDir (Split-Path $SHIPPED_DLL -Leaf)) -Force

    Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stageDir "*") -DestinationPath $ZipPath -Force
    Remove-Item -LiteralPath $stageDir -Recurse -Force

    if (-not (Test-Path $ZipPath)) {
        throw "Packaging produced no zip at $ZipPath"
    }
}

Write-Host "=== Packaging $(Split-Path $APPS_ZIP -Leaf) ==="
New-GodotAppsZip -ExePaths @($MODTOOLS_EXE, $RUNTIME_EXE) -ZipPath $APPS_ZIP

# ---------------------------------------------------------------------------
# 6. Done
# ---------------------------------------------------------------------------
Write-Host "=== Done ==="
Get-Item -LiteralPath $APPS_ZIP | Select-Object Name, Length
