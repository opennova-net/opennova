# Build and export OpenNova Godot applications for Windows.
#
# Produces (version read from godot/project.godot) the workflow's TWO zips:
#
#   dist\opennova-windows-v<version>.zip           the DEV build (PRs + non-tag)
#       opennova.exe            the game; default-mounts the assets\ beside it (loose)
#       opennova-modtools.exe   ONED; same default source tree
#       libopennova.windows.<flavour>.x86_64.dll
#       assets\                 the game's tracked loose sources — ONE copy of the data
#
#   dist\opennova-game-windows-v<version>.zip      the GAME build (tagged releases)
#       opennova.exe            default-mounts its own dir, retail-style
#       libopennova.windows.<flavour>.x86_64.dll
#       localres.pff            the packed game, built by ONED's
#                               --pack-game CLI over the staged sources
#       menumus.sbf gamemus.sbf earlyerr.txt       loose by contract
#
# Both are built on every run so pull-request CI exercises the pack CLI long
# before a tag needs it. The dev zip keeps ONED beside the runtime because
# ONED runs the sibling opennova.exe against the loose assets tree.
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
    # loads the template_debug GDExtension — the development flavour — so a PR
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

# The imgui-godot addon (the ImGui bridge for ONED's surface and the game's dev
# tools, ADR 0039) must be installed for BOTH export modes: ONED ships it always,
# the game's export plugin keeps it for a debug export and strips it from a
# release export (export_presets.cfg imgui/debug, imgui/release). Only the addon
# script runs here: an export must never carry GUT.
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
    # the DLL source/debug game and ONED runs use rather than an /Od build.
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
$GAME_ZIP = "$DIST\opennova-game-windows-v$Version.zip"

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
# 5. Stage the game sources: the TRACKED files of assets/ (never a wildcard copy
#    — the working assets/ dir doubles as the retail validation game dir and
#    holds untracked retail binaries that must never ship).
# ---------------------------------------------------------------------------
function Copy-GameSources {
    param([string]$AssetsStageDir)

    New-Item -ItemType Directory -Force -Path $AssetsStageDir | Out-Null
    $tracked = & git -C $ROOT ls-files -z assets
    if ($LASTEXITCODE -ne 0) { throw "git ls-files assets failed (exit $LASTEXITCODE)" }
    $names = $tracked -split "`0" | Where-Object { $_ }
    # Repo metadata rides the tree but is not game data.
    $names = $names | Where-Object { (Split-Path $_ -Leaf) -notin @(".gitignore", "README.md") }
    if ($names.Count -eq 0) { throw "No tracked asset files found under assets/" }

    $lfsSentinel = [System.Text.Encoding]::ASCII.GetBytes("version https://git-lfs")
    foreach ($name in $names) {
        $src = Join-Path $ROOT ($name -replace "/", "\")
        if (-not (Test-Path -LiteralPath $src)) { throw "Tracked asset missing from the working tree: $name" }
        # git archive would emit LFS POINTERS for the binary majority of this tree;
        # the working tree carries the smudged bytes, but only when LFS pulled — guard.
        $head = [System.IO.File]::ReadAllBytes($src)
        if ($head.Length -ge $lfsSentinel.Length) {
            $prefix = $head[0..($lfsSentinel.Length - 1)]
            if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$prefix, $lfsSentinel)) {
                throw "$name is an unpulled git-LFS pointer; run 'git lfs pull' first"
            }
        }
        $rel = $name -replace "^assets/", "" -replace "/", "\"
        $dst = Join-Path $AssetsStageDir $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
    Write-Host "    staged $($names.Count) tracked asset files"
}

# ---------------------------------------------------------------------------
# 6. Pack the game with ONED's hidden command. The shipped
#    localres.pff is produced by the shipped opennova-modtools.exe; --pack-game
#    runs headless and drops the archive plus loose-by-contract files into the
#    game-zip stage.
# ---------------------------------------------------------------------------
function Invoke-PackGame {
    param([string]$AssetsDir, [string]$GameDir)

    Write-Host "=== Packing the game with ONED ==="
    $dllBeside = Join-Path (Split-Path $MODTOOLS_EXE -Parent) (Split-Path $SHIPPED_DLL -Leaf)
    if (-not (Test-Path $dllBeside)) {
        Copy-Item -LiteralPath $SHIPPED_DLL -Destination $dllBeside -Force
    }
    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()
    try {
        $proc = Start-Process `
            -FilePath $MODTOOLS_EXE `
            -ArgumentList "--headless -- --pack-game `"$AssetsDir`" `"$GameDir`"" `
            -NoNewWindow `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog
        $combined = "$(Get-Content $stdoutLog -Raw)`n$(Get-Content $stderrLog -Raw)"
        Write-Host $combined.TrimEnd()
        if ($proc.ExitCode -ne 0) {
            throw "pack-game exited with code $($proc.ExitCode)"
        }
    }
    finally {
        Remove-Item $stdoutLog, $stderrLog -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path (Join-Path $GameDir "localres.pff"))) {
        throw "pack-game produced no localres.pff in $GameDir"
    }
}

# ---------------------------------------------------------------------------
# 7. The two zips. Dev: both exes + DLL + loose assets\ (one copy of the data;
#    the game default-mounts it and ONED offers it as the implicit selection).
#    Game: opennova.exe + DLL + the packed game, which default-mounts its own
#    dir, retail-style.
# ---------------------------------------------------------------------------
function New-StageDir {
    param([string]$Name)
    $stageDir = Join-Path $DIST $Name
    if (Test-Path $stageDir) {
        Remove-Item -LiteralPath $stageDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
    return $stageDir
}

function Compress-Stage {
    param([string]$StageDir, [string]$ZipPath)
    Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -Force
    if (-not (Test-Path $ZipPath)) {
        throw "Packaging produced no zip at $ZipPath"
    }
}

foreach ($exe in @($MODTOOLS_EXE, $RUNTIME_EXE)) {
    if (-not (Test-Path $exe)) { throw "Cannot package; exe is missing: $exe" }
}
if (-not (Test-Path $SHIPPED_DLL)) {
    throw "Cannot package; shipped GDExtension DLL is missing: $SHIPPED_DLL"
}
$dllLeaf = Split-Path $SHIPPED_DLL -Leaf

# Godot exports every GDExtension library beside the exe. ONED's ImGui surface
# needs the imgui-godot addon's own library in BOTH modes (it rides the dev zip),
# so its export keeps the addon; the game's release export strips it (the game
# zip carries the library only for a debug export, whose dev tools use it).
$imguiLibs = @(Get-ChildItem -LiteralPath $DIST -Filter "libimgui-godot-native.*" -File -ErrorAction SilentlyContinue)
if ($imguiLibs.Count -eq 0) {
    throw "The exports are missing the imgui-godot addon library beside the exes (is the addon installed and enabled?)"
}
function Copy-ImGuiAddonLibraries {
    param([string]$StageDir)
    foreach ($lib in $imguiLibs) {
        Copy-Item -LiteralPath $lib.FullName -Destination (Join-Path $StageDir $lib.Name) -Force
    }
}

Write-Host "=== Packaging $(Split-Path $APPS_ZIP -Leaf) (dev build) ==="
$devStage = New-StageDir -Name ".stage-opennova-windows"
foreach ($exe in @($MODTOOLS_EXE, $RUNTIME_EXE)) {
    Copy-Item -LiteralPath $exe -Destination (Join-Path $devStage (Split-Path $exe -Leaf)) -Force
}
Copy-Item -LiteralPath $SHIPPED_DLL -Destination (Join-Path $devStage $dllLeaf) -Force
Copy-ImGuiAddonLibraries -StageDir $devStage
Copy-GameSources -AssetsStageDir (Join-Path $devStage "assets")
Compress-Stage -StageDir $devStage -ZipPath $APPS_ZIP

Write-Host "=== Packaging $(Split-Path $GAME_ZIP -Leaf) (tagged-release build) ==="
$gameStage = New-StageDir -Name ".stage-opennova-game-windows"
Copy-Item -LiteralPath $RUNTIME_EXE -Destination (Join-Path $gameStage "opennova.exe") -Force
Copy-Item -LiteralPath $SHIPPED_DLL -Destination (Join-Path $gameStage $dllLeaf) -Force
if ($ExportMode -eq "debug") { Copy-ImGuiAddonLibraries -StageDir $gameStage }
Invoke-PackGame -AssetsDir (Join-Path $devStage "assets") -GameDir $gameStage
Compress-Stage -StageDir $gameStage -ZipPath $GAME_ZIP

Remove-Item -LiteralPath $devStage -Recurse -Force
Remove-Item -LiteralPath $gameStage -Recurse -Force

# ---------------------------------------------------------------------------
# 8. Done
# ---------------------------------------------------------------------------
Write-Host "=== Done ==="
Get-Item -LiteralPath @($APPS_ZIP, $GAME_ZIP) | Select-Object Name, Length
