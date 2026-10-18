# Hosted servers

An account with a hosting entitlement can have the service run as many `opennova-serve`
servers for it as the entitlement allows, each in a slot of one of the fleet's machines
(a shared machine, or an own machine of its own), until it stops them (ADR 0053; this
directory; the `hosted_servers`, `hosted_server_events` and `fleet_machines` tables). Each
server runs the data set its owner picks (and an expansion of it), pinned to the data it
started on (ADR 0064). Everything here is service policy. The service around it (the
programs, the jobs, the database) is [`../README.md`](../README.md). The fleet program's
own sources (the supervisor, the stats sampler, the admin links, the console drain, the
internal listener and the agents' routes) live in
[`../../novaworld_fleet/hosted/`](../../novaworld_fleet/README.md); the rest named here is
this directory's.

## The entitlement and the data sets

- **The hosting entitlement** (`billing/entitlement.*`): how many hosted servers an
  account may have live, the larger of its support tier's servers (a monthly Stripe
  subscription: tier 1, 2 or 3 grants 1, 2 or 3; `billing/tiers.h`) and an admin's comp
  grant of servers (`comp_grants`, 0..16). Only an active player's account has one. It is
  read from the billing tables (the cache of Stripe the billing sync keeps), whether or
  not billing is configured, so a deploy that breaks billing never stops a payer's
  servers. A subscription grants while Stripe calls it `active`, `trialing` or `past_due`,
  until 48 h past its period's end; `past_due` keeps the account's servers running but
  starts no new one. The own-machine add-on (one subscription of quantity 1 per machine)
  lets that many of the account's servers run on a machine of their own
  (`hosted_servers.own_machine`), only while a tier also grants; an admin's comp grant of
  own machines (`comp_grants`, 0..16) adds to them while the account has servers (a tier's
  or a grant's), so an admin, the local stack and CI give an own machine without Stripe.
  What every account's entitlement holds is what the service has sold, and no checkout
  sells past the fleet (`sold_capacity.*`; [`../README.md`](../README.md) "Billing").
  A start is still refused `at_capacity` when the fleet has no room for it now.
- **Data sets** (`data_set_repository.*`, `data_set_files.*`,
  `backend/migrations/0011_data_sets.sql`). A data set is a row an admin makes: a **base**
  game (its `title`, the `gameprofile` code `opennova-serve` speaks with `/game`, `jo` or
  `dfx2`, and its `game`, the `games` row whose lobby it lists in) or an **expansion** of a
  base (its `expansion_code`, `/exp jox01`). Its data lives in the object store
  (`ONNET_OBJECT_STORE_URL`) as **trees**, each one immutable version of the data set's
  files (`data_set_files.h` names them) at `trees/<id>/`, uploaded by an admin or built by
  applying a published update release, with the multiplayer missions its archives list. A
  base is **offered** while it has a ready current tree, with each of its expansions that
  has one; the host-screen limits are the title's (`host_limits.h`). Hosting is on only
  with a store. At its placement a server is **pinned** to its data sets' current trees
  (`base_tree_id`, `expansion_tree_id`), kept through a crash restart or a move, until its
  owner asks for the update (`update_requested`: placed again on the current trees, no
  restart counted). One whose data set has no data at its placement fails there.

## The fleet

- **Machines and slots** (`placement.*`, `fleet_repository.*`,
  `backend/migrations/0003_fleet.sql`). A **shared machine** packs up to
  `ONNET_FLEET_SHARED_SLOTS` servers of any account; an **own machine** runs exactly one,
  the add-on's, launched for it and ended when it stops. A **slot** is one server's
  position on a machine (`slots.h`): slot k is the game's UDP port 32768 + k (open to
  everyone), the remote admin's TCP port 32800 + k (open to the service alone), the
  container's user 10001 + k and the working directory `/srv/slots/slot-k`. A
  `fleet_machines` row moves by compare-and-set: `launching` → `booting` (the cloud runs
  it) → `ready` (its agent reports) → `draining` → `ending` → `ended`, or `failed`, and
  `lost` once its agent has been silent 60 s. Two live servers never hold one slot (a
  unique index). An ended or failed machine's row is deleted 14 days after its end by
  `stats.prune`.
- **Placement** (`placement.h`, pure functions): a shared server goes to the fullest
  shared machine that takes servers with a free slot, its lowest free slot, so machines
  fill one at a time and an emptied one can end; with none, a shared machine is launched
  while fewer than `ONNET_FLEET_MAX_SHARED` take servers, else the server waits in
  `provisioning` for a slot. A server on the add-on goes to the machine reserved for it,
  else to a new own machine (at most `ONNET_FLEET_MAX_OWN`). A start the planner could not
  place, counting the servers already waiting, is `at_capacity`.
- **The rows.** `hosted_servers` holds one row per start: its owner and service account,
  its state, its data sets and pins, the owner's settings (range-checked columns, never a
  file), its place in the fleet, the serve image its start pinned (a deploy's new image
  reaches a server at its next start), the remote-admin login rendered for the service,
  the restarts used and why it failed; `hosted_server_events` keeps every transition with
  its cause. A start request (`hosted::request_server`) checks the entitlement, the
  account's live servers, its own-machine servers against the add-on, `ONNET_HOSTED_CAP`
  (a backstop beside the fleet's room) and the fleet's room in the transaction that writes
  it, under the hosted-start advisory lock. When a server is about to be placed its place
  is read again, oldest first, and one past the entitlement is stopped instead.
- **The owners' limits.** Every supervisor pass first stops what an entitlement no longer
  covers (`Supervisor::enforce_limits`): an owner's newest live servers past its servers,
  then its newest own-machine servers past the add-on, each stop's event naming why. The
  brake on the brake: a pass that would stop the servers of more owners than max(2, half
  the owners with live servers) reads as the billing cache gone wrong, so it stops none,
  logs `[hosted] ERROR the owners' limits held: ...` and holds (the heartbeat's
  `limits_held`). It releases itself once a full billing reconcile, with every sold price
  configured, has completed after it took its owners and a pass still finds them past
  their entitlements. An admin's override (`POST /api/admin/billing/reconcile` with
  `{"force_limits": true}`) applies the stops past the brake once. The hold is one row
  (`hosted_limits_hold`, `limits_hold.*`), shown by `GET /api/admin/billing`.

## The supervisor

- **The supervisor** (`supervisor.*`) is a thread of its own, started once the fleet program
  leads. Each pass (every second) asks that the fleet program still leads, steps every live
  machine, then walks every live server: `requested` → `provisioning` (placed; the trees
  pinned, the service account's and the admin's passwords minted, the files rendered for
  the slot) → `booting` (its container wanted until its machine's agent, the trees
  fetched, reports it running) → `listing` (until a listed RID holds one of the service
  account's HostKeys) → `running` → `stopping` → `stopped`, or `failed`. Every move past a
  request or a stop is the supervisor's compare-and-set, so a stop that lands mid-pass
  wins. A pass reads and writes rows alone and never calls the cloud: a launch is the
  machine's row and its `machine.launch` job in one transaction, a check of a booting,
  lost or unaddressed machine a deduped `machine.describe`, an end a `machine.terminate`,
  run by the workers (`fleet_jobs.*`; the service README's "Background jobs"), so a surge
  of starts launches its machines side by side. What a pass remembers of a machine and
  each kind's launch backoff are rows too, so a supervisor started again resumes its
  timers.
- **A stop** is the supervisor's own act: the row goes to `stopping` (asked by the owner
  or an admin, or by the supervisor for an account deleted or no longer active, or for the
  owners' limits), the supervisor pushes `ServerStopHosting` to the listing (MsgCode 7,
  scoped to the service account), and stops wanting the container once the listing has
  left the browser, or 60 s after the push. Its container exiting while `stopping`, with
  any code, finishes the stop. A stop is never read from an exit code
  (`apps/serve/README.md`, "Exit codes").
- **A crash** is what the server itself causes: its container exiting while it should
  run, the kernel's OOM kill, a start that failed (docker's 125, an image that did not
  pull), its container not running within 5 min of its machine's ready, no listing within
  3 min (10 min for an image pull on the ec2 driver; the clock waits while its machine is
  lost). The server lets its slot go and goes back to `provisioning` 15 s later while it
  has restarts left (3, the count reset by an hour of continuous `running`), else
  `failed`. Its next start is a new generation: a new container name with new
  credentials. **What the machine causes is no crash** and costs no restart: a launch the
  cloud refused (the server waits for the next), or a machine that never booted, was lost,
  gone, ended or drained (the server is placed again at once). The brake on a fleet that
  cannot launch is the launch backoff and the heartbeat's `launch_failures`.
- **No lobby live.** While no lobby instance takes clients (`lobbydb::live_lobbies`: a
  lobby outage, or a deployment's first release, whose fleet program rolls before any
  lobby), a gate hands a starting `opennova-serve` no lobby and it exits past its 30 s
  probe (retail's). So the supervisor starts nothing then: a server due to be placed waits
  in `provisioning` (`[hosted] server <id> waits for a live lobby`), an exit places the
  server again with no restart counted, and the listing's clocks wait. Once a lobby is
  live the waiting servers start, and an exit counts again.
- **The machines' lifecycle.** A machine whose launch job has not finished, or whose agent
  never reports, within the driver's boot timeout (15 min on EC2, 2 min for the local
  stack's containers) fails; one whose describe finds it gone first fails at once. A ready
  or draining machine whose agent has been silent 60 s is `lost`: gone, it ends; still
  running, it is terminated 3 min later, unless its agent reports again first. A shared
  machine with no server ends after `ONNET_FLEET_IDLE_MINUTES`; an own machine once its
  server has ended. The **mass-lost brake**: while more than max(2, half) of the live
  machines are lost at once (a bad hostd on every machine, the fleet's network gone: the
  fleet's fault, not the machines'), a lost machine is not terminated at its 3 min but
  held (`fleet_machines.lost_held_since_ms`, an ERROR line, the heartbeat's
  `machines_held`), each until it reports again or an admin ends it, whether or not the
  brake still holds by then. A draining machine moves its servers off one at a time and ends once
  it holds none, counting toward neither maximum, so a roll launches the machines it moves
  to. A refused launch or a machine that never booted holds every launch of its kind for
  1, 2, 4, 8, 16, then 30 minutes (`fleet_launch_backoff`), until a machine of that kind
  boots.
- **The desired state** (`fleet_desired_state.h`, the answer to each agent's sync): the
  containers of the live servers placed on the machine that are booting, listing, running
  or stopping, each with its slot, user, pinned image, trees (the base at `/opt/game`, the
  expansion at `/opt/game/expansion/<code>`), `opennova-serve`'s arguments, the slot's
  limits (`ONNET_FLEET_SLOT_CPUS`, `ONNET_FLEET_SLOT_MEMORY_MB`, 128 processes) and its
  working directory's files until the agent reports it running or exited (one reported
  `starting` waits for its image or its trees, and a hostd replaced meanwhile needs them
  again); and every tree they use, with its store URL and manifest. The files live in the
  fleet's memory alone and are dropped once the container runs or exited: a start a
  restart cut short is placed again with new credentials. The reply sends `drain` while
  the machine's row is draining.
- **Health.** While `running`, every pass reads its agent's report and every 20 s checks
  the listing and the admin link. Three checks in a row with no answer on the link fail
  the server. A listing no check has found for 8 min places the server again, no restart
  counted: every lobby of a deployment shares one address, so a lobby's replacement lists
  it again within about 4.5 min (the service README's "Lobby instances"), and the 8 min
  are the safety net. While its machine is lost, or no lobby is live, the clock waits.
  Nothing reaps an idle server or ends one for its age.
- **Reconciliation** at the supervisor's start: a machine row still `launching` whose job
  is gone fails; a `fleet.sweep` job runs (every cloud machine the driver lists that no
  live row names is terminated, but one whose row still launches; a live row the listing
  lacks is described, and ends if its machine is gone); a server whose files went with
  the fleet program (booting, its container reported neither running nor exited) is
  placed again, a new generation with its files rendered afresh and no restart counted;
  the rest resume, a running one given 8 min
  (`relist_grace_ms`) to list again before its health checks count. A driver that cannot
  list ends nothing. Every 5 min the sweep runs again.

## The machines

- **The agents and the internal listener** (`agent_route.*`, `internal_listener.*`,
  `agent_protocol.*`; the protocol is [`apps/hostd/README.md`](../../hostd/README.md)).
  Each machine runs `opennova-hostd` (on EC2 the fleet cluster's ECS daemon), whose
  machine's user-data carries a one-time bootstrap token (kept as its SHA-256 alone, 30
  minutes' life, spent at the agent's first sync) that it redeems for a long-lived secret
  (stored hashed); then it syncs every 5 s (1 s with work). Every sync opens with its
  envelope (the protocol, the agent's build and its image), recorded on the machine's row
  matched or not (`agent_protocol`, `agent_version`, `agent_image`): a sync of another
  protocol, older or newer, is held (the bare 404, the machine heard, its slots' reports
  kept, `protocol_mismatch_since` set) and ended only after 10 minutes of it (terminated,
  its servers placed again elsewhere, no restart counted: a drain would run each server it
  moved twice, the held agent stopping none), so hostd
  rolls ahead of the fleet program (or behind it) and no server restarts. Both routes are on the **internal listener**, the fleet's second HTTP
  server, bound while it leads to its task's private address at 8070 (from the ECS task
  metadata, with `ONNET_INTERNAL_HTTP_BIND` unset), which the machines' security group
  alone reaches; the agents reach it at `ONNET_INTERNAL_URL`, resolved at each request, so
  they follow a replaced fleet leader. The routes answer only a peer inside
  `ONNET_HOSTED_BOOTSTRAP_CIDRS` (and loopback on a loopback bind) from the machine's own
  recorded address; every refusal is one bare 404, logged without the token or the secret
  at most once per peer per minute.
- **Machine drivers** (`machine_driver.h`): the fleet's one seam to the cloud, called by
  the job workers alone. A driver launches the machine a row stands for (its id and
  one-time token), describes it, terminates it and lists the machines it runs for this
  deployment; what the supervisor knows of it without a call is its `MachineProfile`.
  Every driver's machine runs the real agent and has an address of its own, so each
  slot's ports are its fixed ones and the agent routes can check the peer.
  - `ONNET_HOSTED_PROVIDER=docker` (`docker_machine_driver.*`, the local stack's,
    `deploy/compose/docker-compose.hosting.yml`): each machine a container of the hostd
    image (`ONNET_HOSTED_HOSTD_IMAGE`) as its serve user on the stack's network, over the Docker Engine API (the stack's socket proxy
    admits the containers API alone), named `<deployment>-machine-<id>` and labelled with
    the deployment and machine, taking the local store's directory read-only from a data
    container, its one-time token put in it as a file (never an argument or a variable
    `docker inspect` shows). Its process is the image's `opennova-hostd` with the process
    runtime, its servers `opennova-serve` children. Its servers list through
    `ONNET_PUBLIC_HOST`, the developer's LAN address there.
  - `ONNET_HOSTED_PROVIDER=ec2` (`ec2_machine_driver.*`): EC2 machines from the launch
    template of their kind (`infra/aws`: the deployment's pinned ECS-optimized Amazon
    Linux 2023 image, the kind's type, the machines' security group and instance profile,
    IMDSv2 required), tagged with the deployment, the kind and the machine id, with the
    machine script (`machine/machine_user_data.sh`) as user-data: it writes the token and
    the machine id for the agent, reserves the slots' ports and points the image's ECS
    agent at the fleet cluster (`ONNET_HOSTED_EC2_CLUSTER`) with the machine's id and kind
    as instance attributes. The cluster's daemon service runs `opennova-hostd` on every
    machine that registers (`infra/aws/fleet_agent.tf`): it fences the slots' users,
    redeems the token and runs the servers (`apps/hostd/README.md`). The driver runs the AWS CLI through a
    `CommandRunner` (`apps/common/command_runner.*`: an argv, no shell, a 20 s timeout,
    only the system's and the `AWS_*` variables) on the worker task's role, describes from
    one cached `describe-instances` every 10 s, and terminates a machine at its end, never
    stops it.
- **Several servers on one address.** A stock retail client lists and joins every slot of
  a machine: NovaWorld lists a server at the source address and port of its datagrams,
  `/NWJoin.dll` hands a joiner that `host:port`, and the lobby keys its sessions by both.
  Each slot's `game.cfg` pins `mpnovaworldportmin` = `max` to its game port, and the
  containers run on the machine's own network (`--network host`).
- **The working directory** is rendered from scratch at every start (`hosted_settings.*`),
  in retail's formats: the host file (`server.host`: the owner's settings, `Replay 1` so
  the rotation never ends the session, the owner's first map last since the last line is
  the starting map; cp1252, CR LF), `game.cfg` (through the format's own writer, the
  NovaWorld network type and the slot's ports), `admin.cfg` (user `svc`, a 31-character
  random password, rights `7F`, `ip_restrict` the fleet's network) and the
  `--credentials` file. All but `game.cfg` are secret: sent to the agent only until it
  has reported the container, written owner-only, never logged; the admin login lives in
  the row's own columns, read by `hosted::admin_login_of` alone. `opennova-serve` runs
  with `/HOST server.host --credentials credentials.txt --master-host <host>
  --master-gate-port <port> /game <title>` and `/exp <code>` with an expansion. The
  settings' rules (`check_settings`): an offered data set, a name of 1..31 cp1252 bytes
  with no control character or double quote, a password of up to 16 printable ASCII
  characters with no space or quote, the numbers within the title's limits, an expansion
  of that data set with data, and 1..128 missions the trees list (none: every mission of
  both).
- **The admin listener on a machine.** The apps' TCP listeners set `SO_REUSEADDR`, so an
  `opennova-serve` restarted on a slot's admin port listens at once. A listen that still
  fails leaves the server running without its console, as retail ignores its listen's
  result; the health check, which counts the admin link, then fails the server.

## Static addresses

A **static address** (ADR 0066; `address_*.h`, `hosted_addresses`) is an own machine's
fixed public IPv4 with a name, `<name>.play.<domain>` (staging `play.staging.<domain>`),
which its owner keeps for the own-machine servers it starts: every start of a server that
names it runs on a machine carrying it, so the server lists at the same `address:32768`
each time. Each is its own Stripe subscription (the static-address add-on, quantity 1,
bought through Checkout once the account has an own machine for it, canceled through the
Portal's cancel flow), or an admin's comp address (`POST /api/admin/users/<id>/addresses`).
At most `ONNET_FLEET_MAX_ADDRESSES` are sold (`sold_capacity.*`).

- **The rows** (`address_repository.*`): `allocating` once named (the owner names a bought
  one, `POST /api/me/addresses`; a name of 3 to 32 lower-case letters, digits and `-`, a
  letter or digit first and last, no `--`, not all digits, none the service keeps:
  `address_names.*`), `ready` once allocated with its A record written, `lapsed` when its
  grant is gone or an admin released it, `releasing`, then `released`; `failed` when the
  cloud refused it (its error code: an exhausted Elastic IP quota shows there). A name is
  unique for good: a released address keeps it until an admin renames the row. A start
  names an address (`address_id`, with `own_machine`): it must be the owner's, ready, and
  no other live server's (409 `address_unavailable`; the database's one live server per
  address). An own machine launched for such a server carries it (`fleet_machines.address_id`).
- **The jobs** (`address_jobs.*`, the worker's): `address.allocate` (the address found by
  its tag first, so a run cut short never allocates a second; then allocated with its tags
  in one call; its record written TTL 300; a ready one's run moves its record to a new
  name), `address.associate` (onto the machine launched to carry it, without reassociation
  and only once the machine that held it before has ended; retried while the machine is
  pending; then a `machine.describe`, so the machine's public address follows),
  `address.release` (once no live machine carries it: its record deleted and in sync, the
  TTL and a minute waited out, its grant read again (a subscription granting once more
  keeps it), any association the cloud still shows ended, then released). `fleet.sweep`
  records the addresses the cloud holds for this deployment that no held row names
  (`hosted_address_orphans`, the admin's view), never releasing one.
- **The drivers**: `ec2_address_driver.*` (Elastic IPs through the AWS CLI, tagged
  `opennova:hosted`, `opennova:deployment`, `opennova:address-id` and `Name` at
  allocation, the only tags the worker's policy admits; every other call on an address of
  this deployment's tags alone) with `address_zone.*`'s `Route53Zone` (A records under the
  deployment's play zone, UPSERT and DELETE alone, `get-change` until in sync); and the
  local stack's `docker_address_driver.*` (the lowest free address of
  `ONNET_HOSTED_DOCKER_ADDRESSES`, which the docker machine driver starts the machine at;
  its association confirmed by inspect) with `FileZone` (a zone file the worker rewrites
  whole, which the smoke test reads).

## Service accounts and the listing's owner

- **Service accounts.** Each hosted server lists itself with an account of its own,
  `srv-<id>` (`players.kind = 'service'`), its 32-character CSPRNG password set again at
  every start (the credentials generation moves) and the account deleted in the
  transaction that ends the server. Its lister logs in through the unchanged
  `/NWLogin.dll` and `/NWHost.dll`, so the listing's HostKey is the service account's. No
  website session is ever minted for one, the admin users list hides them, the admin's
  user routes answer 404 for one, and no person may register or rename to a name starting
  with `srv-`.
- **A hosted listing's owner.** One rule (`hosted_repository.h`): a listing belongs to the
  account whose HostKey it holds now (`hostdb::listing_owned_by`), except that a live
  hosted server's service account stands for that server's owner. `GET /api/me/hosts`
  lists the caller's own listings and its live hosted servers' (with `hosted_server_id`);
  the owner pushes resolve the RID the same way and refuse a server that is stopping (409
  `server_stopping`). The stop of a live hosted server's listing (`POST
  /api/me/hosts/<rid>/stop` or `/api/admin/hosts/<rid>/stop`) is the hosted server's stop
  (202 `"status": "hosted_stop"`): no bare `ServerStopHosting` goes out, since the
  supervisor would read the exit as a crash.

## Stats

`stats_sampler.*`, `stats_repository.*`, `backend/migrations/0004_server_stats.sql`. The
first supervisor pass of each minute samples every live server into
`hosted_server_samples`: its state, its listing's players and mission, its admin link's
scene, its container's CPU (a share of the slot's CPUs) and memory as its agent last
reported them, and the bytes its slot sent that minute (the difference of the agent's
per-slot counter; a counter that started over counts from 0). Each sample rewrites its
server's hour in `hosted_server_hourly` and adds the bytes to its machine's month in
`fleet_machine_egress`. Unique players are counted from roster names held in memory for
the hour, never stored, and missions from scene changes: both are approximations, and the
replies say so. Samples are kept 48 h, hours 90 days (`stats.prune`); the routes read the
samples for 1h and 24h and the hours for 7d and 30d. The admin's **Server usage** page
(`/admin/usage`, `GET /api/admin/hosted/usage`) reads every live server's newest sample
and its last hour. The agents report what each slot sends, not what it receives.

## The admin link and the console

- **The admin link** (`admin_link.*`; ADR 0053 d5). A running server opens retail's remote
  admin on its slot's admin port, its `admin.cfg` granting `svc` to the fleet's
  network alone (its private address's /16, since a replaced fleet leader comes back on a
  new address; on a cloud machine its security group too). The service is that console's
  only client: owners never get the port or the login. The supervisor keeps one
  `AdminLink` per server in `running` (up once it runs, replaced when a restart rendered a
  new login, down as it leaves `running`) and publishes each link's state to
  `hosted_admin_links` (every row's `seen_at` moved every 5 s; a row 15 s stale reads as no
  link), which the web routes read. Each link is a thread of its own (at most
  `ONNET_HOSTED_CAP`) holding one logged-in session, speaking the protocol as `RAT.exe`
  does through the engine's client codec (`engine/net/admin/admin_protocol.h`): each
  command's replies are read by the dispatcher's per-verb count (`admin_reply_plan`), with
  a 200 ms quiet gap only where the line cannot tell. The link polls `GET GAMESTATE` every
  15 s, reconnects with a backoff (1 s doubling to 4 s), runs the queued commands (16 at
  most; `QUIT` is refused, the link's own) and ends with `QUIT`. The password is a log
  secret for the link's lifetime and never reaches a log line, a console line or a reply.
- **The console** is the table `hosted_server_console`, a capped ring of each server's
  newest 500 lines: the commands the routes ask a link to record (with their sender,
  `owner`, `admin` or `service`), each reply packet and the link's own status lines, in
  UTF-8 with UTC times. The reads the site polls are not recorded.

## Settings

Two sets (`fleet_config.h`; `../parts.h` `hosted_policy`, `driver_setup`). The fleet's
**policy**, which web, the fleet program and the worker read alike, so each decides alike
whether hosting is on: the provider, the store, placement, the slots' limits, the cap, the
serve image and the fleet's internal listener. The **driver's** own settings, which the
worker alone reads (it alone launches machines): with hosting on, a driver whose settings do
not read or are incomplete is the worker's boot FATAL (exit 1), never hosting off, while web
and the fleet take starts on the policy. Web, the fleet and the data program never read the
driver's settings, and only the worker's image carries the machine script. The data program
reads the store alone.

| Setting | Default | Read by | Meaning |
| --- | --- | --- | --- |
| `ONNET_HOSTED_PROVIDER` | `none` | web, fleet, worker | The machine driver: `ec2`, `docker` (the local stack's) or `none`. Any other value, fleet settings or a store that do not read, a serve image that is no image reference, or a loopback `ONNET_PUBLIC_HOST` is `none` with a boot warning naming why. |
| `ONNET_HOSTED_CAP` | 24 | web, fleet, worker | The most hosted servers live at once, across every account: a backstop beside the fleet's room. |
| `ONNET_OBJECT_STORE_URL` | none | web, fleet, worker, data | The object store of the data sets' trees and the update packages (`../object_store.h`, ADR 0064): `s3://<bucket>/data` (the deployment's ops bucket, through the program's role) or `file:///<dir>` (a developer's machine, the tests). Required for hosting; read whether or not hosting is on. |
| `ONNET_OBJECT_STORE_REGION` | none | web, fleet, worker, data | The S3 store's region (required with an `s3://` URL). |
| `ONNET_INTERNAL_HTTP_BIND` | `127.0.0.1:8070` | fleet (web, worker: the policy's default) | The fleet's internal listener, `<IPv4>:<port>`, a private or loopback address: unset in a deployment, where the fleet program binds its task's private address at 8070. With `ec2`, a fleet program whose bind is not private does not boot. |
| `ONNET_HOSTED_BOOTSTRAP_CIDRS` | none | fleet | Comma-separated private IPv4 CIDRs the agent routes answer (the machines' subnet); 127.0.0.0/8 is admitted besides on a loopback bind. |
| `ONNET_FLEET_SHARED_SLOTS` | 12 | web, fleet, worker | A new shared machine's slots (1..32). |
| `ONNET_FLEET_MAX_SHARED` | 2 | web, fleet, worker | The most shared machines taking servers at once (the worker's launch refuses a machine past it). |
| `ONNET_FLEET_MAX_OWN` | 2 | web, fleet, worker | The most own machines taking servers at once (the same). |
| `ONNET_FLEET_MAX_ADDRESSES` | 0 | web, fleet, worker | The static addresses the fleet sells (0: the add-on is off). Above 0 with hosting on, `ONNET_HOSTED_ADDRESS_ZONE` must name the zone (else a boot warning and none sold), and the worker needs its driver's address settings below (else its boot FATAL). |
| `ONNET_HOSTED_ADDRESS_ZONE` | none | web, fleet, worker | The play zone the addresses are named in, no trailing dot (`play.opennova.net`; infra/aws/edge.tf's): the site shows each one as `<name>.<zone>`. |
| `ONNET_FLEET_IDLE_MINUTES` | 30 | web, fleet, worker | How long a shared machine with no server lives on before it ends. |
| `ONNET_FLEET_SLOT_MEMORY_MB` | 512 | web, fleet, worker | Each container's memory (no swap). |
| `ONNET_FLEET_SLOT_CPUS` | 1 | web, fleet, worker | Each container's CPUs (a decimal). |
| `ONNET_HOSTED_SERVE_IMAGE` | none | web, fleet, worker | The serve image every start pins and the machines pull (pinned by digest at deploy). |
| `ONNET_FLEET_IMAGE_ID` | none | web, fleet, worker | The image the launch templates pin (the deployment's `fleet_ami`, an `ami-...`), which a fleet roll's preview holds each machine's against: one on it is kept. |
| `ONNET_INTERNAL_URL` | none | worker | The fleet's internal listener as the machines reach it, `http://<host>:<port>` (a deployment's Cloud Map name, `fleet.<deployment>.opennova.internal:8070`; the fleet's compose name locally): what a launch hands its machine. Required by both drivers. |
| `ONNET_HOSTED_DEPLOYMENT` | none | worker | The deployment tag (`ec2`) or label (`docker`) every machine carries and every describe, list and terminate is scoped to, so two stacks in one account or one Docker never touch each other's machines. |
| `ONNET_HOSTED_HOSTD_IMAGE` | none | worker | `docker`: the image every machine container runs (the hostd image: `opennova-hostd` with `opennova-serve` beside it). `app deploy` sets the release's on a deployment's worker too, which `ec2` ignores: a cloud machine's hostd is the fleet cluster's daemon, of the revision `app deploy` rolls. |
| `ONNET_HOSTED_EC2_LAUNCH_TEMPLATES` | none | worker | `ec2`: the launch templates by kind, `{"shared": "lt-...", "own": "lt-..."}` (`lt-<id>` or `lt-<id>:<version>`). |
| `ONNET_HOSTED_EC2_REGION` | none | worker | `ec2`: the region every AWS CLI call names. |
| `ONNET_HOSTED_EC2_CLUSTER` | none | worker | `ec2`: the fleet cluster every machine's ECS agent joins (`opennova-<deployment>-fleet`), whose daemon service runs `opennova-hostd`; required. |
| `ONNET_HOSTED_ADDRESS_ZONE_ID` | none | worker | `ec2`, with static addresses on: the play zone's Route 53 id (`Z...`). |
| `ONNET_HOSTED_EC2_SUBNETS` | none | worker | `ec2`: the subnets machines launch into, one per zone, each giving a public IPv4; a launch takes the one with fewer live machines and, when that zone cannot take it, the next. |
| `ONNET_HOSTED_AWS_CLI` | `/usr/local/bin/aws` | web, worker, data | The AWS CLI v2, an absolute path (no `PATH` search): the `ec2` driver's, the S3 object store's and the mailer's SES calls. |
| `ONNET_HOSTED_DOCKER_URL` | none | worker | `docker`: the Docker Engine API, `http://<host>:<port>` (the local stack's socket proxy). |
| `ONNET_HOSTED_DOCKER_NETWORK` | none | worker | `docker`: the network every machine joins. |
| `ONNET_HOSTED_DOCKER_DATA_FROM` | none | worker | `docker`: the container whose mounts every machine takes, read-only. |
| `ONNET_HOSTED_ADDRESS_ZONE_FILE` | none | worker | `docker`, with static addresses on: the zone file the worker writes, an absolute path (the local stack's `/var/lib/opennova/zone/play.localhost.zone`). |
| `ONNET_HOSTED_DOCKER_ADDRESSES` | none | worker | `docker`, with static addresses on: the addresses it hands out, a private /24 to /30 the stack's network holds but Docker never assigns (the local stack's `172.30.77.64/28`). |
| `ONNET_HOSTED_MACHINE_SCRIPT` | `/app/hosted/machine_user_data.sh` | worker | `ec2`: the user-data template, in the worker's image alone; a run outside the image names it. |

The deploy toolbox sets every `ec2` setting from the Terraform outputs (DEPLOY.md,
"Hosted game servers"); nobody copies an id by hand.

## Routes

The routes are `hosted_routes.*`, the admin's fleet routes `fleet_routes.*`, the stats
routes and the comp grant (with the admin user routes), all behind `ONNET_API_PROXY_ONLY`
like every `/api` route. Every refusal is an error code with the service's own message,
never database text; the reads answer `Cache-Control: no-store`. A website session's
writes need the CSRF header.

| Route | Access | Replies |
| --- | --- | --- |
| `GET /api/me/servers` | a website session | `enabled`, `full`, the account's `entitlement`, the offered `data_sets` (each with its version, missions, limits and expansions) and its `servers` (the live ones, then its newest 10 ended), each with its settings, pins, `update_available`, restarts, times and `listing` |
| `GET /api/me/servers/<id>` | a website session | one of the account's servers with its newest 50 `events`; 404 for another account's |
| `POST /api/me/servers` | a website session | the settings (`game_name` required; `data_set_id`, `expansion_id` and `missions` defaulted), `own_machine` and `address_id` (a static address of the account's, with `own_machine`): 202 `requested`; 400 `invalid_json`, `invalid_field`, `invalid_settings` (with the `field`); 403 `not_entitled`, `payment_past_due`; 409 `server_limit`, `no_own_machine`, `at_capacity`, `address_unavailable`; 429 (6, then one every 20 s, shared with the stop); 503 `hosting_disabled` |
| `GET /api/me/addresses` | a website session | `enabled`, the `zone`, how many bought addresses are `unnamed`, and the account's `addresses` (each with its name, hostname, state, IP and port, source, subscription, the live server using it, reason and times) |
| `POST /api/me/addresses` | a website session | `{"name"}`: the account's next bought address named, then allocated by a worker: 201 `{"address"}`; 400 `invalid_json`, `invalid_field`, `invalid_name`; 404; 409 `name_taken`, `no_address`; 503 `addresses_disabled` |
| `POST /api/me/servers/<id>/stop` | a website session | 202 `stopping` (or `stopped`); 404; 409 `not_live`; 429 |
| `POST /api/me/servers/<id>/update` | a website session | `update_requested` (placed again on the current trees; players drop): 202; 404; 409 `not_live`, `up_to_date` |
| `GET /api/me/servers/<id>/stats` | a website session | `?range=1h`, `24h` (the default), `7d`, `30d`: the server now (`live`), the range's `summary` (availability, players, egress, the `approximate` fields) and `series`; 400; 404; 429 (60, then two a second) |
| `GET /api/me/servers/stats` | a website session | the account's servers together, as above |
| `PUT /api/admin/users/<id>/comp` | admin | `{"servers": 0..16, "own_machines": 0..16}`, both: 200 `{"user", "entitlement"}`; 400 `invalid_json`, `missing_field`, `invalid_field`; 404 (or a service account). An entitlement down to no server stops the account's hosted servers, as a ban does |
| `GET /api/admin/hosted` | admin | the fleet: `enabled`, the provider, the cap, each kind's live servers and room, the live machines then the newest 10 ended (each with its state, cloud id, slots, addresses, agent and template versions, this month's egress) and the newest 100 servers (`?owner=`, `?state=live`, `ended`, `all`), each as the owner's plus its owner and tier, service account, container, place, generation, image, pins and players; never the admin password or an agent's credential |
| `GET /api/admin/hosted/<id>` | admin | one server with its events and its machine; 404 |
| `POST /api/admin/fleet/machines/<id>/drain` | admin | the machine takes no new server and its servers move off one at a time (no restart counted), then it ends: 202; 404; 409 `not_live` |
| `POST /api/admin/fleet/machines/<id>/end` | admin | the machine to `ending`, its servers placed again (no restart counted), its agent refused: 202; 404; 409 `not_live` |
| `GET /api/admin/fleet/roll` | admin | a fleet roll's preview (`fleet_roll.h`): each machine that takes servers with its image (EC2's), its servers and `replace` (another image than `ONNET_FLEET_IMAGE_ID`, or one not known) or `keep`, and the ids to replace |
| `POST /api/admin/fleet/roll` | admin | `{"machines": [<id>, ...]}`: each drains onto machines launched for them from the templates as they are (a new image, template or user-data reaches those; hostd and the serve image reach machines in place): 202 with their ids; 400 `invalid_field` for no list, or a machine that takes no servers |
| `POST /api/admin/hosted/update-all` | admin | `{}` or `{"data_set_id": n}`: every live server behind its data sets' current trees marked `update_requested`: 202 with how many; 400 |
| `GET /api/admin/hosted/<id>/stats`, `GET /api/admin/hosted/stats` | admin | any server's stats, and the fleet's |
| `GET /api/admin/hosted/usage` | admin | every live server's use of its slot (its newest sample within 3 min and its last hour summed), by machine and slot, with each machine's and the fleet's totals and the slots' limits; 429 |
| `POST /api/admin/hosted/<id>/stop` | admin | 202; 404; 409 `not_live` |
| `POST /api/admin/hosted/stop-all` | admin | the kill switch: every live server to `stopping`: 200 with how many |
| `GET /api/admin/addresses` | admin | `enabled`, the `zone`, how many the cloud `held` of the `sellable`, every address with its owner, allocation, record and lapse cause, and the `orphans` the last sweep found |
| `PUT /api/admin/addresses/<id>` | admin | `{"name"}`: a rename (any state: a released one's frees its old name; a ready one's record moves): 200; 400 `invalid_name`; 404; 409 `name_taken` |
| `POST /api/admin/addresses/<id>/release` | admin | `{"confirm": "<its name>"}`: lapsed by an admin, then released (its server placed again without it): 202; 400 `confirm_mismatch`; 404; 409 `not_releasable` |
| `POST /api/admin/users/<id>/addresses` | admin | `{"name"}`: a comp address for the account, at most its own machines: 201; 400 `invalid_name`; 404; 409 `name_taken`, `address_limit`; 503 `addresses_disabled` |

## Game data

**Game data** (`apps/novaworld_web/data_sets/`, ADR 0064): the data sets as rows
(`data_set_repository.h`, migrations 0011 and 0013), the trees the admin page uploads into
the object store (web begins each file's multipart upload, takes each part's PUT, 32 MiB
a part, inside Cloudflare's request limits, and completes it), and the update releases
(1SFS packages, uploaded the same way) the data program validates and applies and the
update listener serves. The jobs a route enqueues (`tree.finalize`, `release.validate`,
`release.apply`, `tree.prune`; the service README's "Background jobs") commit with the
state move that causes them, and the data program (`apps/novaworld_data`) alone runs them. Every route is the admin's, each state change logged with
its actor; without a store the store routes answer 503 `store_unavailable`, and a store
call that fails mid-way 502 `store_error` (its own text to the log alone). The shapes
(`DataSet`, `Tree`, `Release`) are `apps/novaworld_web/data_sets/`'s.

| Route | Replies |
| --- | --- |
| `GET /api/admin/data-sets` | the store's kind, the titles, the lobbies and every data set with its trees |
| `POST /api/admin/data-sets` | a base (a title, a `games` slug) or an expansion (a base `parent_id`, an `expansion_code` of 1 to 8 of `a-z0-9`), a `display_name`, the updater's `updater_login` and `command_file` both or neither, and `retail_updater` (names `ftp.novaworld.com` in the updater's SERVER.LST instead of `ONNET_UPDATE_HOST`): 201; 400; 409 `duplicate_login`, `duplicate_expansion`, `no_such_parent` |
| `PATCH /api/admin/data-sets/<id>` | the name, a base's lobby, the updater's fields: 200; 400; 404; 409 `duplicate_login` |
| `DELETE /api/admin/data-sets/<id>` | the data set and its trees' objects: 204; 404; 409 `in_use`; 503 |
| `POST /api/admin/data-sets/<id>/trees` | `{"files": [{"name", "size"}]}`: a new uploading tree and one multipart upload per file (names canonicalised by `data_set_files.h`): 201; 400 `invalid_files` (with `missing` and `unexpected`); 404; 409 `upload_in_flight`; 503 |
| `PUT /api/admin/data-sets/<id>/trees/<t>/files/<name>/parts/<n>?upload_id=<u>` | a part (at most 32 MiB) handed to the store: 200 `{"etag"}`; 400; 404; 409 `not_uploading`; 413; 502; 503 |
| `POST /api/admin/data-sets/<id>/trees/<t>/files/<name>/complete` | `{"upload_id", "parts"}`: the file's upload completed: 200; 400; 404; 409; 502; 503 |
| `POST /api/admin/data-sets/<id>/trees/<t>/finalize` | the tree `finalizing` and its `tree.finalize` job: 202; 404; 409 `not_uploading` |
| `DELETE /api/admin/data-sets/<id>/trees/<t>` | an uploading tree removed (204), a ready or failed one pruned by a `tree.prune` job (202); 404; 409 `current`, `baseline`, `pinned`, `in_progress`; 502; 503 |
| `GET /api/admin/releases` | the releases, newest first |
| `POST /api/admin/releases` | `{"name"?, "size"}`: a release `uploading` and its package's multipart upload: 201; 400; 503 |
| `PUT /api/admin/releases/<id>/parts/<n>?upload_id=<u>` | as a tree's parts |
| `POST /api/admin/releases/<id>/complete` | the upload completed, then the release `validating` with its `release.validate` job: 202; 400; 404; 409; 502; 503 |
| `POST /api/admin/releases/<id>/publish` | a draft published (the release each section's data set named before retired once none names it) with one `release.apply` job per section: 202; 404; 409 `not_draft` |
| `POST /api/admin/releases/<id>/retire` | 202; 404; 409 `not_published` |
| `DELETE /api/admin/releases/<id>` | a draft, invalid, retired or abandoned release and its package objects (a tree built from it keeps its row): 204; 404; 409 `published`, `validating`; 502; 503 |

## The update listener

**The update listener** (`apps/novaworld_web/updater/`, ADR 0061 d3, ADR 0064 d9):
UPDATE.EXE's gateway and update server in one, as retail's was, over the published
releases, started by web when `ONNET_UPDATE_HOST` is set and the object store is there. A
thread of its own pumps `net/update`'s connection machine on `ONNET_UPDATE_PORTS`, watched
by the stall watchdog; a second refreshes its catalog (`release_catalog.*`) from the rows
every 30 s: each data set with an updater login serves the section of its published
release that carries its command file, the package fetched into the cache and the section
unpacked beside it, streamed from disk (a connection's next replies are made only while
less than 64 KiB wait unsent). `SERVER.LST` names one host: `ftp.novaworld.com` for a data
set marked `retail_updater` (a retail install reaches it through the Switcher's hosts
block), else `ONNET_UPDATE_HOST`. A login no published release serves is refused. Logged
under `[update]`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_UPDATE_HOST` | none | The update host `SERVER.LST` sends a client of this deployment to (`update.<domain>`); unset, web runs no update listener. |
| `ONNET_UPDATE_PORTS` | `20044,20556,21068,21580` | The gateway ports, comma-separated (retail's four, which UPDATE.EXE tries in turn); a list that does not read is a boot FATAL. |
| `ONNET_UPDATE_CACHE_DIR` | a scratch directory | Where the packages and their unpacked sections are kept between refreshes. |

## The console routes

**The console routes** (`console_routes.*`) run the curated commands on a hosted server's
admin link, which the fleet program holds: a route writes its command to
`hosted_console_commands` with the deadline its budget allows and reads its row until it
is done; the fleet's console drain (`apps/novaworld_fleet/hosted/console_drain.*`) claims the queued commands
(`FOR UPDATE SKIP LOCKED`), runs each on its link and writes the outcome and each reply
packet (`hosted_console_replies`); the route reads them and deletes the row. A command
still queued when the route's wait ends is withdrawn, never sent; only a done row says a
command went out; one whose fleet leader went away is answered link down by the next
leader's drain. The owner's routes act on the caller's own servers by id (another
account's answers as no server's does); the admin's (`/api/admin/hosted/<id>/...`) on any.
In order: the session or admin check (401; 403 without the CSRF header on a `POST`); the
arguments (400, before anything is read or sent); the server (404 `not_found`, 409
`not_live`, `server_stopping`, `server_not_running`) with its link up (503
`admin_unavailable`); the admission (below); for an owner, a token from the account's
read bucket (30, then two a second) or, for a command that changes the server, its write
bucket (8, then one every 2 s; 429 `rate_limited`); then the link: 503
`admin_unavailable` or `admin_busy`, 504 `admin_timeout` (with `sent`) past the 3 s a
request has, and the command's own refusals.

**The admission.** A console request holds an HTTP handler thread while it waits on the
link (up to 3 s; a map load stalls a server's admin port), so the console never takes
every thread from the retail pages and logins: the HTTP listener runs
`max(hardware_concurrency(), 8)` handler threads; each account (the Bearer token as one)
has at most one console request in flight per web process (429 `console_busy` at once);
at most handler threads - 2 are admitted per web process (503 `console_full`); and a
rotation edit also holds its server's one edit, a Redis key (`nw:rotation:<id>`, 15 s at
most) across every replica, so a second edit answers 409 `rotation_busy` and two removes
can never both pass the last-map check. Each refusal carries `Retry-After`. `GET
.../console` reads the database alone and is not admitted. The site sends every admitted
call of a console page through one queue (`web/src/utils/consoleQueue.ts`), a command
ahead of the waiting polls, so a page never meets its own `console_busy`.

| Route (owner; the admin's: `/api/admin/hosted/<id>/...`) | Command | Replies |
| --- | --- | --- |
| `GET /api/me/servers/<id>/players` | `PLAYER LIST` | the players (slot, name, team, class, kills, deaths, ping); 409 `not_in_game`; 502 `admin_error` |
| `POST /api/me/servers/<id>/players/<slot>/punt`, `/ban`, `/swap`, `/kill` | `PLAYER PUNT` / `BAN` / `SWAPTEAM` / `KILL <slot>` | the result; 400 `invalid_slot` (1..255); 404 `player_not_found`; 409 `no_pcid`, `not_connected` |
| `GET /api/me/servers/<id>/missions` | `MISSION AVAILABLE` | the missions |
| `GET /api/me/servers/<id>/rotation` | `MISSION LIST` (and `GET GAMESTATE` when it lists none) | the rotation with its current and next; 409 `not_in_game` |
| `POST /api/me/servers/<id>/rotation` | `MISSION LIST`, then `MISSION ADD <file> <0\|1>` | `{"mission", "swap_sides"}` appended; 400; 404 `mission_not_found`; 409 `rotation_full` (128 maps, policy), `not_in_game` |
| `POST /api/me/servers/<id>/rotation/<index>/remove`, `/next` | `MISSION LIST`, then `MISSION REMOVE` / `SETNEXT <index>` | 400 `invalid_index`; 404 `no_such_entry` (the server answers OK for any index, so the rotation is read first); 409 `last_mission` (an empty rotation ends the session), `not_in_game` |
| `POST /api/me/servers/<id>/rotation/cycle` | `MISSION CYCLE` | the round ends now; 409 `not_in_game` |
| `GET /api/me/servers/<id>/chat` | `CHAT GET` | the chat window, oldest first |
| `POST /api/me/servers/<id>/chat` | `CHAT SEND <text>` | `{"text"}`: cp1252, no control character, not blank, at most 59 characters; 400 `invalid_text` |
| `GET /api/me/servers/<id>/console` | none | the console ring in any state, with the link's state (`?limit=`, `?after=`); 404 |
| `GET /api/admin/hosted/<id>/settings` (admin only) | `GET GAMESETTINGS` | the settings, in the server's order |

**The console's commands are a curated list** (policy, ADR 0053 d9;
`console_commands.h`): owners get exactly the commands above but `GET GAMESETTINGS`;
admins get that one wider read besides. There is no passthrough of an arbitrary admin
line: every line is composed from checked arguments (a slot 1..255, since the server
compares `(u8)atol` and 256 would be its own slot 0; an index 0..127; a `.bms` name of
letters, digits, `_` and `-`; chat text as above), so nothing reaches the server's files or
config (`SET`, `WEAPON SET`, `CMD`), its admin users or ban files (`ADMINUSER`, `BANLIST`)
or its session's end (`GOTO`, `MISSION CLEAR`, `QUIT`) outside the supervisor's stop.
`MISSION ADD` takes no insert position and no `ONESHOT`, and no `PLAYER ... ALL` form is on
the list. The commands that change the server are recorded in the console with their
sender; the reads are not.
