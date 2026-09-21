# Build and export the Windows products. Game data is supplied with --resource-dir.
# Produces dist/opennova-game-windows-v<version>.zip (opennova.exe and its matching
# native dependencies) and dist/opennova-editor-windows-v<version>.zip (editor/ =
# the OpenNova Editor, runtime/ = the game it plays a project with, ADR 0046 d4).
# Debug exports include the game's ImGui tools; the editor carries ImGui in both.
# Usage: pwsh -File scripts/package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]

param(
    # CI builds the GDExtension once per flavour (the build-gdextension-windows job)
    # and downloads the DLLs into godot\bin; -SkipBuild then skips the per-job
    # rebuild. Local runs omit it and build normally. See
    # .github/workflows/ci.yml build-gdextension-windows.
    [switch]$SkipBuild,
    # Godot export mode. "release" (default; what the release workflow ships):
    # --export-release, and the packaged exes load the template_release
    # GDExtensions. "debug" (pull-request CI): --export-debug, and the packaged exes
    # load the template_debug GDExtensions — the development flavour — so a PR
    # only has to compile one flavour. The Godot editor itself
    # always loads template_debug while it scans scripts for the export, so that
    # DLL is required in both modes.
    [ValidateSet("debug", "release")]
    [string]$ExportMode = "release"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"  # keeps Invoke-WebRequest fast on large files

$ROOT = (Resolve-Path "$PSScriptRoot\..").Path
Set-Location $ROOT

# Install the ImGui bridge: the game's dev tools (debug) and the editor's windows.
Write-Host "=== Installing the imgui-godot addon ==="
& bash "scripts/bootstrap_imgui_godot.sh"
if ($LASTEXITCODE -ne 0) { throw "bootstrap_imgui_godot.sh failed (exit $LASTEXITCODE)" }

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

# Stamp version into the export presets so Godot embeds it in the .exe VERSIONINFO
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
# 3. Build GDExtension binaries: both variants of one flavour per configure
#    (ADR 0046 d4): libopennova.* (runtime-only: the game, the Play child) and
#    libopennova_editor.* (the editor).
# ---------------------------------------------------------------------------
function Invoke-GDExtensionBuild {
    param(
        [string]$GodotCppTarget,
        [string]$BuildDir,
        [string]$Config,
        [string[]]$ExpectedDlls
    )

    Write-Host "=== Building $GodotCppTarget GDExtensions ==="
    cmake -S godot/src -B $BuildDir "-DGODOTCPP_TARGET=$GodotCppTarget"
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    cmake --build $BuildDir --config $Config --target opennova opennova_editor_gdext
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $GodotCppTarget (exit $LASTEXITCODE)" }

    foreach ($dll in $ExpectedDlls) {
        if (-not (Test-Path $dll)) {
            throw "GDExtension DLL missing after $GodotCppTarget build: $dll"
        }
    }
}

$RUNTIME_DEBUG_DLL = "$ROOT\godot\bin\libopennova.windows.template_debug.x86_64.dll"
$RUNTIME_RELEASE_DLL = "$ROOT\godot\bin\libopennova.windows.template_release.x86_64.dll"
$EDITOR_DEBUG_DLL = "$ROOT\godot\bin\libopennova_editor.windows.template_debug.x86_64.dll"
$EDITOR_RELEASE_DLL = "$ROOT\godot\bin\libopennova_editor.windows.template_release.x86_64.dll"

# DLL roles. The Godot editor loads the plain windows.debug row of
# bin/opennova.gdextension (the editor-enabled template_debug library, the superset)
# while scanning scripts for ANY export, so it is always required. Each packaged exe
# loads the library its feature tag and export mode select: the game and the Play
# child the runtime-only one, the editor the editor-enabled one — and only those
# shipped libraries are boot-smoked and zipped.
$SCAN_DLL = $EDITOR_DEBUG_DLL
if ($ExportMode -eq "debug") {
    $SHIPPED_RUNTIME_DLL = $RUNTIME_DEBUG_DLL
    $SHIPPED_EDITOR_DLL = $EDITOR_DEBUG_DLL
} else {
    $SHIPPED_RUNTIME_DLL = $RUNTIME_RELEASE_DLL
    $SHIPPED_EDITOR_DLL = $EDITOR_RELEASE_DLL
}
$NeedsReleaseDll = ($ExportMode -eq "release")
Write-Host "=== Export mode: $ExportMode (ships $(Split-Path $SHIPPED_RUNTIME_DLL -Leaf) and $(Split-Path $SHIPPED_EDITOR_DLL -Leaf)) ==="

if ($SkipBuild) {
    Write-Host "=== -SkipBuild: using prebuilt GDExtension DLLs in godot\bin ==="
    foreach ($dll in @($SCAN_DLL, $RUNTIME_DEBUG_DLL)) {
        if (-not (Test-Path $dll)) { throw "Expected prebuilt debug DLL missing: $dll" }
    }
    if ($NeedsReleaseDll) {
        foreach ($dll in @($RUNTIME_RELEASE_DLL, $EDITOR_RELEASE_DLL)) {
            if (-not (Test-Path $dll)) { throw "Expected prebuilt release DLL missing: $dll" }
        }
    }
}
else {
    # RelWithDebInfo, not Debug: the same optimized-plus-symbols flavour
    # scripts/build_godot.sh Dev and CI produce, so a debug-mode package ships
    # the DLL source/debug game runs use rather than an /Od build.
    Invoke-GDExtensionBuild `
        -GodotCppTarget "template_debug" `
        -BuildDir "build-godot-debug" `
        -Config "RelWithDebInfo" `
        -ExpectedDlls @($RUNTIME_DEBUG_DLL, $EDITOR_DEBUG_DLL)

    if ($NeedsReleaseDll) {
        Invoke-GDExtensionBuild `
            -GodotCppTarget "template_release" `
            -BuildDir "build-godot-release" `
            -Config "Release" `
            -ExpectedDlls @($RUNTIME_RELEASE_DLL, $EDITOR_RELEASE_DLL)
    }
}

# ---------------------------------------------------------------------------
# 4. Run headless exports
# ---------------------------------------------------------------------------
$DIST = "$ROOT\dist"
New-Item -ItemType Directory -Force -Path $DIST | Out-Null

$RUNTIME_EXE = "$DIST\opennova.exe"
$PLAY_EXE = "$DIST\play\opennova.exe"
$EDITOR_EXE = "$DIST\editor\opennova-editor.exe"
$GAME_ZIP = "$DIST\opennova-game-windows-v$Version.zip"
$EDITOR_ZIP = "$DIST\opennova-editor-windows-v$Version.zip"

function Invoke-GodotExport {
    param([string]$PresetName, [string]$OutputPath)
    New-Item -ItemType Directory -Force -Path (Split-Path $OutputPath -Parent) | Out-Null
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
            -WindowStyle Hidden `
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
# shipped zip layout. The game products must refuse to start without data (exit 2,
# the usage line); the editor must load its variant and say so (exit 0).
function Test-GodotAppBoot {
    param(
        [string]$PackageName,
        [string]$ExePath,
        [string]$DllPath,
        [string]$GameArguments,
        [int]$ExpectedExit,
        [string]$ExpectedOutput
    )

    Write-Host "=== Boot smoke: $PackageName ==="
    $exeDir = Split-Path $ExePath -Parent
    $dllBeside = Join-Path $exeDir (Split-Path $DllPath -Leaf)
    # The export directory survives repeated -SkipBuild validation runs. Always
    # refresh the side-by-side extension so the smoke cannot execute a DLL from
    # an earlier build while claiming to validate the current source tree.
    Copy-Item -LiteralPath $DllPath -Destination $dllBeside -Force

    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()
    $smokeTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $bootProfile = Join-Path $smokeTempRoot ("opennova-package-smoke-" + [Guid]::NewGuid().ToString("N"))
    $previousAppData = [Environment]::GetEnvironmentVariable("APPDATA", "Process")
    try {
        # This startup smoke must not discover the packager's saved retail mount,
        # credentials, or editor state. Music with retail data is validated through
        # the runtime's orderly quit; --quit-after bypasses its playback drain.
        New-Item -ItemType Directory -Path $bootProfile | Out-Null
        [Environment]::SetEnvironmentVariable("APPDATA", $bootProfile, "Process")
        # Headless uses Godot's Dummy renderer, so a render loop cannot validate
        # GPU work here. Disable it to keep --quit-after from racing render-server
        # teardown while this smoke still loads the product scene, scripts,
        # shaders, and GDExtension.
        $proc = Start-Process `
            -FilePath $ExePath `
            -ArgumentList "--headless --disable-render-loop --disable-crash-handler --quit-after 120 --verbose $GameArguments" `
            -WindowStyle Hidden `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $combinedOutput = "$(Get-Content $stdoutLog -Raw)`n$(Get-Content $stderrLog -Raw)"

        if ($proc.ExitCode -ne $ExpectedExit -or $combinedOutput -notmatch $ExpectedOutput) {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke for '$PackageName' exited with code $($proc.ExitCode) (expected $ExpectedExit and '$ExpectedOutput')"
        }
        if ($combinedOutput -match "Failed loading scene|Cannot open file 'res://|SCRIPT ERROR|GDExtension dynamic library not found|Failed to load script") {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke for '$PackageName' logged load errors"
        }
    }
    finally {
        [Environment]::SetEnvironmentVariable("APPDATA", $previousAppData, "Process")
        Remove-Item -LiteralPath $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $bootProfile) {
            $resolvedProfile = (Resolve-Path -LiteralPath $bootProfile).Path
            $tempPrefix = $smokeTempRoot.TrimEnd('\') + '\'
            if (-not $resolvedProfile.StartsWith($tempPrefix, [StringComparison]::OrdinalIgnoreCase) -or
                ((Get-Item -LiteralPath $bootProfile).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                throw "Refusing to remove unexpected smoke profile path: $resolvedProfile"
            }
            Remove-Item -LiteralPath $resolvedProfile -Recurse -Force
        }
    }
}

$usagePattern = "OpenNova requires game data.*--resource-dir"
Write-Host "=== Exporting opennova.exe ==="
Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE
Test-GodotAppBoot -PackageName "opennova-runtime" -ExePath $RUNTIME_EXE -DllPath $SHIPPED_RUNTIME_DLL `
    -GameArguments "" -ExpectedExit 2 -ExpectedOutput $usagePattern

Write-Host "=== Exporting the editor's Play runtime ==="
Invoke-GodotExport -PresetName "OpenNova Play Runtime" -OutputPath $PLAY_EXE
Test-GodotAppBoot -PackageName "opennova-play-runtime" -ExePath $PLAY_EXE -DllPath $SHIPPED_RUNTIME_DLL `
    -GameArguments "" -ExpectedExit 2 -ExpectedOutput $usagePattern

Write-Host "=== Exporting opennova-editor.exe ==="
Invoke-GodotExport -PresetName "OpenNova Editor" -OutputPath $EDITOR_EXE
Test-GodotAppBoot -PackageName "opennova-editor" -ExePath $EDITOR_EXE -DllPath $SHIPPED_EDITOR_DLL `
    -GameArguments "-- --editor-smoke" -ExpectedExit 0 -ExpectedOutput "OpenNova Editor: smoke ok"

# A staging directory under dist/, wiped and recreated; never a data directory.
function New-StageDir {
    param([string]$Name)
    $stage = Join-Path $DIST $Name
    $stagePath = [IO.Path]::GetFullPath($stage)
    $distPrefix = [IO.Path]::GetFullPath($DIST).TrimEnd('\') + '\'
    if (-not $stagePath.StartsWith($distPrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unexpected stage path: $stagePath"
    }
    if (Test-Path -LiteralPath $stagePath) {
        if ((Get-Item -LiteralPath $stagePath).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to remove a linked stage directory: $stagePath"
        }
        Remove-Item -LiteralPath $stagePath -Recurse -Force
    }
    New-Item -ItemType Directory -Path $stagePath | Out-Null
    return $stagePath
}

# The ImGui addon's native library the export placed beside an exe (the game's
# debug tools; the editor's windows in every mode).
function Copy-ImGuiLibs {
    param([string]$ExeDir, [string]$Destination, [bool]$Required)
    $imguiLibs = @(Get-ChildItem -LiteralPath $ExeDir -Filter "libimgui-godot-native.*" -File)
    if ($Required -and $imguiLibs.Count -eq 0) { throw "Export in $ExeDir is missing the ImGui addon library" }
    foreach ($lib in $imguiLibs) { Copy-Item -LiteralPath $lib.FullName -Destination $Destination }
}

# --- the game zip: opennova.exe + its runtime DLL ---
$stagePath = New-StageDir ".stage-opennova-game-windows"
try {
    Copy-Item -LiteralPath $RUNTIME_EXE -Destination (Join-Path $stagePath "opennova.exe")
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $stagePath
    Copy-ImGuiLibs -ExeDir $DIST -Destination $stagePath -Required ($ExportMode -eq "debug")
    $launchHelp = @"
OpenNova requires your own game data.

Packed install:
  opennova.exe -- --resource-dir "C:\Games\Joint Operations"
Loose data:
  opennova.exe -- --resource-dir "C:\MyGameData" --loose-root /d

Use /game <code> and /exp <name> to select a game or expansion.
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $launchHelp -Encoding UTF8
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $GAME_ZIP -Force
    if (-not (Test-Path -LiteralPath $GAME_ZIP)) { throw "Packaging produced no zip" }
}
finally {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}

# --- the editor zip: editor/ (the editor + its DLL + ImGui) and runtime/ (the Play
#     child + the runtime DLL), the layout the editor's Play looks for ---
$stagePath = New-StageDir ".stage-opennova-editor-windows"
try {
    $editorDir = Join-Path $stagePath "editor"
    $runtimeDir = Join-Path $stagePath "runtime"
    New-Item -ItemType Directory -Path $editorDir, $runtimeDir | Out-Null
    Copy-Item -LiteralPath $EDITOR_EXE -Destination (Join-Path $editorDir "opennova-editor.exe")
    Copy-Item -LiteralPath $SHIPPED_EDITOR_DLL -Destination $editorDir
    Copy-ImGuiLibs -ExeDir (Split-Path $EDITOR_EXE -Parent) -Destination $editorDir -Required $true
    Copy-Item -LiteralPath $PLAY_EXE -Destination (Join-Path $runtimeDir "opennova.exe")
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $runtimeDir
    Copy-ImGuiLibs -ExeDir (Split-Path $PLAY_EXE -Parent) -Destination $runtimeDir -Required ($ExportMode -eq "debug")
    $editorHelp = @"
OpenNova Editor (experimental).

  editor\opennova-editor.exe   the editor: create or open a project, fill in the
                               files the game needs, build, play
  runtime\opennova.exe         the game the editor's Play runs your project with

Keep the two folders side by side: Play looks for runtime\opennova.exe next to
the editor folder (or set another runtime under Project in the editor).
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $editorHelp -Encoding UTF8
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $EDITOR_ZIP -Force
    if (-not (Test-Path -LiteralPath $EDITOR_ZIP)) { throw "Packaging produced no editor zip" }
}
finally {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
Write-Host "=== Done ==="
Get-Item -LiteralPath $GAME_ZIP, $EDITOR_ZIP | Select-Object Name, Length
