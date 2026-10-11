# Build and export the Windows runtime. Produces
# dist/opennova-game-windows-v<version>.zip with opennova.exe, its matching
# native dependencies and the bundled assets/ placeholder game (ADR 0048).
# Retail game data is supplied by the player. Debug exports include the game's
# ImGui tools.
# Usage: pwsh -File scripts/package_godot_windows.ps1 [-ExportMode release|debug] [-SkipBuild]

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

# Install the game dev-tools bridge; the release export strips it.
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

# Stamp version into the runtime export preset so Godot embeds it in the .exe VERSIONINFO
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
    # the DLL source/debug game runs use rather than an /Od build.
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
# assets/ side by side), with a throwaway directory for everything it writes,
# so nothing lands in the stage. $UserArgs are the game's own arguments (after
# Godot's `--`). Returns the combined output.
function Invoke-GodotAppBoot {
    param([string]$ExePath, [string]$UserArgs, [int]$ExpectedExit)

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
        # The game keeps its saves in its working directory, as retail does
        # (player.sav and weapon.sav, written at the first menu start;
        # LaunchFlags.working_dir). Godot's export templates move the process to
        # the exe's folder before the game runs (Main::setup), whatever folder it
        # was started in, so the smoke of the staged exe saved into the stage and
        # the 0.0.16 and 0.0.17 zips shipped the packager's profile, its Windows
        # login the callsign. --working-dir names the throwaway profile instead,
        # as the editor's Play names its run directory, and the process starts
        # there too. user:// sits under APPDATA, above. The bundled assets/ still
        # resolve from the exe's folder (BootRootMount.bundled_assets_dir), so the
        # staged layout is what boots.
        $proc = Start-Process `
            -FilePath $ExePath `
            -ArgumentList "--headless --disable-render-loop --disable-crash-handler --quit-after 120 --verbose -- --working-dir `"$bootProfile`" $UserArgs" `
            -WorkingDirectory $bootProfile `
            -WindowStyle Hidden `
            -Wait `
            -PassThru `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog

        $combinedOutput = "$(Get-Content $stdoutLog -Raw)`n$(Get-Content $stderrLog -Raw)"
        if ($proc.ExitCode -ne $ExpectedExit) {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke ($UserArgs) exited with code $($proc.ExitCode), expected $ExpectedExit"
        }
        if ($combinedOutput -match "Failed loading scene|Cannot open file 'res://|SCRIPT ERROR|GDExtension dynamic library not found|Failed to load script") {
            Write-Host $combinedOutput.TrimEnd()
            throw "Boot smoke ($UserArgs) logged load errors"
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
    # No arguments: the bundled assets/ placeholder menu (ADR 0048).
    $bundled = Invoke-GodotAppBoot -ExePath $ExePath -UserArgs "" -ExpectedExit 0
    if ($bundled -match "bundled assets not found|no menu found in resource dir") {
        Write-Host $bundled.TrimEnd()
        throw "Boot smoke did not reach the bundled placeholder menu"
    }
    # --resource-dir without a value stays a usage error.
    $usage = Invoke-GodotAppBoot -ExePath $ExePath -UserArgs "--resource-dir" -ExpectedExit 2
    if ($usage -notmatch "--resource-dir needs a path") {
        Write-Host $usage.TrimEnd()
        throw "Boot smoke did not report the --resource-dir usage error"
    }
}

# Stage the TRACKED files of assets/ (never a wildcard copy: a working checkout
# may hold untracked local data there that must never ship). The models, clips
# and textures ride LFS, so a checkout without them pulled holds pointer files,
# which must never ship either. Returns each staged file's path under
# $AssetsStageDir.
function Copy-BundledAssets {
    param([string]$AssetsStageDir)

    $copied = New-Object System.Collections.Generic.List[string]
    New-Item -ItemType Directory -Force -Path $AssetsStageDir | Out-Null
    $tracked = & git -C $ROOT ls-files -z assets
    if ($LASTEXITCODE -ne 0) { throw "git ls-files assets failed (exit $LASTEXITCODE)" }
    $names = @($tracked -split "`0" | Where-Object { $_ })
    if ($names.Count -eq 0) { throw "No tracked files found under assets/" }
    $pointerHead = "version https://git-lfs.github.com/spec/v1"

    foreach ($name in $names) {
        $src = Join-Path $ROOT ($name -replace "/", "\")
        if (-not (Test-Path -LiteralPath $src)) { throw "Tracked asset missing from the working tree: $name" }
        $head = New-Object byte[] $pointerHead.Length
        $stream = [IO.File]::OpenRead($src)
        try { $read = $stream.Read($head, 0, $head.Length) } finally { $stream.Dispose() }
        if ([Text.Encoding]::ASCII.GetString($head, 0, $read) -eq $pointerHead) {
            throw "Tracked asset is an unpulled LFS pointer: $name (run git lfs pull --include=`"assets/**`")"
        }
        $rel = $name -replace "^assets/", "" -replace "/", "\"
        $dst = Join-Path $AssetsStageDir $rel
        New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $src -Destination $dst -Force
        $copied.Add($rel)
    }
    Write-Host "    staged $($names.Count) tracked asset files"
    return $copied
}

# The zip ships exactly the files this script staged. Anything else in the
# stage fails the package, named: a save or log the boot smoke wrote (the
# 0.0.16 and 0.0.17 zips shipped the CI runner's player.sav and weapon.sav,
# which also overwrite a player's own on unzipping over an install) or a file
# any other step left there. $Staged holds paths relative to $StageDir.
function Assert-StageHoldsOnly {
    param([string]$StageDir, [string[]]$Staged)

    $prefix = [IO.Path]::GetFullPath($StageDir).TrimEnd('\') + '\'
    $files = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $dirs = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($rel in $Staged) {
        $rel = $rel -replace '/', '\'
        [void]$files.Add($rel)
        for ($dir = Split-Path $rel -Parent; $dir; $dir = Split-Path $dir -Parent) { [void]$dirs.Add($dir) }
    }
    $unexpected = @(Get-ChildItem -LiteralPath $StageDir -Recurse -Force | ForEach-Object {
        $rel = $_.FullName.Substring($prefix.Length)
        if ($_.PSIsContainer) {
            if (-not $dirs.Contains($rel)) { "$rel\" }
        }
        elseif (-not $files.Contains($rel)) { $rel }
    })
    if ($unexpected.Count -gt 0) {
        throw "The game stage holds files this script did not stage, which the zip would ship: $($unexpected -join ', ')"
    }
    $missing = @($files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $StageDir $_) -PathType Leaf) })
    if ($missing.Count -gt 0) {
        throw "Files this script staged are gone from the game stage: $($missing -join ', ')"
    }
    Write-Host "    the stage holds exactly the $($files.Count) files staged"
}

Write-Host "=== Exporting opennova.exe ==="
Invoke-GodotExport -PresetName "OpenNova Runtime" -OutputPath $RUNTIME_EXE

# Stage this export's runtime dependencies plus the bundled assets/.
$gameStage = Join-Path $DIST ".stage-opennova-game-windows"
$stagePath = [IO.Path]::GetFullPath($gameStage)
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
try {
    # Each file staged, as its path in the stage: the only files the zip may
    # hold (Assert-StageHoldsOnly).
    $staged = New-Object System.Collections.Generic.List[string]
    $stagedExe = Join-Path $stagePath "opennova.exe"
    Copy-Item -LiteralPath $RUNTIME_EXE -Destination $stagedExe
    $staged.Add("opennova.exe")
    Copy-Item -LiteralPath $SHIPPED_DLL -Destination $stagePath
    $staged.Add((Split-Path $SHIPPED_DLL -Leaf))
    if ($ExportMode -eq "debug") {
        $imguiLibs = @(Get-ChildItem -LiteralPath $DIST -Filter "libimgui-godot-native.*" -File)
        if ($imguiLibs.Count -eq 0) { throw "Debug export is missing the ImGui addon library" }
        foreach ($lib in $imguiLibs) {
            Copy-Item -LiteralPath $lib.FullName -Destination $stagePath
            $staged.Add($lib.Name)
        }
    }
    foreach ($rel in @(Copy-BundledAssets -AssetsStageDir (Join-Path $stagePath "assets"))) {
        $staged.Add("assets\$rel")
    }
    $launchHelp = @"
OpenNova

Run opennova.exe. OpenNova's own game is coming soon; until then, press
PLAY RETAIL and choose your Joint Operations folder to play the retail game.
The folder is remembered; CHANGE FOLDER picks another.

Command line (advanced):
  opennova.exe -- --resource-dir "C:\Games\Joint Operations"
Loose data:
  opennova.exe -- --resource-dir "C:\MyGameData" --loose-root /d

Use /game <code> and /exp <name> to select a game or expansion.
"@
    Set-Content -LiteralPath (Join-Path $stagePath "README.txt") -Value $launchHelp -Encoding UTF8
    $staged.Add("README.txt")
    Test-GodotAppBoot -ExePath $stagedExe
    Assert-StageHoldsOnly -StageDir $stagePath -Staged $staged
    Compress-Archive -Path (Join-Path $stagePath "*") -DestinationPath $GAME_ZIP -Force
    if (-not (Test-Path -LiteralPath $GAME_ZIP)) { throw "Packaging produced no zip" }
}
finally {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
Write-Host "=== Done ==="
Get-Item -LiteralPath $GAME_ZIP | Select-Object Name, Length
