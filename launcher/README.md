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

## The exe is unsigned

`opennova-launcher.exe` carries no code signature. The first run may show SmartScreen's
"Windows protected your PC" (**More info**, then **Run anyway**), and the UAC prompt
names an unknown publisher. Since it runs as administrator, check the download first: a
tagged release lists `opennova-launcher-windows-<version>.zip.sha256` beside the zip, and
in PowerShell

```powershell
Get-FileHash .\opennova-launcher-windows-<version>.zip -Algorithm SHA256
```

must print the same hash. A PR build's hashes are in its CI log (the package step).

## What it changes

The launcher owns one block in the hosts file the Windows resolver reads
(`%SystemRoot%\System32\drivers\etc\hosts`, or the folder in the Tcpip `DataBasePath`
setting). The first switch adds it at the end of the file; later switches replace it
where it is:

```text
# >>> OpenNova Launcher managed block - do not edit between markers >>>
# mode: opennova
203.0.113.10 gs.novaworld.net
# <<< OpenNova Launcher managed block <<<
```

- The mode line (`opennova` or `custom`) records which choice wrote the block. A block
  with no mode line is one the retired launcher wrote: the menu names it as an older
  launcher's entry, and any switch rewrites it.
- Lines outside the markers keep their exact bytes and line endings, and adding then
  removing the block gives back the file as it was, a missing final newline included.
- The block holds only the launcher's own lines. A line you add between the markers is
  kept, moved after the block, at the next switch. A block whose end marker was deleted
  ends at its first line that is not the launcher's, and everything after it stays. Until
  the next switch the menu shows either as a hand-edited block.
- Each change goes to a temp file, flushed to disk, that then replaces the hosts file in
  one step. The file it replaced is kept as `hosts.opennova-launcher.bak`, and the file
  as it was before the launcher's first change as `hosts.opennova-launcher.orig`, which
  is never overwritten. The DNS cache is flushed after each change.
- If another program rewrites the hosts file while a switch runs (Docker Desktop, say),
  the switch starts over on the new file rather than overwrite it, or stops with a
  message when it was removing lines you agreed to remove.
- A hosts file saved as UTF-16 (Unicode), or holding NUL bytes, is left alone with a
  message: save it as ANSI or UTF-8 in an elevated editor first.

### The hostname

| Name | Why |
| --- | --- |
| `gs.novaworld.net` | The NovaWorld gate (UDP 7597), the one host retail's gate config names. Its reply names the service by address, and the NovaWorld session, the server browser and the login, HOSTKEY and NWJoin web pages follow that address. |

Retail JO:CA's `Jointops.exe` names three NovaWorld hosts in all: `gs.novaworld.net`,
`novaworld.net` (a default the game overwrites with `gs.novaworld.net` before it looks
anything up) and `www.novaworld2.com` (the patch page, outside NovaWorld). The
NovaWorld menus OpenNova's service sends name a fourth, `assets.novaworld.net`, for
their icons and flags; the launcher leaves it alone (the service does not serve those
files), so those icons still come from NovaLogic's host in every mode. The evidence is in
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

**OpenNova service URL...** points the launcher at another OpenNova service. It takes an
https URL, or http only to this computer or a private network address, for example
`http://127.0.0.1:8080` for a local stack (the launcher adds `/api/server-info`); never a
user name or password. A service other than the default shows in the menu. Only a
service on this computer or a private network may answer a loopback, private or
link-local address: from any other, the launcher refuses one.

The settings (the service URL, the saved OpenNova address and the last custom address)
live in the registry under `HKEY_LOCAL_MACHINE\SOFTWARE\OpenNova\Launcher`, which only
administrators can change, since they decide what an elevated program writes.

## Administrator rights

Changing the hosts file needs administrator rights, and that change is the launcher's
whole job, so it asks for them when it starts (a UAC prompt) and never fails halfway
through a switch. One launcher runs per computer: starting a second one, from any
signed-in user, says the first is already running.

## Windows Defender

Windows Defender flags some hosts-file edits as `SettingsModifier:Win32/HostsFileHijack`,
because malware redirects security sites that way. The launcher's block maps only the
NovaWorld gate name, and you can read the hosts file at any time. If Defender reverts
the change, allow it from Defender's protection history and switch again.

## Checking it by hand

What CI cannot run, with a stock JO:CA install and two machines (or two installs):

1. Start the launcher, pick **OpenNova NovaWorld**, and read the hosts file: the block
   names the address `https://opennova.net/api/server-info` gives.
2. Start `Jointops.exe`, open NovaWorld, and log in with an OpenNova account.
3. Host a game on NovaWorld; it shows in the site's server list.
4. On the second client (also switched to OpenNova), find the game in the browser and
   join it.
5. Pick **NovaLogic NovaWorld**: the block is gone, the hosts file matches
   `hosts.opennova-launcher.orig` when the launcher had made no other change, and the
   game's NovaWorld login reaches NovaLogic's service again.

## Building

Needs the .NET 8 SDK on Windows. From the repository root:

```bash
dotnet test launcher/OpenNovaLauncher.sln -c Release
dotnet publish launcher/src/OpenNovaLauncher/OpenNovaLauncher.csproj -c Release -r win-x64 -o build/launcher
```

The publish step writes the self-contained single-file `opennova-launcher.exe` into
`build/launcher/`, with no native library to extract at run time. CI packages it with
this README as `opennova-launcher-windows.zip`
(`.github/workflows/package-launcher-windows.yml`); CI's required `test` check runs the
suite too.
