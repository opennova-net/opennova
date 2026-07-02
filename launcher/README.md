# OpenNova Launcher

A small Windows tray app that points your game at OpenNova's NovaWorld servers. It adds one managed entry to the Windows hosts file so the game's built-in NovaWorld hostnames resolve to OpenNova instead. Your game files stay completely stock: no patched executables, no injected DLLs, no config edits inside the game directory.

OpenNova Launcher supports JO and newer titles (Joint Operations: Typhoon Rising today; Delta Force Xtreme 2 is listed but its redirect hostname is pending verification). The launcher is pre-1.0 and under active development.

## How it works

1. You register your game install directories in Preferences.
2. You turn on **Redirect to OpenNova** in the tray menu.
3. The launcher resolves the current OpenNova server address (backend `/server-info`, falling back to a cached address, then a DNS lookup of `nw.opennova.net`) and writes a managed block into `%SystemRoot%\System32\drivers\etc\hosts`:

   ```
   # >>> OpenNova Launcher managed block - do not edit between markers >>>
   203.0.113.10 gs.novaworld.net
   # <<< OpenNova Launcher managed block <<<
   ```

4. Launching a game starts the stock executable with stock flags (`/w` windowed, `/many` multiple instances, `/exp <slug>` expansion), plus any per-game advanced arguments from settings.

Everything outside the markers is preserved byte for byte. The launcher refreshes the address before every launch and rewrites the block only when it is stale. The DNS cache is flushed after each change so it takes effect immediately.

The hosts entries stay in place while the toggle is on, including across launcher restarts. They are removed when you turn **Redirect to OpenNova** off in the tray menu.

## Launching

Launch is always available from the tray. When you launch, a dialog lets you pick **OpenNova** (the default: the launcher refreshes the server address and writes the hosts redirect first) or the **original NovaWorld** (the launcher clears its managed hosts block so the game reaches NovaLogic's servers for that session; it is re-applied the next time you launch OpenNova). The right-click menu also has a **Redirect to OpenNova** toggle to turn the launcher's hosts management on or off without opening Preferences.

If a conflicting entry from another tool is found, the launcher asks before removing it, and removes only the lines that mention the NovaWorld hostnames.

## Administrator rights

The launcher requires administrator rights (UAC prompt at start). Editing the hosts file is a protected operation on Windows, and that edit is the launcher's whole job. The manifest requests elevation up front so the launcher never fails halfway through a change.

## Windows Defender note

Windows Defender flags some hosts-file edits as `SettingsModifier:Win32/HostsFileHijack`. That detection exists because malware sometimes redirects security sites. The launcher's managed block only touches the NovaWorld game hostnames, and you can inspect the hosts file at any time. If Defender reverts the entry, add an allowance for it or restore the change from Defender's protection history.

## Developer mode

**Developer mode (127.0.0.1)** in the tray menu points the NovaWorld hostnames at your local machine instead of the OpenNova servers, for testing a locally hosted server. It only changes the hosts redirect target; the launcher still talks to the production backend API for catalogs and updates (override with `ONLAUNCHER_API_BASE_URL` if needed).

## Settings

Settings live in `%APPDATA%\OpenNovaLauncher\settings.json`. On first run, settings from the older OnLauncher (`%APPDATA%\OnLauncher\settings.json`) are copied over automatically.

Useful environment overrides:

| Variable | Effect |
| --- | --- |
| `ONLAUNCHER_API_BASE_URL` | Backend API base (default `https://opennova.net/api`) |
| `ONLAUNCHER_NW_ANCHOR_HOST` | DNS fallback hostname (default `nw.opennova.net`) |
| `ONLAUNCHER_UPDATE_MANIFEST_URL` | Launcher update manifest URL |

## Building

Requires the .NET 8 SDK or newer on Windows.

```
dotnet build launcher/OpenNovaLauncher.sln -c Release
dotnet test launcher/OpenNovaLauncher.sln -c Release
dotnet publish launcher/src/OpenNovaLauncher/OpenNovaLauncher.csproj -c Release -r win-x64
```

The publish step produces a single self-contained `OpenNovaLauncher.exe` under `src/OpenNovaLauncher/bin/Release/net8.0-windows/win-x64/publish/`.
