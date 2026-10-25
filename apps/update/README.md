# OpenNova Update

`update.exe` is the online updater shipped beside `opennova.exe`; `revupdat.exe`
replaces `update.new` after the running updater exits and relaunches it.

```text
update.exe --check /game acidgambit --runtime-dir <runtime> --resource-dir <content>
update.exe /game jo --runtime-dir <runtime> --resource-dir <retail> /RUN
```

`--check` has no UI, changes no files, and exits 0 when current, 10 when an update
is available, 1 on failure and 2 for invalid arguments. Its network work has a ten
second total budget; OS hostname resolution can also take time. The game keeps
playing installed content when a background check fails. Normal execution shows
a native progress dialog on Windows. `/RUN` starts `opennova.exe` after success;
repeat `--run-arg <value>` to preserve its arguments. `--parent-pid <pid>` waits for
the game to exit before files are changed. `--server` and `--port` select a local
test server; `--no-runtime` checks/applies only the selected content products.

The runtime product defaults to `ONRUNTIME` / `ONRUNTIME.CMD`, configured by
`OPENNOVA.WIZ`. OpenNova content is `ACIDGAMBIT` / `ACIDGAMBIT.CMD`. JO/DFX2 content
uses the existing `UPDATE.WIZ`, including its territory substitution. OpenNova's
runtime config never replaces the retail wizard or its `version.txt`. A native
expansion's `expansion/<name>/UPDATE.WIZ` registers it for first installation;
retail expansion updates additionally require the installed `<name>.pff`.

Every product updates through the gateways the runtime's `OPENNOVA.WIZ` names (its
`URL`, `URL2` and `URL3`); a product's own wizard URLs are never dialed. The game zip
writes that wizard by its export mode: `update.opennova.net`, production's, in a release
package, and `update.staging.opennova.net`, staging's, in a debug one (`-ExportMode
debug`, what PR CI builds), so one updater build serves both. With no `OPENNOVA.WIZ`, or
one naming no URL, the gateway is production's, `update.opennova.net`.

The existing retail protocol, command files, CRCs, NLZ payloads and PFF writer are
shared with the service.
Downloads for one product are validated before installation; file failures can
leave a partial installation, and the next run checks and retries it. There is
no transaction journal or rollback. Updating the updater itself uses its helper;
other files are installed in one pass once the game has exited.

Legacy positional window handles, `/DIR`, `/RUN`, `/VERBOSE` (an `update.log`),
and stock wizards are accepted. This milestone implements online updates;
appended executable SFS/offline `/SFU` execution and retail's server load-score
selection are not implemented. Advertised servers are tried as failover peers.

Native release packages must use `opennova-update-tool build --full`, with
`--previous <last-package.sfs>` after the first release. This includes reverted
files and carries removals across skipped releases. Keep the preceding published
package available to the release pipeline. Native packages retain the format's
flat product files and 15-character payload names. The native client downloads
all rows when the command file changes, avoiding the retail prefix-CRC precheck
for a newly published release; subsequent repair checks retain the retail CRC
and archive timestamp behavior.

Build the runtime package with `--base ONRUNTIME.CMD --product "OpenNova Runtime" --version <v>`:
every package's PRODUCT and VERSION are the release's, and no version file is read or written (ADR 0067).
