# OpenNova Launcher

A small Windows tray app that switches a stock Joint Operations install between
OpenNova's NovaWorld and NovaLogic's own, by managing one block in the Windows hosts
file. Your game files stay stock: no patched executable, no injected DLL, no config
edit in the game folder. It is pre-1.0 and experimental.

It covers JO and newer titles: retail Joint Operations (Combined Arms included) looks
up NovaWorld by the name below, and Delta Force Xtreme 2's client names the same one.

## Using it

1. Unzip `opennova-launcher-windows.zip` anywhere and run `opennova-launcher.exe`.
   Windows asks for administrator rights (see below).
2. Right-click the OpenNova icon in the notification area. The first line says where
   NovaWorld points now, read from the hosts file each time the menu opens.
3. Pick one:
   - **OpenNova NovaWorld**: retail reaches OpenNova's NovaWorld service. The launcher
     asks the service for its address (below) and writes it.
   - **NovaLogic NovaWorld**: the launcher removes its block, so retail reaches
     NovaLogic's real NovaWorld.
   - **Custom address...**: an IPv4 address you type, such as `127.0.0.1` for a
     NovaWorld service on this computer or a LAN address for one on another.
4. Start `Jointops.exe` as usual. The choice applies the next time the game connects
   to NovaWorld.

The choice stays in the hosts file after you exit the launcher and across restarts.
To put the hosts file back to stock, pick **NovaLogic NovaWorld** before you delete the
launcher.

## What it changes

The launcher owns one block at the end of `%SystemRoot%\System32\drivers\etc\hosts`:

```text
# >>> OpenNova Launcher managed block - do not edit between markers >>>
# mode: opennova
203.0.113.10 gs.novaworld.net
# <<< OpenNova Launcher managed block <<<
```

The mode line (`opennova` or `custom`) records which choice wrote the block. Every
line outside the markers is kept byte for byte. Each change is written to a temp file
that then replaces the hosts file in one step, and the file it replaced is kept beside
it as `hosts.opennova-launcher.bak`. The DNS cache is flushed after each change.

### The hostname

| Name | Why |
| --- | --- |
| `gs.novaworld.net` | The NovaWorld gate (UDP 7597), the one name retail looks up for NovaWorld. Its reply names the service by address: the NovaWorld session, the server browser and the login, HOSTKEY and NWJoin web pages all follow that address, so no other name needs mapping. |

Names retail carries that the launcher leaves alone: `novaworld.net` (a default the game
overwrites with `gs.novaworld.net` before it looks anything up), `www.novaworld2.com`
(the patch page, outside NovaWorld) and `assets.novaworld.net` (the icon host the
NovaWorld menus name, which OpenNova's service does not serve). The evidence is in
`docs/net/novaworld-net-re.md` (NW-L1 and NW-L2) and ADR 0054.

### Entries the launcher did not write

If the hosts file already maps `gs.novaworld.net` outside the block (an entry you or
another tool added), the menu says so. The launcher never removes such a line without
asking: a switch shows the lines and asks to remove them first, since Windows uses the
first entry it finds and the switch would not take effect reliably otherwise. Say no and
nothing is written. **Other ... entries in the hosts file: remove them...** in the menu
removes them on their own, after the same question.

## OpenNova's address

OpenNova's site sits behind a proxy, so looking up its domain does not give the game
server's address. The launcher asks the service instead: `GET
https://opennova.net/api/server-info` answers `{"novaworld_ip": "<address>"}`. The last
address it gave is saved, so the launcher can still switch while that request fails;
it says so when it used the saved one. Each time the launcher starts with the hosts file
on OpenNova, it asks again and rewrites the block if the address moved.

**OpenNova service URL...** points the launcher at another OpenNova service, for example
`http://127.0.0.1:8080` for a local one (the launcher adds `/api/server-info`). Settings
live in `%APPDATA%\OpenNova Launcher\settings.json`: the service URL, the saved OpenNova
address and the last custom address.

## Administrator rights

Changing the hosts file needs administrator rights, and that change is the launcher's
whole job, so it asks for them when it starts (a UAC prompt) and never fails halfway
through a switch. Only one launcher runs at a time.

## Windows Defender

Windows Defender flags some hosts-file edits as `SettingsModifier:Win32/HostsFileHijack`,
because malware redirects security sites that way. The launcher's block maps only the
NovaWorld gate name, and you can read the hosts file at any time. If Defender reverts
the change, allow it from Defender's protection history and switch again.

## Building

Needs the .NET 8 SDK on Windows. From the repository root:

```bash
dotnet test launcher/OpenNovaLauncher.sln -c Release
dotnet publish launcher/src/OpenNovaLauncher/OpenNovaLauncher.csproj -c Release -r win-x64 -o build/launcher
```

The publish step writes the self-contained single-file `opennova-launcher.exe` into
`build/launcher/`. CI packages it with this README as `opennova-launcher-windows.zip`
(`.github/workflows/package-launcher-windows.yml`).
