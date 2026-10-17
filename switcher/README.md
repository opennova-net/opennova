# OpenNova Switcher

A small Windows tray app that switches a stock Joint Operations install between
OpenNova's NovaWorld and NovaLogic's own, by managing one block in the Windows hosts
file. Your game files stay stock: no patched executable, no injected DLL, no config
edit in the game folder. It is pre-1.0 and experimental.

It is for the original executables alone: `opennova.exe` reaches OpenNova's NovaWorld by
itself (its NovaWorld screen's OpenNova target dials `nw.opennova.net`), so it needs no
Switcher.

It covers JO and newer titles: retail Joint Operations (Combined Arms included) looks
up NovaWorld by the gate name below, and Delta Force Xtreme 2's client names the same
one; their updater, `UPDATE.EXE`, looks up the three gateway names beside it, so a
switched install updates from OpenNova too.

## Using it

1. Unzip `opennova-switcher-windows.zip` anywhere and run `opennova-switcher.exe`.
   Windows asks for administrator rights (see below).
2. Right-click the OpenNova icon in the notification area. The first line says where
   NovaWorld points now, read from the hosts file each time the menu opens.
3. Pick one:
   - **OpenNova NovaWorld**: retail reaches OpenNova's NovaWorld service. The switcher
     asks the service for its address (below) and writes it.
   - **NovaLogic NovaWorld**: the switcher removes its block, so retail reaches
     NovaLogic's real NovaWorld.
   - **Custom address...**: an IPv4 address you type, such as `127.0.0.1` for a
     NovaWorld service on this computer or a LAN address for one on another.
4. Start `Jointops.exe` as usual. The choice applies the next time the game connects
   to NovaWorld.

The choice stays in the hosts file after you exit the switcher and across restarts.
To put the hosts file back to stock, pick **NovaLogic NovaWorld** before you delete the
switcher.

## The exe is unsigned

`opennova-switcher.exe` carries no code signature. The first run may show SmartScreen's
"Windows protected your PC" (**More info**, then **Run anyway**), and the UAC prompt
names an unknown publisher. Since it runs as administrator, check the download first: a
tagged release lists `opennova-switcher-windows-<version>.zip.sha256` beside the zip, and
in PowerShell

```powershell
Get-FileHash .\opennova-switcher-windows-<version>.zip -Algorithm SHA256
```

must print the same hash. A PR build's hashes are in its CI log (the package step).

## What it changes

The switcher owns one block in the Windows hosts file,
`C:\Windows\System32\drivers\etc\hosts` (the System32 folder Windows reports, whatever
`%SystemRoot%` says; a hosts file moved elsewhere through the Tcpip `DataBasePath` setting
is not supported). The first switch adds the block at the end of the file; later switches
replace it where it is:

```text
# >>> OpenNova Switcher managed block - do not edit between markers >>>
# mode: opennova
203.0.113.10 gs.novaworld.net
203.0.113.10 ftp.novaworld.com
203.0.113.10 ftp2.novaworld.com
203.0.113.10 ftp4.novaworld.com
# <<< OpenNova Switcher managed block <<<
```

- The mode line (`opennova` or `custom`) records which choice wrote the block.
- Lines outside the markers keep their exact bytes and line endings, and adding then
  removing the block gives back the file as it was, a missing final newline included.
- The block holds only the switcher's own lines: one per name below, all to one address.
  A line you add between the markers is kept, moved after the block, at the next switch. A
  block whose end marker was deleted ends at its first line that is not the switcher's,
  its first blank line, or a second line for a name it already holds, and everything
  after it stays (such a second line is then an entry the switcher did not write, below).
  Until the next switch the menu shows either as a hand-edited block, and so does a block
  an earlier switcher wrote with the gate name alone: the next switch rewrites it with
  every name.
- Each change goes to a temp file, flushed to disk, that then replaces the hosts file in
  one step. The file it replaced is kept as `hosts.opennova-switcher.bak`, and the file
  as it was before the switcher's first change as `hosts.opennova-switcher.orig`, which
  is never overwritten. The DNS cache is flushed after each change.
- If another program rewrites the hosts file while a switch runs (Docker Desktop, say),
  the switch starts over on the new file rather than overwrite it, or stops with a
  message when it was removing lines you agreed to remove.
- A hosts file saved as UTF-16 (Unicode), or holding NUL bytes, is left alone with a
  message: save it as ANSI or UTF-8 in an elevated editor first.

### The hostnames

| Name | Why |
| --- | --- |
| `gs.novaworld.net` | The NovaWorld gate (UDP 7597), the one host retail's gate config names. Its reply names the service by address, and the NovaWorld session, the server browser and the login, HOSTKEY and NWJoin web pages follow that address. |
| `ftp.novaworld.com`, `ftp2.novaworld.com`, `ftp4.novaworld.com` | The updater's gateways: retail `UPDATE.EXE` dials them on its fixed TCP ports (20044, 20556, 21068, 21580), hardcoded in it. OpenNova's NovaWorld serves the updater at the gate's address, so a switched install takes its updates from OpenNova; with the block gone, from NovaLogic. |

Retail JO:CA's `Jointops.exe` names three NovaWorld hosts in all: `gs.novaworld.net`,
`novaworld.net` (a default the game overwrites with `gs.novaworld.net` before it looks
anything up) and `www.novaworld2.com` (the patch page, outside NovaWorld). The
NovaWorld menus OpenNova's service sends name a fourth, `assets.novaworld.net`, for
their icons and flags; the switcher leaves it alone (the service does not serve those
files), so those icons still come from NovaLogic's host in every mode. The evidence is in
`docs/net/novaworld-net-re.md` (NW-L1 and NW-L2), `docs/update/updater-re.md` (section 3,
the updater's gateways and ports), ADR 0054 and ADR 0061.

### Entries the switcher did not write

If the hosts file already maps one of the names outside the block (an entry you or
another tool added), the menu says so. The switcher never removes such a line without
asking: a switch shows the lines and asks to remove them first, since Windows uses the
first entry it finds and the switch would not take effect reliably otherwise. Say no and
nothing is written. **Other ... entries in the hosts file: remove them...** in the menu
removes them on their own, after the same question.

## OpenNova's address

OpenNova's site sits behind a proxy, so looking up its domain does not give the game
server's address. The switcher asks the service instead: `GET
https://opennova.net/api/server-info` answers `{"novaworld_ip": "<address>"}`. The last
address it gave is saved, so the switcher can still switch while that request fails;
it says so when it used the saved one. Each time the switcher starts with the hosts file
on OpenNova, it asks again and rewrites the block if the address moved.

**OpenNova service URL...** points the switcher at another OpenNova service. It takes an
https URL, or http only to this computer or a private network address, for example
`http://127.0.0.1:8080` for a local stack (the switcher adds `/api/server-info`); never a
user name or password. A service other than the default shows in the menu. Only a
service on this computer or a private network may answer a loopback, private or
link-local address: from any other, the switcher refuses one.

The settings (the service URL, the saved OpenNova address and the last custom address)
live in the registry under `HKEY_LOCAL_MACHINE\SOFTWARE\OpenNova\Switcher`, which only
administrators can change, since they decide what an elevated program writes. Each save
gives the key its own permissions (owned by Administrators; SYSTEM and Administrators full
control, Users read; nothing inherited) rather than relying on what it inherits.

## Administrator rights

Changing the hosts file needs administrator rights, and that change is the switcher's
whole job, so it asks for them when it starts (a UAC prompt) and never fails halfway
through a switch. It will not run without them: a start that skipped the prompt (a
compatibility setting, or UAC turned off for a standard account) says so and exits
before it does anything else. One switcher runs per computer: starting a second one,
from any signed-in user, says the first is already running.

## Windows Defender

Windows Defender flags some hosts-file edits as `SettingsModifier:Win32/HostsFileHijack`,
because malware redirects security sites that way. The switcher's block maps only the
NovaWorld gate name and the updater's three gateway names, and you can read the hosts
file at any time. If Defender reverts the change, allow it from Defender's protection
history and switch again.

## Checking it by hand

What CI cannot run, with a stock JO:CA install and two machines (or two installs):

1. Start the switcher, pick **OpenNova NovaWorld**, and read the hosts file: the block
   maps the four names to the address `https://opennova.net/api/server-info` gives.
2. Start `Jointops.exe`, open NovaWorld, and log in with an OpenNova account.
3. Host a game on NovaWorld; it shows in the site's server list.
4. On the second client (also switched to OpenNova), find the game in the browser and
   join it.
5. With a data set's release published on OpenNova's Game data page, run the install's
   `UPDATE.EXE` (or the game's update check): it reaches OpenNova's update service through
   the block and installs the release.
6. Pick **NovaLogic NovaWorld**: the block is gone, the hosts file matches
   `hosts.opennova-switcher.orig` when the switcher had made no other change, and the
   game's NovaWorld login reaches NovaLogic's service again.

## Building

Needs the .NET 8 SDK on Windows. From the repository root:

```bash
dotnet test switcher/OpenNovaSwitcher.sln -c Release
dotnet publish switcher/src/OpenNovaSwitcher/OpenNovaSwitcher.csproj -c Release -r win-x64 -o build/switcher
```

The publish step writes the self-contained single-file `opennova-switcher.exe` into
`build/switcher/`, with no native library to extract at run time. CI packages it with
this README as `opennova-switcher-windows.zip`
(`.github/workflows/package-switcher-windows.yml`); CI's required `test` check runs the
suite too.
