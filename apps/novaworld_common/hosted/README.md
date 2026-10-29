# Hosted servers

An account with a hosting entitlement can have the service run as many `opennova-serve`
servers for it as the entitlement allows, each in a slot of one of the fleet's machines
(a shared machine, or a dedicated machine, the server's alone), until it stops them (ADR 0053; this
directory; the `hosted_servers`, `hosted_server_events` and `fleet_machines` tables). Each
server runs the product its owner picks (and an expansion of it), pinned to the data it
started on (ADR 0064). Everything here is service policy. The service around it (the
programs, the jobs, the database) is [`../README.md`](../README.md). The fleet program's
own sources (the supervisor, the stats sampler, the admin links, the console drain, the
internal listener and the agents' routes) live in
[`../../novaworld_fleet/hosted/`](../../novaworld_fleet/README.md); the rest named here is
this directory's.

## The entitlement and the products

- **The hosting entitlement** (`billing/entitlement.*`): how many hosted servers an
  account may have live, in two pools. Its **shared servers**, on shared machines, are an
  admin's comp grant of servers (`comp_grants`, 0..16; `POST /api/register` writes one,
  `billing/tiers.h` `kRegistrationCompServers`) and its support tier's servers (a monthly
  Stripe subscription: tier 1, 2 or 3 grants 1, 2 or 3 more; `billing/tiers.h`), added
  together. Its **dedicated machines** are each one more server, on a machine of its own
  (`hosted_servers.own_machine`): one per dedicated-machine subscription (quantity 1
  each; no tier needed), plus an admin's comp grant of them (`comp_grants`, 0..16), so an
  admin, the local stack and CI give one without Stripe. Its servers are the two together
  (the registration's grant, tier 2 and two dedicated machines: five; an admin's grant of 9
  shared servers and 2 dedicated machines: eleven). Only an active player's account has one. It is
  read from the billing tables (the cache of Stripe the billing sync keeps), whether or
  not billing is configured, so a deploy that breaks billing never stops a payer's
  servers. A subscription grants while Stripe calls it `active`, `trialing` or `past_due`,
  until 48 h past its period's end; `past_due` keeps the account's servers running but
  starts no new one.
  What every account's entitlement holds is what the service has sold, and no checkout
  sells past the fleet (`model/sold_capacity.*`; [`../README.md`](../README.md) "Billing").
  A start is still refused `at_capacity` when the fleet has no room for it now.
- **The admin's settings** (`model/service_settings.*`, `service_meta` rows, `GET`/`PUT
  /api/admin/settings`; the fleet's sizing on the admin's Servers > Machines,
  Subscriptions Enabled on Billing): **Subscriptions Enabled** (off on a fresh
  deployment: no tier, dedicated machine or static address sells, a checkout answers 403
  `subscriptions_disabled`, and what an account already pays for still grants and is managed
  in the billing portal), and the fleet's maximum shared machines, their slots (a new
  machine's), maximum dedicated machines and maximum static addresses (static addresses
  only where the deployment runs them), each over the `ONNET_FLEET_*` value the programs
  booted with. Every program reads them where it decides (a start's room, a placement, a
  launch, the sold capacity), so a change lands at the next decision with no redeploy.
- **Products** (`../products/product_repository.*`, `../products/tree_files.*`,
  `backend/migrations/0001_baseline.sql`, `0002_players_projects.sql`, `0003_project_lobbies.sql`). A product is a row named by its `key`, one **game**
  (the baseline seeds the catalog's, an admin may make more, and any signed-in account its own:
  "Products" below): a base game (kind `game`: its `title`, the `gameprofile` code
  `opennova-serve` speaks with `/game`, `jo` or `dfx2`, and its `lobby`, the `games` row whose
  lobby it lists in, selected when the game is created) or an **expansion**
  of one (its `expansion_code`, `/exp jox01`, unique across every game); a third kind, `app`, is
  one of OpenNova's own programs with no
  tree and no project ("Products" below). Every game and expansion has a **project** (its
  OpenNova Editor project's revisions, `../products/project_repository.h`), and its releases are
  built from it. Its data lives in the object store
  (`ONNET_OBJECT_STORE_URL`) as **trees**, each one immutable version of the product's
  files (`../products/tree_files.h` names them) at `trees/<id>/`, built from nothing out
  of its published release (every release is whole), with the multiplayer missions its archives list. A
  base is **offered** while it has a ready current tree, with each of its expansions that
  has one, to an account when it is the service's own, public, or the account's own
  (`offered_products(db, caller)`: the start form's list and the start's check); the
  host-screen limits are the title's (`model/host_limits.h`). Hosting is on only
  with a store. At its placement a server is **pinned** to its products' current trees
  (`base_tree_id`, `expansion_tree_id`), kept through a crash restart or a move, until its
  owner asks for the update (`update_requested`: placed again on the current trees, no
  restart counted). One whose product has no data at its placement fails there.
- **Launch-time network and frame settings** (`model/hosted_settings.*`): the start form and
  `POST /api/me/servers` accept `send_holdoff_ticks` (0..255, default 12; both 0 and 1 permit
  a send each logic tick), `entity_send_budget` (100..1600 bytes, default 600),
  `max_packet_size` (100..16384 bytes, default 1300), and `lock_framerate` (boolean,
  default true). These are stored with the server and reused on crash restarts, moves and data
  updates. Omitted fields and rows created before migration `0007_hosted_network_tuning.sql`
  keep the previous defaults. The canonical configuration document keeps these fields at its
  top level; the migration moves older `options.max_packet_size` values to that field with the
  same effective game.cfg clamp. Settings edits save them for the next launch.

## The fleet

- **Machines and slots** (`model/placement.*`, `model/fleet_repository.*`,
  `backend/migrations/0001_baseline.sql`). A **shared machine** packs up to
  `ONNET_FLEET_SHARED_SLOTS` servers of any account; a **dedicated machine** runs exactly one,
  its owner's, launched for it and ended when it stops or moves to a shared machine
  (the owners' limits, below). A **slot** is one server's
  position on a machine (`protocol/slots.h`): slot k is the game's UDP port 32768 + k (open to
  everyone), the remote admin's TCP port 32800 + k (open to the service alone), the
  container's user 10001 + k and the working directory `/srv/slots/slot-k`. A
  `fleet_machines` row moves by compare-and-set: `launching` → `booting` (the cloud runs
  it) → `ready` (its agent reports) → `draining` → `ending` → `ended`, or `failed`, and
  `lost` once its agent has been silent 60 s. Every move writes its event in the same
  statement (`fleet_machine_events`, `backend/migrations/0001_baseline.sql`:
  from, to, the reason; a launch the cloud refused names its error code, `the launch
  failed (VcpuLimitExceeded)`, from the AWS CLI's text), and so does each new log stream
  of the machine's agent (`log stream <name>`, from and to its state). What the agent says
  of the machine at every sync lands on its row (its image, its own log stream, its CPU,
  memory, the trees' disk and its network, `stats_at`) and in `fleet_machine_trees` (each
  tree it holds: fetching, ready or failed, its bytes, its tries, the last failure). Two
  live servers never hold one slot (a unique index). An ended or failed machine's row is
  deleted 14 days after its end by `stats.prune`, its events and trees with it.
- **Placement** (`model/placement.h`, pure functions): a shared server goes to the fullest
  shared machine that takes servers with a free slot, its lowest free slot, so machines
  fill one at a time and an emptied one can end; with none, a shared machine is launched
  while fewer than `ONNET_FLEET_MAX_SHARED` take servers, else the server waits in
  `provisioning` for a slot. A server on a dedicated machine goes to the machine reserved for it,
  else to a new dedicated machine (at most `ONNET_FLEET_MAX_OWN`). A start the planner could not
  place, counting the servers already waiting, is `at_capacity`; a dedicated machine whose
  server stopped, failed or moved to a shared machine holds no place for it (the next
  pass ends that machine, and only then launches the start's: an owner who stops and
  starts again at the maximum is taken, and waits a pass for the machine).
- **The rows.** `hosted_servers` holds one row per start: its owner and service account,
  its state, its products and pins, the owner's settings (range-checked columns, never a
  file), its place in the fleet, the serve image its start pinned (a deploy's new image
  reaches a server at its next start), the remote-admin login rendered for the service,
  the restarts used and why it failed; `hosted_server_events` keeps every transition with
  its cause. Each run is a row of `hosted_server_starts` (its generation): made when its
  slot is assigned (the machine and slot), its image, trees and log stream
  (`<server id>/g<generation>`) written when it renders, `running_at` once its agent
  reports it running, and ended with its cause (`crash`, `timeout`, `fleet`, `owner_stop`
  and `admin_stop` (who asked its stop), `stop` (the service's own stop), `update`, `limits`, `no_data`, `link_lost`,
  `data_unavailable`) and how its container
  ended (its exit code, OOM flag and the server's exit line, or the agent's reason it never
  started). The stats' restarts are
  its crash and timeout ends. A start request (`hosted::request_server`) checks the entitlement, the
  account's live servers in the pool it asks for (on shared machines against its shared
  servers, `server_limit`; on a dedicated machine against its dedicated machines,
  `no_own_machine`), `ONNET_HOSTED_CAP`
  (a backstop beside the fleet's room) and the fleet's room in the transaction that writes
  it, under the hosted-start advisory lock. When a server is about to be placed its place
  in its pool is read again, oldest first, and one on a shared machine past the shared
  servers is stopped instead; one on a dedicated machine past the account's dedicated
  machines goes to a shared machine as the owners' limits move one (below), or is
  stopped when it cannot.
- **The owners' limits.** Every supervisor pass first acts on what an entitlement no
  longer covers (`Supervisor::enforce_limits`), each pool apart: it stops an owner's
  newest live servers on shared machines past its shared servers, then moves its newest
  servers on dedicated machines past its dedicated machines (paid ones and an admin's
  grant alike) to shared machines, newest first, each judged as a shared server's start
  is, under the hosted-start lock: the owner must have a shared server free (the stops
  just asked still count) and the fleet a shared place for it (`fleet_has_room`), with the
  server's dedicated-machine choice cleared and its slot let go in the same transaction
  (`hosted::clear_own_machine`). A moved server's match ends: its start ends `limits`, its
  dedicated machine ends after it, its static address stays the account's but is no
  longer the server's (only a dedicated machine carries one), and it starts again on a
  shared machine, no restart counted (the owner's phase reads `moving`). One its owner has
  no shared server free for, or the fleet no shared room for, stops; a shared server never
  moves onto a free dedicated machine. Each event names why: `its owner's support tier
  allows 1 hosted server on shared machines`, `moved to a shared machine: its owner has 1
  dedicated machine now`, `its owner has 1 dedicated machine now and no shared server
  free`, `its owner has 0 dedicated machines now and the fleet has no shared room`; a
  stopped server's start keeps the limit it was past (`limit_kind` `shared_servers`,
  `no_shared_server` or `no_shared_room`, and `limit_count`, its owner's shared servers
  or dedicated machines then), which its owner's page says in the owner's words. The brake on the
  brake: a pass that would stop or move the servers of more owners than max(2, half the
  owners with live servers) reads as the billing cache gone wrong, so it acts on none,
  logs `[hosted] ERROR the owners' limits held: ...` and holds (the heartbeat's
  `limits_held`). It releases itself once a full billing reconcile, with every sold price
  configured, has completed after it took its owners and a pass still finds them past
  their entitlements. An admin's override (`POST /api/admin/billing/reconcile` with
  `{"force_limits": true}`) applies the stops and moves past the brake once. The hold is
  one row (`hosted_limits_hold`, `model/limits_hold.*`), shown by `GET /api/admin/billing`.

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
  run by the workers (`model/fleet_jobs.*`, their handlers `cloud/fleet_job_handlers.*`; the
  service README's "Background jobs"), so a surge
  of starts launches its machines side by side. What a pass remembers of a machine and
  each kind's launch backoff are rows too, so a supervisor started again resumes its
  timers.
- **A stop** is the supervisor's own act: the row goes to `stopping` (asked by the owner
  or an admin, or by the supervisor for an account deleted or no longer active, or for the
  owners' limits, its open start taking who asked as the cause its end keeps:
  `owner_stop`, `admin_stop`, `stop`, `limits`), the supervisor pushes `ServerStopHosting`
  to the listing (MsgCode 7, scoped to the service account), and stops wanting the
  container once the listing has
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
  gone, ended or drained (the server is placed again at once). Nor is a start whose data
  its machine could not fetch (its agent's tries spent, `its data could not be fetched: `
  and the fetch's error; the cause `data_unavailable`): placed again 15 s later, so the
  machine's agent forgets the failed tree and its next start fetches it afresh, no restart
  counted; its event says so without the agent's line, which its start keeps for an admin.
  The brake on a fleet that cannot launch is the launch backoff and the heartbeat's
  `launch_failures`.
- **No lobby live.** While no lobby instance takes clients (`lobbydb::live_lobbies`: a
  lobby outage, or a deployment's first release, whose fleet program rolls before any
  lobby), a gate hands a starting `opennova-serve` no lobby and it gives up 20 s into its
  gate wait and exits (retail's dedicated host's; the probe itself stops at 10 s). So the
  supervisor starts nothing then: a server due to be placed waits
  in `provisioning` (`[hosted] server <id> waits for a live lobby`), an exit places the
  server again with no restart counted, and the listing's clocks wait. Once a lobby is
  live the waiting servers start, and an exit counts again.
- **The machines' lifecycle.** A machine whose launch job has not finished, or whose agent
  never reports, within the driver's boot timeout (15 min on EC2, 2 min for the local
  stack's containers) fails; one whose describe finds it gone first fails at once. A ready
  or draining machine whose agent has been silent 60 s is `lost`: gone, it ends; still
  running, it is terminated 3 min later, unless its agent reports again first. A shared
  machine with no server ends after `ONNET_FLEET_IDLE_MINUTES`; a dedicated machine once its
  server has ended or moved to a shared machine. The **mass-lost brake**: while more than
  max(2, half) of the live machines are lost at once (a bad hostd on every machine, the
  fleet's network gone: the fleet's fault, not the machines'), a lost machine is not
  terminated at its 3 min but held (`fleet_machines.lost_held_since_ms`, an ERROR line,
  the heartbeat's `machines_held`), each until it reports again or an admin ends it,
  whether or not the brake still holds by then. A draining machine moves its servers off one at a time and ends once
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
  it again within its next update and reconnect, under a minute (the old lobby's goodbye,
  the service README's "Lobby instances"), or about 4.5 min after a lobby that died
  without its stop, and the 8 min are the safety net. While its machine is lost, or no lobby is live, the clock waits.
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

## The fleet's health

- **Findings over the rows** (`model/fleet_health.*`): `read_health` reads the rows in one
  snapshot and `fleet_health` judges them, a pure function, into findings, each with a
  level (`danger`, `warn`, `info`), its subject, what is wrong, since when, the admin page
  that shows it and its rule: a start stalled past 15 min (one waiting for its static
  address says so); a machine launching or booting past 20 min, draining past 60, ending
  past 15, lost past 10, or moving more than 6 times in an hour; a lost machine the brake
  holds (danger); a protocol mismatch (danger); an agent on another hostd image than the
  release's (`ONNET_HOSTED_HOSTD_IMAGE`), and the machines on another image than the
  launch templates' (`ONNET_FLEET_IMAGE_ID`, one finding: a fleet roll); a machine's CPU
  past 85 %, memory past 90 % or disk under 15 % free; a machine whose servers ran over
  1 % of a minute's ticks late; a kind's launches held, with the last failure's cause; no
  lobby that takes clients (danger); a server failed in the last 24 h; a server whose data
  could not be fetched at its last 3 starts in a row, or for over 15 min (danger past an
  hour: such a start costs no restart, so the server never fails of it), with its product,
  its trees and hostd's last reason; a failed or orphan
  static address, a release that has not moved for 16 min, and addresses held while the
  add-on is off; the hosting sold past what the fleet sells. Rows too old to judge (a
  machine's report or use, a server's ticks) are a warn `unknown`, never healthy. The
  admin's attention list (`GET /api/admin/attention`) and the fleet leader's heartbeat
  read the same findings.

## The machines

- **The agents and the internal listener** (`agent_route.*`, `internal_listener.*`,
  `protocol/agent_protocol.*`; the protocol is [`apps/hostd/README.md`](../../hostd/README.md)).
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
- **Machine drivers** (`model/machine_driver.h`): the fleet's one seam to the cloud, called by
  the job workers alone. A driver launches the machine a row stands for (its id and
  one-time token), describes it, terminates it and lists the machines it runs for this
  deployment; what the supervisor knows of it without a call is its `MachineProfile`
  (`model/machine_profile.*`, made from the policy with no driver: the boot timeout, a
  slot's fixed ports, the fleet's network its admin.cfg admits).
  Every driver's machine runs the real agent and has an address of its own, so each
  slot's ports are its fixed ones and the agent routes can check the peer.
  - `ONNET_HOSTED_PROVIDER=docker` (`cloud/docker_machine_driver.*`, the local stack's,
    `deploy/compose/docker-compose.hosting.yml`): each machine a container of the hostd
    image (`ONNET_HOSTED_HOSTD_IMAGE`) as its serve user on the stack's network, over the Docker Engine API (the stack's socket proxy
    admits the containers API alone), named `<deployment>-machine-<id>` and labelled with
    the deployment and machine, taking the local store's directory read-only from a data
    container, its one-time token put in it as a file (never an argument or a variable
    `docker inspect` shows). Its process is the image's `opennova-hostd` with the process
    runtime, its servers `opennova-serve` children. Its servers list through
    `ONNET_PUBLIC_HOST`, the developer's LAN address there.
  - `ONNET_HOSTED_PROVIDER=ec2` (`cloud/ec2_machine_driver.*`): EC2 machines from the launch
    template of their kind (`infra/aws`: the deployment's pinned ECS-optimized Amazon
    Linux 2023 image, the kind's type, the machines' security group and instance profile,
    IMDSv2 required), tagged with the deployment, the kind and the machine id, with the
    machine script (`cloud/machine/machine_user_data.sh`) as user-data: it writes the token and
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
- **The working directory** is rendered from scratch at every start (`model/hosted_settings.*`),
  in retail's formats: the host file (`server.host`: the owner's settings and replay preference, maps in play order with `--rotation-first` selecting entry zero; cp1252, CR LF), `game.cfg` (through the format's own writer, the
  NovaWorld network type, the slot's ports, and the owner's `mpmaxpacketsize` and
  `lock_framerate`), `admin.cfg` (user `svc`, a 31-character
  random password, rights `8000007F`, `ip_restrict` the fleet's network) and the
  `--credentials` file. All four files are secret: sent to the agent only until it
  has reported the container, written owner-only, never logged; the admin login lives in
  the row's own columns, read by `hosted::admin_login_of` alone. `opennova-serve` runs
  with `/HOST server.host --credentials credentials.txt --master-host <host>
  --master-gate-port <port> --status-page off --stats-file tick-stats.json --exit-file
  exit.txt --send-holdoff-ticks <ticks> --entity-send-budget <bytes> --rotation-first /game <title>`
  and `/exp <code>` with an expansion: no status page (it would
  stream the players' names and chat into the server's log every 200 ms), and the two
  files the machine's agent reads in the slot's directory, the server's tick health and the
  error a non-zero exit ended on (`apps/serve/README.md`, "Supervised runs"). A server whose
  owner asked to be listed on NovaLogic's NovaWorld (`list_on_novalogic`) also gets
  `--second-listing-file second-listing.json`, the listing's state, which the agent forwards
  and its page shows (off when the service offers no listing at that start), and, while the
  service offers it, the second listing itself (`--also-master-host` and the rest, ADR 0068). The
  settings' rules (`check_settings`): an offered product, a name of 1..31 cp1252 bytes
  with no control character or double quote, a password of up to 16 printable ASCII
  characters with no space or quote, the numbers within the title's limits, an expansion
  of that product with data, and 1..128 missions the trees list (none: every mission of
  both).
- **The admin listener on a machine.** The apps' TCP listeners set `SO_REUSEADDR`, so an
  `opennova-serve` restarted on a slot's admin port listens at once. A listen that still
  fails leaves the server running without its console, as retail ignores its listen's
  result; the health check, which counts the admin link, then fails the server.

## Static addresses

A **static address** (ADR 0066; `model/address_*.h`, `cloud/address_*.h`, `hosted_addresses`) is a dedicated machine's
fixed public IPv4 with a name, `<name>.play.<domain>` (staging `play.staging.<domain>`),
which its owner keeps for the servers it starts on dedicated machines: every start of a server that
names it runs on a machine carrying it, so the server lists at the same `address:32768`
each time. Each is its own Stripe subscription (the static-address add-on, quantity 1,
bought through Checkout once the account has a dedicated machine for it, canceled through the
Portal's cancel flow), or an admin's comp address (`POST /api/admin/users/<id>/addresses`),
which lapses once nothing covers it ("The fleet's pass" below). At most `ONNET_FLEET_MAX_ADDRESSES` are sold (`model/sold_capacity.*`), counting what the
cloud holds too (an account's rows that hold a grant where they pass its grants, and every
lapsed or releasing row, whose Elastic IP is held until released), so the fleet never
holds more Elastic IPs than it sells. With the add-on off (`ONNET_FLEET_MAX_ADDRESSES` 0,
or no zone name) none is sold or allocated, and a worker whose address settings still read
releases the held ones.

- **The rows** (`model/address_repository.*`): `allocating` once named (the owner names a bought
  one, `POST /api/me/addresses`; a name of 3 to 32 lower-case letters, digits and `-`, a
  letter or digit first and last, no `--`, not all digits, none the service keeps:
  `model/address_names.*`), `ready` once allocated with its A record written, `lapsed` when its
  grant is gone or an admin released it, `releasing`, then `released` (its subscription,
  granting still, may name another); `failed` when the cloud refused it (its error code: an
  exhausted Elastic IP quota shows there), `allocating` again when an admin retries it
  (`POST /api/admin/addresses/<id>/retry`) or, for an exhausted quota, once the fleet's pass
  finds its `retry_at` reached (5 min after the refusal, doubling to 6 h). A name is
  unique for good: a released address keeps it until an admin renames the row. A start
  names an address (`address_id`, with `own_machine`): it must be the owner's, ready, and
  no other live server's (409 `address_unavailable`; the database's one live server per
  address). A dedicated machine launched for such a server carries it (`fleet_machines.address_id`).
- **The jobs** (`model/address_jobs.*`, the worker's handlers `cloud/address_job_handlers.*`):
  `address.allocate` (the address found by its tag first, so a run cut short never allocates
  a second; then allocated with its tags
  in one call; its record written TTL 300 and recorded on the row whatever its state by
  then; a ready one's run moves its record to a new name; with the add-on off it allocates
  nothing and waits), `address.associate` (onto the machine launched to carry it, without reassociation
  and only once the machine that held it before has ended; retried while the machine is
  pending; then a `machine.describe`, so the machine's public address follows),
  `address.release` (once no live machine carries it and no `address.allocate` of it is
  queued or running, so it sees every record written: its record deleted and in sync, the
  TTL and a minute waited out, its grant read again (a subscription granting once more,
  or a comp address its account covers again, keeps it), any association the cloud still
  shows ended, then released). `fleet.sweep`
  records the addresses the cloud holds for this deployment that no held row names
  (`hosted_address_orphans`, the admin's view), never releasing one.
- **The drivers**: `cloud/ec2_address_driver.*` (Elastic IPs through the AWS CLI, tagged
  `opennova:hosted`, `opennova:deployment`, `opennova:address-id` and `Name` at
  allocation, the only tags the worker's policy admits; every other call on an address of
  this deployment's tags alone) with `cloud/address_zone.*`'s `Route53Zone` (A records under the
  deployment's play zone, UPSERT and DELETE alone, `get-change` until in sync); and the
  local stack's `cloud/docker_address_driver.*` (the lowest free address of
  `ONNET_HOSTED_DOCKER_ADDRESSES`, which the docker machine driver starts the machine at;
  its association confirmed by inspect) with `FileZone` (a zone file the worker rewrites
  whole, which the smoke test reads).
- **The fleet's pass** (`apps/novaworld_fleet/hosted/supervisor.*`): a dedicated machine
  launched for a server that names an address carries it; once the machine has a cloud id
  the pass asks its `address.associate` and a describe, and holds the server's container
  back (booting, its clock waiting) until the machine's public address is the address, so
  the lobby lists it at `<address>:32768` from its first packet (a dedicated machine's public
  address never changes while it runs a server). An address whose grant is gone lapses:
  a subscription's whose subscription no longer grants, and a comp address nothing covers
  (`uncovered_comp_addresses`): every comp address of an account deleted or no longer an
  active player's (banned), and, while an account's addresses that count (its granting
  static-address subscriptions and its comp addresses) pass its dedicated machines, its newest
  comp ones until they no longer do (an admin's cut of its dedicated machines; a
  paid address never lapses for that, but with its subscription, which the billing sync
  sets to cancel at its period's end once it is past the dedicated machines:
  [`../README.md`](../README.md) "A static address past the dedicated machines ends"). One the billing cache
  judged (a subscription's, or a comp one of an account with subscriptions, whose dedicated
  machines may be the cache's) lapses only once a clean reconcile with Stripe has
  completed since the pass first saw it so, and never more at once than max(2, half the
  addresses) (the owners' limits' brake: it logs an ERROR and lapses none of them; an
  admin may release one); one its account and its admin's grant alone judged lapses at
  once, billing configured or not. A lapsed address's machine ends (the association goes with
  it) and its server is placed again without it, no restart counted, its event saying
  why; once no live machine carries the address it goes releasing. With the add-on on, an
  allocating address whose `address.allocate` ran out of attempts (a throttle, a cloud
  that did not answer) is allocated again, and a failed one is tried again at its
  `retry_at`.

## Service accounts and the listing's owner

- **Service accounts.** Each hosted server lists itself with an account of its own,
  `srv-<id>` (`players.kind = 'service'`), its 32-character CSPRNG password set again at
  every start (the credentials generation moves) and the account deleted in the
  transaction that ends the server. Its lister logs in through the unchanged
  `/NWLogin.dll` and `/NWHost.dll`, so the listing's HostKey is the service account's. No
  website session is ever minted for one, the admin users list hides them, the admin's
  user routes answer 404 for one, and no person may register or rename to a name starting
  with `srv-`.
- **A hosted listing's owner.** One rule (`model/hosted_repository.h`): a listing belongs to the
  account whose HostKey it holds now (`hostdb::listing_owned_by`), except that a live
  hosted server's service account stands for that server's owner. `GET /api/me/hosts`
  lists the caller's own listings and its live hosted servers' (with `hosted_server_id`);
  the owner pushes resolve the RID the same way and refuse a server that is stopping (409
  `server_stopping`). The stop of a live hosted server's listing (`POST
  /api/me/hosts/<rid>/stop` or `/api/admin/hosts/<rid>/stop`) is the hosted server's stop
  (202 `"status": "hosted_stop"`): no bare `ServerStopHosting` goes out, since the
  supervisor would read the exit as a crash.

## Stats

`stats_sampler.*`, `model/stats_repository.*`, `backend/migrations/0001_baseline.sql`. The
first supervisor pass of each minute samples every live server into
`hosted_server_samples`: its state, its listing's players and mission, its admin link's
scene, its container's CPU (a share of the slot's CPUs) and memory as its agent last
reported them, the bytes its slot sent that minute (the difference of the agent's
per-slot counter; a counter that started over counts from 0), its bytes in and out as
counters since its first sample on the slot (the differences summed, so a move never
jumps them; a reader takes the differences, a count below the last starting over), and
its server's last minute of ticks (frame time p50, p99, longest, the late frames). Each
sample rewrites its server's hour in `hosted_server_hourly` (with the peak memory, the
bytes in and out, the worst p99 and the late ticks) and adds the bytes to its machine's
month in `fleet_machine_egress`, which outlives the machine's row to its own 13 months.
Every live machine gets its minute too (`fleet_machine_samples`: its agent's last report
of its use, none older than 2 minutes, and how many servers it holds), rolled up by the
hour by `stats.prune` (`fleet_machine_hourly`); both keep a plain machine id, so the
history outlives the machine. Unique players are counted from roster names held in memory
for the hour, never stored, and missions from scene changes: both are approximations, and
the replies say so. Samples are kept 48 h, hours 400 days (`stats.prune`), the servers'
and the machines'; the routes read the samples for 1h and 24h and the hours for 7d and
30d. The admin's **Servers** page (`/admin/servers`, `GET /api/admin/hosted/usage`) shows
every live server's newest sample and its last hour in its table.
`GET /api/admin/usage?subject=server|machine&id=<id>` reads one server's or machine's
minutes and hours in the shape every subject shares ([`../README.md`](../README.md)
"Usage").

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
  most), supports `QUIT` by closing and reconnecting its admin session, and ends with `QUIT`. The password is a log
  secret for the link's lifetime and never reaches a log line, a console line or a reply.
- **The console** is the table `hosted_server_console`, a capped ring of each server's
  newest 500 lines: the commands the routes ask a link to record (with their sender,
  `owner`, `admin` or `service`), each reply packet and the link's own status lines, in
  UTF-8 with UTC times. The reads the site polls are not recorded.

## Settings

Two sets (`model/fleet_config.h` with `model/hosted_policy.h` `hosted_policy`;
`cloud/driver_config.h` `driver_setup`). The fleet's
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
| `ONNET_OBJECT_STORE_URL` | none | web, fleet, worker, data | The object store of the products' trees and the update packages (`../core/object_store.h`, ADR 0064): `s3://<bucket>/data` (the deployment's ops bucket, through the program's role) or `file:///<dir>` (a developer's machine, the tests). Required for hosting; read whether or not hosting is on. |
| `ONNET_OBJECT_STORE_REGION` | none | web, fleet, worker, data | The S3 store's region (required with an `s3://` URL). |
| `ONNET_DOWNLOADS_URL` | none | data | The downloads host's bucket (`../products/downloads_job.h`, ADR 0070): `s3://<bucket>` (the deployment's downloads bucket at its top, in `ONNET_OBJECT_STORE_REGION`, through the data program's role) or `file:///<dir>` (the tests). Unset (the local stack), `downloads.publish` writes nothing. |
| `ONNET_INTERNAL_HTTP_BIND` | `127.0.0.1:8070` | fleet (web, worker: the policy's default) | The fleet's internal listener, `<IPv4>:<port>`, a private or loopback address: unset in a deployment, where the fleet program binds its task's private address at 8070. With `ec2`, a fleet program whose bind is not private does not boot. |
| `ONNET_HOSTED_BOOTSTRAP_CIDRS` | none | fleet | Comma-separated private IPv4 CIDRs the agent routes answer (the machines' subnet); 127.0.0.0/8 is admitted besides on a loopback bind. |
| `ONNET_FLEET_SHARED_SLOTS` | 12 | web, fleet, worker | A new shared machine's slots (1..32). |
| `ONNET_FLEET_MAX_SHARED` | 2 | web, fleet, worker | The most shared machines taking servers at once (the worker's launch refuses a machine past it). |
| `ONNET_FLEET_MAX_OWN` | 2 | web, fleet, worker | The most dedicated machines taking servers at once (the same). |
| `ONNET_FLEET_MAX_ADDRESSES` | 0 | web, fleet, worker | The static addresses the fleet sells (0: the add-on is off). Above 0 with hosting on, `ONNET_HOSTED_ADDRESS_ZONE` must name the zone (else a boot warning and none sold), and the worker needs its driver's address settings below (else its boot FATAL). At 0 none is sold or allocated, while a worker whose address settings still read (the zone's name among them) releases the addresses held. |
| `ONNET_HOSTED_ADDRESS_ZONE` | none | web, fleet, worker | The play zone the addresses are named in, no trailing dot (`play.opennova.net`; infra/aws/edge.tf's): the site shows each one as `<name>.<zone>`. |
| `ONNET_FLEET_IDLE_MINUTES` | 30 | web, fleet, worker | How long a shared machine with no server lives on before it ends. |
| `ONNET_FLEET_SLOT_MEMORY_MB` | 512 | web, fleet, worker | Each container's memory (no swap). |
| `ONNET_FLEET_SLOT_CPUS` | 1 | web, fleet, worker | Each container's CPUs (a decimal). |
| `ONNET_HOSTED_SERVE_IMAGE` | none | web, fleet, worker | The serve image every start pins and the machines pull (pinned by digest at deploy). |
| `ONNET_FLEET_IMAGE_ID` | none | web, fleet, worker | The image the launch templates pin (the deployment's `fleet_ami`, an `ami-...`), which a fleet roll's preview holds each machine's against (one on it is kept), and the fleet's health too (`model/fleet_health.h`: a machine's `outdated`, the `outdated_image` finding). |
| `ONNET_HOSTED_HOSTD_IMAGE` | none | web, fleet, worker | The hostd image the release pins (`app deploy` sets it, by digest): with `docker`, the image every machine container runs (`opennova-hostd` with `opennova-serve` beside it); everywhere, what the fleet's health holds each machine's agent image against (a machine's `outdated`, the `outdated_agent` finding). A cloud machine's hostd is the fleet cluster's daemon, of the revision `app deploy` rolls. |
| `ONNET_INTERNAL_URL` | none | worker | The fleet's internal listener as the machines reach it, `http://<host>:<port>` (a deployment's Cloud Map name, `fleet.<deployment>.opennova.internal:8070`; the fleet's compose name locally): what a launch hands its machine. Required by both drivers. |
| `ONNET_HOSTED_DEPLOYMENT` | none | worker | The deployment tag (`ec2`) or label (`docker`) every machine carries and every describe, list and terminate is scoped to, so two stacks in one account or one Docker never touch each other's machines. |
| `ONNET_HOSTED_EC2_LAUNCH_TEMPLATES` | none | worker | `ec2`: the launch templates by kind, `{"shared": "lt-...", "own": "lt-..."}` (`lt-<id>` or `lt-<id>:<version>`). |
| `ONNET_HOSTED_EC2_REGION` | none | worker | `ec2`: the region every AWS CLI call names. |
| `ONNET_HOSTED_EC2_CLUSTER` | none | worker | `ec2`: the fleet cluster every machine's ECS agent joins (`opennova-<deployment>-fleet`), whose daemon service runs `opennova-hostd`; required. |
| `ONNET_HOSTED_ADDRESS_ZONE_ID` | none | worker | `ec2`, for the static addresses (on, or off with addresses still held): the play zone's Route 53 id (`Z...`). |
| `ONNET_HOSTED_EC2_SUBNETS` | none | worker | `ec2`: the subnets machines launch into, one per zone, each giving a public IPv4; a launch takes the one with fewer live machines and, when that zone cannot take it, the next. |
| `ONNET_HOSTED_AWS_CLI` | `/usr/local/bin/aws` | web, worker, data | The AWS CLI v2, an absolute path (no `PATH` search): the `ec2` driver's, the S3 object store's and the mailer's SES calls. |
| `ONNET_HOSTED_DOCKER_URL` | none | worker | `docker`: the Docker Engine API, `http://<host>:<port>` (the local stack's socket proxy). |
| `ONNET_HOSTED_DOCKER_NETWORK` | none | worker | `docker`: the network every machine joins. |
| `ONNET_HOSTED_DOCKER_DATA_FROM` | none | worker | `docker`: the container whose mounts every machine takes, each in its mode there (the local stack's `machine-data`: the store read-only, the log store read-write). |
| `ONNET_HOSTED_ADDRESS_ZONE_FILE` | none | worker | `docker`, for the static addresses (on, or off with addresses still held): the zone file the worker writes, an absolute path (the local stack's `/var/lib/opennova/zone/play.localhost.zone`). |
| `ONNET_HOSTED_DOCKER_ADDRESSES` | none | worker | `docker`, for the static addresses (on, or off with addresses still held): the addresses it hands out, a private /24 to /30 the stack's network holds but Docker never assigns (the local stack's `172.30.77.64/28`). |
| `ONNET_HOSTED_MACHINE_SCRIPT` | `/app/hosted/machine_user_data.sh` | worker | `ec2`: the user-data template, in the worker's image alone; a run outside the image names it. |
| `ONNET_LOG_STORE` | none | web | Where the admin's pages read a machine's hostd log and a server start's log from (`GET /api/admin/logs`, [`../README.md`](../README.md) "Logs"): the deployment's `/machines` and `/servers` groups (`cloudwatch:/opennova/<deployment>`), or the local stack's directory (`file:///var/log/opennova`, its `machines/` and `servers/`). |

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
| `GET /api/me/servers` | a website session | `enabled`, `full` (the cap reached, or no fleet room in any pool the account has a free place in), the account's `entitlement` (`max_servers` and `used`, and per pool `shared_servers` and `shared_used`, `own_machines` and `own_used`), the offered `products` (each with its version, missions, limits and expansions) and its `servers` (the live ones, then its newest 10 ended), each with its settings, its `product` (`id`, `key`, `display_name` and `title`, the game's gameprofile code), pins, `update_available`, restarts, times, `listing` (its RID, name, mission, `game_type`, players and address; null while it holds none), its `phase` (where its start stands: `queued`, `restarting` n of 3 after a `crash` or a `timeout` (its `cause`), `moving`, `data_unavailable` (its last start's data could not be fetched: placed again shortly, no restart counted), `launches_held`, `launching_machine`, `waiting_for_slot`, `waiting_for_address` (its dedicated machine does not show its static address yet), `downloading_data` with its bytes so far of the total, `downloading_server`, `starting_server`, `signing_in`, or the state; `model/server_phase.*`) `novalogic` (for an owner who asked to be listed on NovaLogic's NovaWorld, while it is live: that listing's `state` as its agent last reported it, `off`, `trying`, `listed`, `retrying` or `not_listed`, with `try` and `tries` while trying or retrying, `next_try_at` while retrying, the server's `reason` while retrying or not listed, and the console `player_slots` of the players that service sent; an admin's view adds the NWEC `tag`; null otherwise, and while no report carries it) and `stopped_because` (an ended server's last run: its cause (`owner_stop`, `admin_stop` and `stop` say who stopped it), a limits stop's `limit` (`kind` and `count`; null for any other end), exit code, OOM flag and the server's own last line; never the agent's reason a start never ran, which `GET /api/admin/hosted/<id>` alone carries, there in `phase.line` too) |
| `GET /api/me/servers/<id>` | a website session | one of the account's servers with its newest 50 `events`; 404 for another account's |
| `POST /api/me/servers` | a website session | the settings (`game_name` required; `product_id`, `expansion_id` and `rotation` defaulted), `own_machine` and `address_id` (a static address of the account's, with `own_machine`): 202 `requested`; 400 `invalid_json`, `invalid_field`, `invalid_settings` (with the `field`); 403 `not_entitled`, `payment_past_due`; 409 `server_limit` (with `shared_servers` and `shared_used`), `no_own_machine` (with `own_machines` and `own_used`), `at_capacity`, `address_unavailable`; 429 (6, then one every 20 s, shared with the stop); 503 `hosting_disabled` |
| `GET /api/me/addresses` | a website session | `enabled`, the `zone`, how many bought addresses are `unnamed`, and the account's `addresses` (each with its name, hostname, state, IP and port, source, subscription, the live server using it, reason, why it lapsed and times) |
| `POST /api/me/addresses` | a website session | `{"name"}`: the account's next bought address named, then allocated by a worker: 201 `{"address"}`; 400 `invalid_json`, `invalid_field`, `invalid_name`; 404; 409 `name_taken`, `no_address`; 503 `addresses_disabled` |
| `POST /api/me/servers/<id>/stop` | a website session | 202 `stopping` (or `stopped`); 404; 409 `not_live`; 429 |
| `POST /api/me/servers/<id>/update` | a website session | `update_requested` (placed again on the current trees; players drop): 202; 404; 409 `not_live`, `up_to_date` |
| `GET /api/me/servers/<id>/stats` | a website session | `?range=1h`, `24h` (the default), `7d`, `30d`: the server now (`live`), the range's `summary` (availability, players, egress, the `approximate` fields) and `series`; 400; 404; 429 (60, then two a second) |
| `GET /api/me/servers/stats` | a website session | the account's servers together, as above |
| `PUT /api/admin/users/<id>/comp` | admin | `{"servers": 0..16, "own_machines": 0..16}`, both (shared servers, and dedicated machines added to them): 200 `{"user", "entitlement", "capacity"}` (the service's sold hosting against what the fleet sells: `shared`, `own_machines` and `addresses`, each `{sold, sellable}`, and `oversold` when a grant takes any past it; logged); 400 `invalid_json`, `missing_field`, `invalid_field`; 404 (or a service account). An entitlement down to no server stops the account's hosted servers, as a ban does |
| `GET /api/admin/hosted` | admin | the fleet: `enabled`, the provider, the cap, each kind's live servers and room, the live machines then the newest 10 ended (each with its state, cloud id, slots, the live servers on them (`slot_holders`: each slot's server and its state), addresses, agent and template versions, this month's egress, its `image_id` and `agent_log_stream`, its `stats` as its agent last reported them at `stats_at` with `net_in_bps` and `net_out_bps` between its two newest minutes, the `release`'s images, `outdated` (true, false, or null when nothing compares; `model/fleet_health.h` `machine_outdated`) and its static `address`) and the newest 100 servers (`?owner=`, `?state=live`, `ended`, `all`), each as the owner's plus its owner (its tier, and `granted` while an admin grants it hosting), service account, container, place, generation, image, pins, players and `tick` (its last minute's `p99_ms` and `late` ticks from its slot's report, null with none); never the admin password or an agent's credential |
| `GET /api/admin/hosted/<id>` | admin | one server with its `phase`, `stopped_because`, events and its machine; 404 |
| `GET /api/admin/fleet/machines/<id>` | admin | one machine for its page: `machine` (as the fleet view's, each of its `slot_holders` also with its server's `name`, `game_port` and `tick`, so the page reads no fleet list), its newest 100 `events` oldest first (`from` null at its making; a note's `to` is its `from`), its `trees` (state, bytes so far, `total_bytes` from the tree's manifest or null, tries, error); 404 |
| `POST /api/admin/fleet/machines/<id>/drain` | admin | the machine takes no new server and its servers move off one at a time (no restart counted), then it ends: 202; 404; 409 `not_live` |
| `POST /api/admin/fleet/machines/<id>/end` | admin | the machine to `ending`, its servers placed again (no restart counted), its agent refused: 202; 404; 409 `not_live` |
| `GET /api/admin/fleet/roll` | admin | a fleet roll's preview (`model/fleet_roll.h`): each machine that takes servers with its image (EC2's), its servers and `replace` (another image than `ONNET_FLEET_IMAGE_ID`, or one not known) or `keep`, and the ids to replace |
| `POST /api/admin/fleet/roll` | admin | `{"machines": [<id>, ...]}`: each drains onto machines launched for them from the templates as they are (a new image, template or user-data reaches those; hostd and the serve image reach machines in place): 202 with their ids; 400 `invalid_field` for no list, or a machine that takes no servers |
| `POST /api/admin/hosted/update-all` | admin | `{}` or `{"product_id": n}`: every live server behind its products' current trees marked `update_requested`: 202 with how many; 400 |
| `GET /api/admin/hosted/<id>/stats`, `GET /api/admin/hosted/stats` | admin | any server's stats, and the fleet's |
| `GET /api/admin/hosted/usage` | admin | every live server's use of its slot (its newest sample within 3 min and its last hour summed), by machine and slot, with each machine's and the fleet's totals and the slots' limits; 429 |
| `POST /api/admin/hosted/<id>/stop` | admin | 202; 404; 409 `not_live` |
| `POST /api/admin/hosted/stop-all` | admin | the kill switch: every live server to `stopping`: 200 with how many |
| `GET /api/admin/addresses` | admin | `enabled`, the `zone`, how many the cloud `held` of the `sellable`, every address with its owner, allocation, record, lapse cause and a failed one's `retry_at`, and the `orphans` the last sweep found |
| `PUT /api/admin/addresses/<id>` | admin | `{"name"}`: a rename (any state: a released one's frees its old name; a ready one's record moves): 200; 400 `invalid_name`; 404; 409 `name_taken` |
| `POST /api/admin/addresses/<id>/release` | admin | `{"confirm": "<its name>"}`: lapsed by an admin, then released (its server placed again without it): 202; 400 `confirm_mismatch`; 404; 409 `not_releasable` |
| `POST /api/admin/addresses/<id>/retry` | admin | an address the cloud refused allocated again (its automatic tries counted from 0): 202; 404; 409 `not_retryable` (not failed); 503 `addresses_disabled` |
| `POST /api/admin/users/<id>/addresses` | admin | `{"name"}`: a comp address for the account, at most its dedicated machines: 201; 400 `invalid_name`; 404; 409 `name_taken`, `address_limit`; 503 `addresses_disabled` |

## Products

**Products** (`apps/novaworld_web/products/`, ADR 0064): the games and OpenNova's apps as rows
(`../products/product_repository.h`, `backend/migrations/0001_baseline.sql` and
`0002_players_projects.sql`), each named by its
key, each game with its project (`../products/project_repository.h`; the editor's routes,
`apps/novaworld_web/products/project_routes.h`, push and pull its revisions by that key; an
app's key names no project), and
the releases the data program builds and validates into each game's tree (1SFS packages)
and the update listener serves. Every release of a game is built from one of its project's
revisions (`POST /api/products/<key>/releases`, its owner's or an admin's, the `release.build` job,
`../products/release_build.h`: each revision laid out in a folder of its project's key, an
expansion's base game's revision beside it and exported first, the laid-out expansion's
`base_project` pointed at it, so the folders an author keeps the projects in never matter);
an app's from its build, which Publish uploads (web begins the build's multipart upload to
`updates/source-<id>.zip`, takes each part's PUT, 32 MiB a part, inside Cloudflare's request
limits, and completes it into the release's `release.build`): an updatable app's (the
runtime's, the editor's) unpacked (`../core/zip_files.h`, a name that would leave the folder
refused), its `OPENNOVA.WIZ` written from its row (`CMD` its command file, `URL`
`ONNET_UPDATE_HOST`, refused with none, `PRODUCT_LOGIN` its login) and packed whole against its
published release, every file of the build a row, then validated as a game's is; a
download-only app's (the tools', the Blender add-on's) kept as uploaded under
its digest (`updates/<sha256>.zip`), a draft with no section, which publishing makes its own
product's. Every release is whole, so
`release.apply` builds a game's tree from nothing out of its published release alone (an
expansion's its own, beside its base game's): no tree is ever uploaded. Publishing or retiring
a release also queues `downloads.publish` (`../products/downloads_job.h`), which writes the
downloads host (`ONNET_DOWNLOADS_URL`) from the published releases: each product with a
`download` rendered under that name (a game's the runtime's package applied with its
`ONRUNTIME.CMD`, a retail game's or an account's own public game's `UPDATE.WIZ` (the latter's
naming its title, `GAME=`) and its expansions' of the service's own
`expansion/<code>/UPDATE.WIZ`, each naming `ONNET_UPDATE_HOST`; the editor's its own package
with its `ONEDITOR.CMD`; a download-only app's its build as uploaded), then `manifest.json`
(the service's products and the accounts' public games with a published release, each one's `key`,
`kind`, `name`, `description`, `file`, `size`, `sha256`, `version`, `release`, its release's
name, and `published`, the file's fields null while it offers none), then every other object
removed. The jobs a route
enqueues (`release.build`, `release.validate`, `release.apply`, `tree.prune`, `product.purge`, `downloads.publish`; the service
README's "Background jobs") commit with the state move that causes them, and the data program
(`apps/novaworld_data`) alone runs them. Every route below is the admin's, each state change
logged with its actor; without a store the store routes answer 503 `store_unavailable`, and a
store call that fails mid-way 502 `store_error` (its own text to the log alone). The shapes
(`Product`, `Tree`, `Release`) are `apps/novaworld_web/products/`'s.

**An account's own projects** (ADR 0069, ADR 0074). Any signed-in account may make projects of
its own (a mod: an expansion of a game of the service's own, public or private, or of its own
game; or a game that speaks `jo` or `dfx2`), push to them from the OpenNova Editor and release
them; its owner's publish waits for an admin's approval (`awaiting_approval`: nothing serves,
applies, renders or lists it, and its owner tests it from the editor's Play meanwhile), which an
admin's publish gives and an admin's reject sends back to draft with a `review_note` saying
why; an admin's own projects publish at once. The row's `owner_id` names it (NULL for the service's own,
every seeded row and every one an admin makes). One rule, every project route's
(`../products/project_repository.h` `project_rights`): a project is read by anyone when public,
else by its owner and the admins alone (404 `not_found` to anyone else, the same as no
project); pushed and released by the own session of its owner or an admin (never the Bearer
token); renamed, made public or private and deleted by its owner or an admin (403 `forbidden`
to a reader who may not). An admin's state change through these routes on a project not its own
is logged with its actor, as an admin route's is. Its key is at most 11 characters with no `QQ`,
and names its updater product (`updater_login` KEY, `command_file` KEY.CMD, given at its
create; a key another product's updater or download already goes by is 409 `duplicate_key`)
and, a game's, its download (`player-<key>-game-windows.zip`, offered while public with a
published release). An account holds at most 3 projects (`kOwnedProjectsMax`) and 10 GiB in
their stores and their releases' packages together (`kOwnedBytesMax`: its projects'
`project_blobs` and its releases' `bytes`), whoever pushes to them; an admin's own take neither
limit. A file its store got through its own URL is counted before it is held (past the quota,
413 and not held), and one the store holds at another size than named is removed. An account
that is no admin has one release building or validating at a time (409
`release_in_progress`), starts at most 5 a UTC day (429 `too_many_releases`, `release_starts`:
a deleted one still counts) and none while its usage is at the quota (413 `quota_exceeded`).
A game selects an existing compatible lobby or names a new one when created. A player's
choices are the lobbies of system games, public games with a published release, or the
caller's games, never a retail game's (Joint Operations' and Delta Force Xtreme 2's, which the
original `Jointops.exe` and `dfx2.exe` browse); an admin may select any lobby of the game's
title. An unpublished lobby remains reachable for testing but is unlisted publicly. Starting the
first release build of a game with a lobby locks the selection, including when a mod first
builds on the game; failed or deleted releases do not unlock it, and admins obey the same lock
(a locked game with no lobby still takes one). A mod inherits its base game's lobby. Sharing a
lobby grants no management rights, and deleting a project retains the lobby identity; a lobby a
game created goes when the game moves off it and no other game names it. The service generates
`gt.ssc` for every release from this configuration; project imports and pushes exclude that file.
A published mod is installed in game (`GET /api/mods`); private means unlisted, not
secret: the update service serves any published product to whoever has its wizard. Its
delete, and the account's (the admin's `DELETE /api/admin/users/<id>`, which deletes its
projects first and refuses 409 `in_use` or `has_expansion` while one may not go), queue each
project's `product.purge` as the admin's delete does (`../products/product_jobs.h`
`delete_product_purged`), its releases going with it. The routes are
`apps/novaworld_web/products/project_routes.h`'s (each on the account's own session, a state
change with the CSRF header):

| Route | Replies |
| --- | --- |
| `GET /api/products` | the projects the caller may read (the public ones, its own, every one for an admin), by key, each `{key, title, kind, game, expansion_code, visibility, retail, base, product_id, head, owner, can_push, can_manage, can_release, release, lobby, lobby_locked}` (`release` its published release's `{id, version}`, null for none; `lobby` the resolved `{slug, display_name, title, gate_tag_file}`, null if unconfigured, with generated file bytes in base64; mods inherit both lobby fields) and, for one it may manage, `storage` |
| `POST /api/products` | `{key, display_name, kind, game?, base?, expansion_code?, visibility, lobby?, new_lobby_name?}`: a game (`game` a title) requires exactly one existing `lobby` slug or `new_lobby_name` (1 to 64 visible characters); an expansion (`base` a game of the service's or the caller's own, `expansion_code` as the admin's) inherits its lobby and accepts neither field. The caller owns it; its key is 1 to 11 of `A-Z0-9_` with no `QQ`: 201 `{project}`; 400 `invalid_field`, `missing_field` (with the `field`); 403 `too_many_projects` (with the `limit`); 404 `no_such_base`; 409 `duplicate_key`, `duplicate_expansion` (`field` `expansion_code`: another mod, of any game, has the code); 429 (5, then one a minute) |
| `PATCH /api/products/<key>` | `{display_name?, visibility?, lobby?, new_lobby_name?}` by its owner or an admin; the mutually exclusive lobby fields change a game's selection before its first release build: 200 `{project}`; 400; 403 `forbidden`; 404; 409 `lobby_locked` |
| `DELETE /api/products/<key>` | by its owner or an admin, its objects' `product.purge` queued: 204; 403 `forbidden`; 404; 409 `in_use`, `has_expansion` |
| `GET /api/me/projects` | the account's own `projects`, their `usage` (`projects`, `bytes`: their stores and their releases' packages, `release_bytes`: the packages alone) and the `limits`, the `games` a mod of its may build on (`{key, title, game}`: the service's and its own), the `titles` a game of its may speak, and the `lobbies` it may select (`{slug, display_name, title}`; a player's never a retail game's, an admin's every one; selection must match the game's title) |
| `POST /api/products/<key>/missing` | 413 `quota_exceeded` `{used, limit}` when an account's own project's files to upload, or those its store got through its own URLs, would take its projects past 10 GiB (none is signed or held then); the directory store's `PUT .../blobs/<sha256>` and the push alike |
| `GET /api/products/<key>/releases` | its releases, newest first, each as the admin's (`Release`), for its owner or an admin: 200 `{releases}`; 403; 404 |
| `POST /api/products/<key>/releases` | `{version, revision?, base_revision?}`: a release built from the revision (the head when none is named) and, an expansion's, its base game's (its head), `created_by` `user <name>` or `admin <name>`: 201 `{release}`, `building`; 400 `invalid_field`, `no_revision`; 403; 404; 409 `release_in_progress`; 413 `quota_exceeded` `{used, limit}`; 429 `too_many_releases` `{limit}` with `Retry-After` (the seconds to the next UTC midnight); 503 (an admin's start takes none of the three limits) |
| `POST /api/products/<key>/releases/<id>/publish` | by an admin, one of its releases published as the admin's route does (a draft or one awaiting approval); by its owner who is no admin, a draft made `awaiting_approval` for an admin's approval, its `review_note` cleared, nothing served, applied or rendered: 202 `{release}`; 403; 404 (no release of this project's); 409 `not_draft` |
| `POST /api/products/<key>/releases/<id>/retire`, `DELETE /api/products/<key>/releases/<id>` | one of its releases retired or deleted (one awaiting approval too: its owner's withdrawal), by its owner or an admin, as the admin's release routes do: 202 `{release}` or 204; 403; 404; 409 as the admin's |
| `GET /api/mods?game=<command file>` | anyone's: `{mods: [{key, code, name, owner, version, updater_login, command_file}]}`, the expansions with an updater product and a published release whose base game's command file is `game` (without case), the public ones and the caller's own, by key (`owner` null for the service's own); 400 `invalid_query` with no `game` |

| Route | Replies |
| --- | --- |
| `GET /api/admin/products` | the store's kind, the titles, every lobby (`games`: `{slug, display_name, title}`) and every product (games, expansions, then apps), each with its `key`, its project's `visibility` (null for an app) and `head`, its `owner_id` and `owner` (the account whose own project it is, null for the service's own), its `lobby_locked`, `wizard_login`, `download` and `description`, and its trees |
| `POST /api/admin/products` | a game (a title, a `lobby`, a `games` slug) or an expansion (a game's `parent_id`, an `expansion_code` of 1 to 8 of `a-z0-9`, unique across every game without case), a `display_name`, the updater's `updater_login` and `command_file` both or neither (each unique without case), `retail` (a retail game: its SERVER.LST names `ftp.novaworld.com` instead of `ONNET_UPDATE_HOST`, and its project's revision 1 is the stock install), and its `key` (1 to 16 of `A-Z0-9_`) and its project's `visibility` (`public`, `private`), both required: 201; 400; 409 `duplicate_login`, `duplicate_command_file`, `duplicate_key`, `duplicate_expansion`, `no_such_parent` |
| `PATCH /api/admin/products/<id>` | any product's (an account's own too, never made `retail`): the name, a game's compatible `lobby` before its first release build, the updater's fields, the `key` (a rename, of its row alone: the store keeps a project's files under its product's id, `content/<id>/`, so none moves; editors find a project by its key, so a project file naming the old key reads 404 `not_found`) and a game's `visibility` (an app has no project): 200; 400; 404; 409 `duplicate_login`, `duplicate_command_file`, `duplicate_key`, `lobby_locked` |
| `DELETE /api/admin/products/<id>` | any product (an account's own too): the game, its trees, its releases and its project's history, at once; its objects (its trees' prefixes, its project's files under `content/<id>/` and its releases' packages) removed by the data program's `product.purge`, queued in the delete's transaction to run 75 minutes after it (past every presigned transfer handed out before and the jobs then on its trees): 204; 404; 409 `in_use` (a live server runs it), `has_expansion` (an expansion builds on it) |
| `GET /api/admin/releases` | the releases, newest first, each naming its product (`product_id`), its `version`, its `review_note` and, for a game's, its `source` revision |
| `POST /api/admin/releases` | `{"name"?, "size", "product_id", "version"}`: an app's release `uploading` (`version` one to four numbers between dots) and its build's multipart upload: 201; 400; 409 `built_from_project` (the product is a game); 503 |
| `PUT /api/admin/releases/<id>/parts/<n>?upload_id=<u>` | a part (at most 32 MiB) handed to the store: 200 `{"etag"}`; 400; 404; 409 `not_uploading`, `built_from_project`; 413; 502; 503 |
| `POST /api/admin/releases/<id>/complete` | the upload completed, then the release `building` with its `release.build` job: 202; 400; 404; 409 `not_uploading`, `built_from_project`; 502; 503 |
| `POST /api/admin/releases/<id>/publish` | a draft or a release awaiting approval published (the release each section's product named before, or a sectionless release's own product's, retired once none names it; its `review_note` cleared) with one `release.apply` job per section of a game with a tree, and `downloads.publish`: 202; 404; 409 `not_draft` |
| `POST /api/admin/releases/<id>/reject` | `{"message"?}`: a release awaiting approval sent back to `draft`, the message (trimmed, at most 500 characters, no control character but a line break or a tab) its `review_note`: 202 `{release}`; 400 `invalid_field`; 404; 409 `not_awaiting`. Each release awaiting approval is a warn finding of `GET /api/admin/attention` (source `releases`, its name, "Awaits approval (by <owner>)", linked to `/admin/products/<key>`) |
| `POST /api/admin/releases/<id>/retire` | a published release retired, its products serving none, and `downloads.publish`: 202; 404; 409 `not_published` |
| `DELETE /api/admin/releases/<id>` | a draft, invalid, retired, awaiting approval or abandoned release and its objects (an app's uploaded build, its package or download; a tree built from it keeps its row): 204; 404; 409 `published`, `validating`, `building`; 502; 503 |

The seeded products are the games `JO`, `DFX2` and `ACIDGAMBIT`, the expansions `JOX01` and
`NIFTY`, and the apps `ONRUNTIME`, `ONEDITOR`, `ONTOOLS` and `ONBLENDER`, ids 1 to
9 in that seeded order (the first six as before the apps). Their updater products are
`ONRUNTIME` / `ONRUNTIME.CMD` (OpenNova's runtime), `ONEDITOR` / `ONEDITOR.CMD` (the OpenNova
Editor), `ACIDGAMBIT` / `ACIDGAMBIT.CMD` (OP: Acid Gambit), `NIFTY` / `NIFTY.CMD` (its `nifty`
expansion), `DFX2` / `DFX2.CMD`, `JOTRus` and `JOEus` (their stock wizards' `JOTRqq` and `JOEqq`,
`wizard_login`); the tools and the Blender add-on are download-only apps, with no
updater product. Every game and expansion has a project (`ACIDGAMBIT` and `NIFTY` public, `JO`,
`JOX01` and `DFX2` private: a retail game's revision 1 is its stock install imported afresh), an
app none. A product's `download` is its file's stable name on the downloads host (the three
games', the editor's and the two download-only apps'). The runtime is an `app` row: it
appears among the products with its releases alone, has no hosted game tree and is no
hosted-server choice. Every app's releases arrive from CI: the Publish workflow
(`.github/workflows/publish.yml`, its helper `scripts/publish_apps.py`) uploads each app's
build through the routes above with the deployment's admin API token, naming the app and the
version, the service builds each release from it, and Publish publishes them all once every one
is a draft. A release of the same version as an earlier one is taken like any other; only the
same bytes again are refused ("already uploaded"), so Publish leaves a download-only app whose
zip is its published release's as it is.

## The update listener

**The update listener** (`apps/novaworld_web/updater/`, ADR 0061 d3, ADR 0064 d9):
UPDATE.EXE's gateway and update server in one, as retail's was, over the published
releases, started by web when `ONNET_UPDATE_HOST` is set and the object store is there. A
thread of its own pumps `net/update`'s connection machine on `ONNET_UPDATE_PORTS`, watched
by the stall watchdog; a second refreshes its catalog (`release_catalog.*`) from the rows
every 30 s: each product with an updater login serves the section of its published
release that carries its command file, the package fetched into the cache and the section
unpacked beside it, streamed from disk (a connection's next replies are made only while
less than 64 KiB wait unsent). `SERVER.LST` names one host: `ftp.novaworld.com` for a retail
game's product (its `retail` column: a retail install reaches it
through a hosts-file line), else `ONNET_UPDATE_HOST`. A login no published release
serves is refused when it is OpenNova's own (not retail), so a first install fails rather than finding
nothing; a retail product (`JOTRus`, `JOEus`, `DFX2`) with none is served as current, from
a package of its command file alone that the catalog writes into its cache. Logged under
`[update]`.

| Setting | Default | Meaning |
| --- | --- | --- |
| `ONNET_UPDATE_HOST` | none | The update host `SERVER.LST` sends a client of this deployment to (`update.<domain>`); unset, web runs no update listener. |
| `ONNET_UPDATE_PORTS` | `20044,20556,21068,21580` | The gateway ports, comma-separated (retail's four, which UPDATE.EXE tries in turn); a list that does not read is a boot FATAL. |
| `ONNET_UPDATE_CACHE_DIR` | a scratch directory | Where the packages and their unpacked sections are kept between refreshes. |

## The console routes

**The console routes** (`console_routes.*`, `configuration_routes.cpp`) run structured and raw commands on a hosted server's
admin link, which the fleet program holds: a route writes its command to
`hosted_console_commands` with the deadline its budget allows and reads its row until it
is done; the fleet's console drain (`apps/novaworld_fleet/hosted/console_drain.*`) claims the queued commands
(`FOR UPDATE SKIP LOCKED`), runs each on its link and writes the outcome and each reply
packet (`hosted_console_replies`); the route reads them and deletes the row. A command
still queued when the route's wait ends is withdrawn, never sent. A claimed command keeps running after the HTTP deadline; an uncertain result is explicitly marked `delivery_unknown` and is never replayed. A new leader also marks unfinished work uncertain. The owner's routes act on the caller's own servers by id (another
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
| `POST /api/me/servers/<id>/rotation` | `MISSION LIST`, then `MISSION ADD <file> <0\|1>` | `{"mission", "swap_sides", "insert_at", "one_shot"}`; omitted position appends; 400; 404 `mission_not_found`; 409 `rotation_full` (128 maps, policy), `not_in_game` |
| `POST /api/me/servers/<id>/rotation/<index>/remove`, `/next` | `MISSION LIST`, then `MISSION REMOVE` / `SETNEXT <index>` | 400 `invalid_index`; 404 `no_such_entry` (the server answers OK for any index, so the rotation is read first); 409 `last_mission` (an empty rotation ends the session), `not_in_game` |
| `POST /api/me/servers/<id>/rotation/cycle` | `MISSION CYCLE` | the round ends now; 409 `not_in_game` |
| `GET /api/me/servers/<id>/chat` | `CHAT GET` | the chat window, oldest first |
| `POST /api/me/servers/<id>/chat` | `CHAT SEND <text>` | `{"text"}`: cp1252, no control character, not blank, at most 59 characters; 400 `invalid_text` |
| `GET /api/me/servers/<id>/console` | none | the console ring in any state, with the link's state (`?limit=`, `?after=`); 404 |
| `GET .../settings` | `GET GAMESETTINGS` | live values, saved configuration, revision, title schema and pinned weapon catalog |
| `PATCH .../settings` | supported `SET` and `WEAPON SET` commands | `{settings_revision, patch}`; file-only changes wait for the next launch |
| `GET .../weapons` | `WEAPON LIST` | complete reply packets |
| `POST .../command` | `{command}` | full ordered replies, delivery status, saved status and new revision |

Owners and admins can use the full TCP command surface on hosted servers, including
bulk player actions, weapon restrictions, inserted/one-shot missions, `CMD`, `GOTO`,
and `QUIT`. `ADMINUSER`, `BANLIST`, and `SET LevelRestrict` remain retail stubs;
the web shows the server's unsupported response. The console never connects to
arbitrary external endpoints. Authentication, ownership, CSRF, admission and rate
limits still apply. Password-bearing lines are redacted in persistent logs.

## Saved server configuration

`engine/formats/gamecfg/server_settings.*` describes the title-aware fields, defaults,
types, validation and live setters shared by the website and in-game host editor. It
offers only the settings something in the runtime acts on (a game.cfg key nothing reads
stays in the format, not the schema), and a row's `depends_on` names the toggle or count
it does nothing without, which the editors show it beneath.
NovaWorld and LAN use the same authored host screen, with a supplemental options panel.

`hosted_servers.configuration` is the canonical JSONB document. Existing core fields
remain at the top level, advanced fields live under `options`, and `rotation` is an
ordered array of `{file, swap_sides}` entries, including repeated maps. A map's side-switch
flag applies to every occurrence, as in retail. Classes and
weapons use stable names; weapon availability is `always`, `never` or `armory`.
A product tree carries its weapon catalog, so published updates cannot reinterpret
an already running server's restrictions by another tree's numeric indexes.

The fleet serializes queued operations per server and fences them by process generation
and configuration revision. It saves accepted live changes and command completion in
one transaction, retries database persistence without resending TCP commands, and
finishes independently of the browser. Structured edits require the current revision;
stale edits return 409. File-only options are saved for the next launch without forcing
a restart. One-shot missions are excluded from saved rotations. QUIT closes only the
admin session; exhausted non-repeating rotations and explicit session ends release the
allocation without consuming a crash restart.

Deployment requires `0005_server_configuration.sql`. It migrates existing settings
and rotations, removes their old columns, and queues catalog backfills for ready trees.
Deploy the matching service binaries together with that schema (old binaries cannot
use the removed columns); update the serve/hostd runtime and fleet before exposing the
new web UI. Let `tree.server_catalog` jobs finish before configuring weapons on existing
trees. The migration preserves existing servers' repeat-rotation behavior.
`0006_server_options_honored.sql` then drops from saved options the keys the schema no
longer offers (the settings nothing in the runtime reads, and `lan_mode`, which the
hosted NovaWorld channel ignores) and turns a numeric `listing_ping` into its toggle.
