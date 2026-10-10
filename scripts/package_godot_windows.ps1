# Build and export the Windows products. Produces
# dist/opennova-game-windows-v<version>.zip (opennova.exe, its matching native
# dependencies and, in assets/, OpenNova's own game: the OpenNova Editor's export of
# the base game project assets/, ADR 0048 d8, with each expansion project under
# expansions/ exported into its expansion/<name>/, ADR 0046 T5) and
# dist/opennova-editor-windows-v<version>.zip (editor/ = the OpenNova Editor,
# runtime/ = the game it plays a project with, ADR 0046 d4). Retail game data is
# supplied by the player. Debug exports include the game's ImGui tools; the
# editor carries ImGui in both.
# Usage: pwsh -File scripts/package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]
#        [-ProjectCli <opennova-project.exe>]

param(
    # CI builds the GDExtension once per flavour (the build-gdextension-windows job)
    # and downloads the DLLs into godot\bin; -SkipBuild then skips the per-job
    # rebuild. Local runs omit it and build normally. See
    # .github/workflows/ci.yml build-gdextension-windows.
    [switch]$SkipBuild,
    # The OpenNova Editor's project CLI that builds the base game the game zip
    # ships. Left out, it is built from the root CMake project into build/, as
    # scripts/build.sh configures it (CMAKE_GENERATOR and the compiler launcher
    # from the environment, as CI's package job sets them).
    [string]$ProjectCli = "",
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
# 3b. The base game's builder: opennova-project, the OpenNova Editor's project
#     session on the command line (ADR 0046 S13 A7). The game zip ships the
#     editor's export of assets/ (ADR 0048 d8), never the project's own files:
#     its imports (the terrain set, the texture sources) make files only its
#     Build has.
# ---------------------------------------------------------------------------
if ($ProjectCli) {
    $PROJECT_CLI = (Resolve-Path -LiteralPath $ProjectCli).Path
}
else {
    Write-Host "=== Building opennova-project ==="
    $cliBuildDir = "$ROOT\build"
    if (-not (Test-Path "$cliBuildDir\CMakeCache.txt")) {
        cmake -S $ROOT -B $cliBuildDir -DCMAKE_BUILD_TYPE=Release
        if ($LASTEXITCODE -ne 0) { throw "CMake configure of the root project failed (exit $LASTEXITCODE)" }
    }
    cmake --build $cliBuildDir --config Release --target opennova_project
    if ($LASTEXITCODE -ne 0) { throw "Building opennova-project failed (exit $LASTEXITCODE)" }
    # Ninja puts it in apps/project/, the Visual Studio generator under Release/.
    $PROJECT_CLI = @("$cliBuildDir\apps\project\opennova-project.exe",
                     "$cliBuildDir\apps\project\Release\opennova-project.exe") |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $PROJECT_CLI -or -not (Test-Path -LiteralPath $PROJECT_CLI)) {
    throw "opennova-project not found (pass -ProjectCli, or let the script build it)"
}
Write-Host "    opennova-project: $PROJECT_CLI"

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

# Boot the staged exe headless for a few frames. This is the guard the
# packaging pipeline was missing: the export step only validates export-time
# logs, so a package whose main scene cannot load (e.g. a missing per-product
# run/main_scene feature override in project.godot) still shipped, crashing on
# first launch. Runs against the staged zip layout (exe, GDExtension DLL and
# assets/ side by side). Returns the combined output.
function Invoke-GodotAppBoot {
    param([string]$ExePath, [string]$ExtraArgs, [int]$ExpectedExit)

    $stdoutLog = [System.IO.Path]::GetTempFileName()
    $stderrLog = [System.IO.Path]::GetTempFileName()
    $smokeTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $bootProfile = Join-Path $smokeTempRoot ("opennova-package-smoke-" + [Guid]::NewGuid().ToString("N"))
    $previousAppData = [Environment]::GetEnvironmentVariable("APPDATA", "Process")
    try {
        # This startup smoke must not discover the packager's saved retail folder,
        # credentials, or editor state.
        New-Item -ItemType Directory -Path $bootProfile | Out-Null
        [Environment]::SetEnvironmentVariable("APPDATA", $bootProfile, "Process")
        # Headless uses Godot's Dummy renderer, so a render loop cannot validate
        # GPU work here. Disable it to keep --quit-after from racing render-server
        # teardown while this smoke still loads the product scene, scripts,
        # shaders, and GDExtension.
        $proc = Start-Process `
            -FilePath $ExePath `
            -ArgumentList "--headless --disable-render-loop --disable-crash-handler --quit-after 120 --verbose $ExtraArgs" `
            -WindowStyle Hidden `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $combinedOutput = "$(Get-Content $stdoutLog -Raw)`n$(Get-Content $stderrLog -Raw)"
        if ($proc.ExitCode -ne $ExpectedExit) {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke ($ExtraArgs) exited with code $($proc.ExitCode), expected $ExpectedExit"
        }
        if ($combinedOutput -match "Failed loading scene|Cannot open file 'res://|SCRIPT ERROR|GDExtension dynamic library not found|Failed to load script") {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke ($ExtraArgs) logged load errors"
        }
        return $combinedOutput
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

function Test-GodotAppBoot {
    param([string]$ExePath)

    Write-Host "=== Boot smoke: opennova-runtime ==="
    # No arguments: the bundled game's menu (ADR 0048), from the base game's
    # build in assets/ (its archives mounted, not a loose folder).
    $bundled = Invoke-GodotAppBoot -ExePath $ExePath -ExtraArgs "" -ExpectedExit 0
    if ($bundled -match "bundled assets not found|no menu found in resource dir") {
        Write-Host $bundled.TrimEnd()
        throw "Boot smoke did not reach the bundled game's menu"
    }
    if ($bundled -notmatch "OpenNova: bundled menu up from .* \(build\)") {
        Write-Host $bundled.TrimEnd()
        throw "Boot smoke did not mount the base game's build"
    }
    # --resource-dir without a value stays a usage error.
    $usage = Invoke-GodotAppBoot -ExePath $ExePath -ExtraArgs "-- --resource-dir" -ExpectedExit 2
    if ($usage -notmatch "--resource-dir needs a path") {
        Write-Host $usage.TrimEnd()
        throw "Boot smoke did not report the --resource-dir usage error"
    }
}

# The editor zip's two exes (ADR 0046 d4). The Play child ships no assets/ (Play
# always hands it --resource-dir), so it proves its scene and variant load
# through the same usage error; the editor loads its variant and says so.
function Test-EditorAppBoot {
    param([string]$EditorExe, [string]$PlayExe)

    Write-Host "=== Boot smoke: opennova-play-runtime ==="
    $usage = Invoke-GodotAppBoot -ExePath $PlayExe -ExtraArgs "-- --resource-dir" -ExpectedExit 2
    if ($usage -notmatch "--resource-dir needs a path") {
        Write-Host $usage.TrimEnd()
        throw "Boot smoke did not report the Play runtime's --resource-dir usage error"
    }
    Write-Host "=== Boot smoke: opennova-editor ==="
    $editor = Invoke-GodotAppBoot -ExePath $EditorExe -ExtraArgs "-- --editor-smoke" -ExpectedExit 0
    if ($editor -notmatch "OpenNova Editor: smoke ok") {
        Write-Host $editor.TrimEnd()
        throw "Boot smoke did not report 'OpenNova Editor: smoke ok'"
    }
}

# Stage the TRACKED files of one project folder of the repository (assets/, an
# expansion under expansions/; never a wildcard copy: a working checkout may hold
# untracked local data there that must never ship, and the editor's cache). The
# models, clips and textures ride LFS, so a checkout without them pulled holds
# pointer files, which must never ship either.
function Copy-TrackedProject {
    param([string]$Folder, [string]$StageDir)

    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
    $tracked = & git -C $ROOT ls-files -z -- $Folder
    if ($LASTEXITCODE -ne 0) { throw "git ls-files $Folder failed (exit $LASTEXITCODE)" }
    $names = @($tracked -split "`0" | Where-Object { $_ })
    if ($names.Count -eq 0) { throw "No tracked files found under $Folder/" }
    $pointerHead = "version https://git-lfs.github.com/spec/v1"

    foreach ($name in $names) {
        $src = Join-Path $ROOT ($name -replace "/", "\")
        if (-not (Test-Path -LiteralPath $src)) { throw "Tracked file missing from the working tree: $name" }
        $head = New-Object byte[] $pointerHead.Length
        $stream = [IO.File]::OpenRead($src)
        try { $read = $stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ([Text.Encoding]::ASCII.GetString($head, 0, $read) -eq $pointerHead) {
            throw "Tracked file is an unpulled LFS pointer: $name (run git lfs pull --include=`"$Folder/**`")"
        }
        $rel = $name.Substring($Folder.Length + 1) -replace "/", "\"
        $dst = Join-Path $StageDir $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
    Write-Host "    staged $($names.Count) tracked files of $Folder/"
}

# OpenNova's own game into $GameDir (ADR 0048 d8): the tracked files of assets/
# staged as a project of their own, then the OpenNova Editor's export of it, as
# its File > Export makes one: the build's three boot-table archives and loose
# files (the editor's Build refuses a project the game could not boot) and
# export.json. Each expansion project of the repository (expansions/<folder>/,
# its project.opennova naming assets/ as its base game's project, ADR 0046 T5)
# is staged beside it as the repository lays them out, exported over the base
# game's export, and its expansion\<name>\ folder put into $GameDir, where the
# game mounts it with /exp <name> (OpenNova, and the original game dropped into
# the folder alike). The staged projects, and the caches their builds fill, are
# removed.
function Export-BaseGame {
    param([string]$GameDir)

    Write-Host "=== Building the base game (opennova-project export) ==="
    $stage = New-StageDir ".stage-game-projects"
    try {
        $base = Join-Path $stage "assets"
        Copy-TrackedProject -Folder "assets" -StageDir $base
        # Into the project's own export folder (build\export), where an expansion
        # on it finds its base game; then copied out as what ships.
        & $PROJECT_CLI export $base
        if ($LASTEXITCODE -ne 0) { throw "opennova-project export of the base game failed (exit $LASTEXITCODE)" }
        $baseExport = Join-Path $base "build\export"
        foreach ($archive in @("language.pff", "localres.pff", "resource.pff")) {
            if (-not (Test-Path -LiteralPath (Join-Path $baseExport $archive))) {
                throw "The base game's export holds no $archive"
            }
        }
        New-Item -ItemType Directory -Force -Path $GameDir | Out-Null
        Copy-Item -Path (Join-Path $baseExport "*") -Destination $GameDir -Recurse -Force

        $projects = & git -C $ROOT ls-files -- "expansions/*/project.opennova"
        if ($LASTEXITCODE -ne 0) { throw "git ls-files expansions failed (exit $LASTEXITCODE)" }
        foreach ($projectFile in @($projects | Where-Object { $_ })) {
            $folder = Split-Path $projectFile -Parent
            $folder = $folder -replace "\\", "/"
            Write-Host "=== Building the expansion $folder (opennova-project export) ==="
            $expStage = Join-Path $stage ($folder -replace "/", "\")
            Copy-TrackedProject -Folder $folder -StageDir $expStage
            $out = Join-Path $stage ("out-" + (Split-Path $folder -Leaf))
            & $PROJECT_CLI export $expStage --out $out
            if ($LASTEXITCODE -ne 0) { throw "opennova-project export of $folder failed (exit $LASTEXITCODE)" }
            $built = @(Get-ChildItem -LiteralPath (Join-Path $out "expansion") -Directory)
            if ($built.Count -ne 1) { throw "The export of $folder holds no expansion folder" }
            $name = $built[0].Name
            if (-not (Test-Path -LiteralPath (Join-Path $built[0].FullName "$name.pff"))) {
                throw "The expansion $name's export holds no $name.pff"
            }
            $into = Join-Path $GameDir "expansion"
            New-Item -ItemType Directory -Force -Path $into | Out-Null
            Copy-Item -LiteralPath $built[0].FullName -Destination $into -Recurse -Force
            Write-Host "    shipped expansion\$name"
        }
    }
    finally {
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}

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

Write-Host "=== Exporting opennova.exe ==="
Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE

Write-Host "=== Exporting the editor's Play runtime ==="
Invoke-GodotExport -PresetName "OpenNova Play Runtime" -OutputPath $PLAY_EXE

Write-Host "=== Exporting opennova-editor.exe ==="
Invoke-GodotExport -PresetName "OpenNova Editor" -OutputPath $EDITOR_EXE

# --- the game zip: opennova.exe + its runtime DLL + OpenNova's own game in assets/ ---
$stagePath = New-StageDir ".stage-opennova-game-windows"
try {
    $stagedExe = Join-Path $stagePath "opennova.exe"
    Copy-Item -LiteralPath $RUNTIME_EXE -Destination $stagedExe
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $stagePath
    Copy-ImGuiLibs -ExeDir $DIST -Destination $stagePath -Required ($ExportMode -eq "debug")
    Export-BaseGame -GameDir (Join-Path $stagePath "assets")
    $launchHelp = @"
OpenNova

Run opennova.exe. It plays OpenNova's own small game (assets\), more of which
is coming. To play Joint Operations itself, press PLAY RETAIL and choose your
Joint Operations folder. The folder is remembered; CHANGE FOLDER picks another.

Command line (advanced):
  opennova.exe -- --resource-dir "C:\Games\Joint Operations"
Loose data:
  opennova.exe -- --resource-dir "C:\MyGameData" --loose-root /d

Use /game <code> and /exp <name> to select a game or expansion. OpenNova's own
expansions ship in assets\expansion\; the main menu's MODS lists them, or start
one with, for example:
  opennova.exe -- /exp onjo1
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $launchHelp -Encoding UTF8
    Test-GodotAppBoot -ExePath $stagedExe
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
    $stagedEditorExe = Join-Path $editorDir "opennova-editor.exe"
    $stagedPlayExe = Join-Path $runtimeDir "opennova.exe"
    Copy-Item -LiteralPath $EDITOR_EXE -Destination $stagedEditorExe
    Copy-Item -LiteralPath $SHIPPED_EDITOR_DLL -Destination $editorDir
    Copy-ImGuiLibs -ExeDir (Split-Path $EDITOR_EXE -Parent) -Destination $editorDir -Required $true
    Copy-Item -LiteralPath $PLAY_EXE -Destination $stagedPlayExe
    Copy-Item -LiteralPath $SHIPPED_RUNTIME_DLL -Destination $runtimeDir
    Copy-ImGuiLibs -ExeDir (Split-Path $PLAY_EXE -Parent) -Destination $runtimeDir -Required ($ExportMode -eq "debug")
    $editorHelp = @"
OpenNova Editor (experimental).

  editor\opennova-editor.exe   the editor: create or open a project, fill in the
                               files the game needs, build, play
  runtime\opennova.exe         the game the editor's Play runs your project with

Keep the two folders side by side: Play looks for runtime\opennova.exe next to
the editor folder (or set another runtime in the editor's File > Project settings...).
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $editorHelp -Encoding UTF8
    Test-EditorAppBoot -EditorExe $stagedEditorExe -PlayExe $stagedPlayExe
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $EDITOR_ZIP -Force
    if (-not (Test-Path -LiteralPath $EDITOR_ZIP)) { throw "Packaging produced no editor zip" }
}
finally {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
Write-Host "=== Done ==="
Get-Item -LiteralPath $GAME_ZIP, $EDITOR_ZIP | Select-Object Name, Length
