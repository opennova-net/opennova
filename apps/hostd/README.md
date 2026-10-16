# opennova-hostd

The machine agent of the hosted servers' fleet (pre-1.0, experimental). Each machine the
NovaWorld service launches runs one `opennova-hostd`, as root under systemd. It fetches
the game data trees the service's replies name (ADR 0064), starts the `opennova-serve`
containers the service places on the machine's slots, stops the ones the service no
longer wants, and reports every container's state, CPU, memory and its slot's egress
back. It never decides anything on its own: the service places
servers, pins their trees, counts restarts and ends machines; the agent does what the
last reply said and keeps doing it while the service cannot be reached.

Linux only. Target `opennova_hostd`, binary `opennova-hostd`, built from `apps/hostd` with
the service's half of the protocol (`apps/novaworld_common/hosted/agent_protocol.*`,
`slots.h`) compiled in. It ships inside the `opennova-serve` image at
`/usr/local/bin/opennova-hostd` (`apps/serve/Dockerfile`). On a cloud machine it never runs
in a container: the machine copies it out of the image it pulled at its bootstrap, so each
machine's agent is the build of the serve image it was launched with. The local stack's
machines are themselves containers of that image, the agent their own process, with the
process runtime (below).

## Vocabulary

A **machine** is the EC2 virtual machine; a **shared machine** packs up to K servers, an
**own machine** runs exactly one (the paid add-on). A **slot** is one server's position on a
machine (`hosted/slots.h`): slot k is the game's UDP port 32768 + k (open to everyone), the
remote admin's TCP port 32800 + k (open to the service alone), the container's user
10001 + k and the working directory `/srv/slots/slot-k` (0700, that user's). At most 32
slots; an own machine has slot 0 alone. A **tree** is one immutable version of a data
set's files in the object store (CONTEXT.md "Tree"): a server is pinned to a base tree
(stock JO, DFX2) and, with an expansion, to an expansion tree (Escalation's), and the
machine holds each tree it needs at `/opt/game/<tree id>/`.

## Command line

```
opennova-hostd --service http://<host>:<port> --machine <id> [options]
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--service` | required | The controller's internal listener as the machine reaches it (`ONNET_INTERNAL_URL`: the deployment's private name for the controller, resolved at each request, so the agent follows the controller to a new instance; each request tries every address the name lists until one takes the connection, since during a controller's replacement it lists the standby, which does not listen, beside the leader), `http://` only. |
| `--machine` | required | This machine's id, the service's `fleet_machines` row. |
| `--token-file` | `/etc/opennova/hostd.token` | The one-time bootstrap token the machine's user-data wrote (0600). Deleted once redeemed. |
| `--state-dir` | `/var/lib/opennova-hostd` | The saved bootstrap (`machine-<id>.json`, 0600) and `scratch/` (the CLIs' output files, a pull's error file, a start's environment file). Made 0700. |
| `--slots-root` | `/srv/slots` | The slots' working directories. Made 0755 when missing. |
| `--runtime` | `docker` | How the servers run: `docker`, each a docker container (a cloud machine); `process`, each an `opennova-serve` child of the agent (a machine that is itself a container, the local stack's; "The process runtime" below). |
| `--trees-root` | `/opt/game` | The trees' directories (`<root>/<tree id>/`, `.partial` while one fetches). Made 0755 when missing; the machine's unit names it. |
| `--docker` | `/usr/bin/docker` | The docker CLI, run as given (no `PATH` search). |
| `--serve` | `/usr/local/bin/opennova-serve` | The process runtime's `opennova-serve`. |
| `--aws` | `/usr/local/bin/aws` | The AWS CLI a tree's `s3 sync` runs, as given. |
| `--iptables` | `/usr/sbin/iptables` | The egress counters' iptables, or `none` to count nothing. |
| `--ip6tables` | `/usr/sbin/ip6tables` | The same for IPv6, used only where the kernel has IPv6; `none` skips it. |
| `--log-group` | none | With `--log-region`, the docker runtime's: the CloudWatch log group the containers' output goes to, a stream per slot ("Logs" below). Unset, the machine's journal. |
| `--log-region` | none | That log group's region. |
| `--once` | | One pass, then exit: 0 when the sync was answered and read. |
| `--verbose` | | Debug lines too. |
| `--version` | | The build (`OPENNOVA_HOSTD_VERSION` at configure time; the serve image passes the commit). |

The agent reads no environment variable. The CLIs it runs get the system's variables alone
(`PATH`, `HOME`, `LANG`, `LC_*`, `TZ`, `TMPDIR`, `DOCKER_*`, and `AWS_*` for a developer's
profile and region; on a machine the AWS CLI's credentials are the instance role's).

The unit the machine's user-data installs (`hosted/machine/machine_user_data.sh`; its
`ExecStartPre` puts the slots' users' fences back in place at every start of the agent):

```
[Unit]
Description=opennova-hostd, the hosted servers' machine agent
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
ExecStartPre=/usr/local/sbin/opennova-machine-fence
ExecStart=/usr/local/bin/opennova-hostd --service http://controller.prod.opennova.internal:8070 --machine 12 --trees-root /opt/game --aws /usr/local/bin/aws --log-group /opennova/prod/machines --log-region us-east-1
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Restarting the agent (or its unit) never touches a container: they are docker's, not the
agent's children.

## The protocol

Both routes are on the service's internal listener and answer every refusal with a bare
404. The message shapes, their checks and their caps are
`apps/novaworld_common/hosted/agent_protocol.h`; the agent and the service build the same
encoders and strict decoders.

**Bootstrap**, once per machine:

```
POST /hosted/machines/<id>/bootstrap      Authorization: Bearer <one-time token>
{"protocol": 2, "agent_version": "<build>"}
-> 200 {"protocol": 2, "agent_secret", "serve_image", "slots"}
```

The reply is saved owner-only at `<state-dir>/machine-<id>.json` and the token file deleted,
so a restarted agent never needs the token again. The service spends the token at the
agent's first sync, not at the redeem: a redeem whose reply was lost (or did not read) is
made again, and the fresh secret it gets replaces the first. Without a saved reply and a
token, or with the service out of reach, the agent tries again every 10 s; a token the
service refuses (spent, lapsed, unknown, a machine no longer booting) is logged and tried
again twice as late each time, up to 5 minutes. The service sees a machine that never
reports and ends it.

**Sync**, every 5 s, every 1 s while the agent has work (a container to start, one stopping,
one to remove, a tree to fetch):

```
POST /hosted/machines/<id>/sync           Authorization: Bearer <agent secret>
{"protocol": 2, "agent_version", "containers": [{"name", "slot", "state", "exit_code",
  "oom", "started_at", ["reason",] "cpu_percent", "memory_bytes", "egress_bytes"}],
  ["listing_failed": true]}
-> 200 {"protocol": 2, "drain", "desired": [{"name", "slot", "image", "uid", "base_tree",
  "expansion_tree", "expansion_code", "args", "files", "env", "cpus", "memory_mb", "pids"}],
  "trees": {"<tree id>": {"url", "version", "files": [{"name", "size"}]}}}
```

`state` is `starting`, `running` or `exited`; `started_at` Unix seconds; `reason` rides a
container that never started (exited with no start) when the agent knows why (its data
could not be fetched: the owner reads it in the server's events); `egress_bytes` the
slot's count since the machine booted (a lower count than the last is a counter that started
over). `files` (each file's bytes in base64) is sent until the agent has reported the
container once; `uid` is always the slot's. The reply's `trees` names every tree its
desired containers use, with where to fetch it (`url`: `s3://<bucket>/<root>/trees/<id>/`,
or `file:///` on a developer's machine) and its manifest (each file's name and size);
`expansion_tree` is null and `expansion_code` `""` for a server without an expansion.

## What a sync does

The agent lists its containers (`docker ps` on its label, then `docker inspect`), reads the
running ones' CPU and memory (`docker stats --no-stream`, at most every 5 s) and each slot's
egress, posts the report, fetches what the reply's trees
name and the store lacks (below) and acts on the reply (`reconcile.h`, a pure function):

- A desired container that runs, runs on. One that exited stays, reported with its exit
  code and OOM flag, until the reply drops it: the service decides a restart (a new
  container name, `srv-<id>-g<generation>`), never the agent.
- A running container the reply leaves out gets SIGTERM (`opennova-serve` deregisters from
  NovaWorld), and SIGKILL if it still runs 60 s later; once it is not running it is
  removed, and its slot's directory with it (the credentials go).
- A desired container the machine lacks starts once its slot is free: never while another
  container, of any state, still holds the slot, so a slot's next server never meets its
  previous one's port, user or files. Its directory is written from scratch (0700 for the
  slot's user, each file 0600, opened with `O_EXCL` and `O_NOFOLLOW`), its environment
  goes in a 0600 file that lives for the `docker run` alone, and at most 4 start in a pass.
- A desired container that cannot start here is logged once and not reported: its files
  were not sent (the agent lost its state), or its slot is past the machine's.
- A start that fails (the image did not pull, its data could not be fetched, a mount is
  missing) is reported `exited` with docker's exit code (125) and, when its data could not
  be fetched, the fetch's reason (`reason`), its leftovers and files removed, and never
  retried.

A start never pulls (`docker run --pull never`). A desired container whose image the
machine lacks (`docker image inspect`) waits for a `docker pull` the agent runs in the
background, one per image however many containers want it: it is reported `starting` on its
slot meanwhile (so the service stops sending its files, which the agent keeps), and starts
at the first pass after the pull. A pull that fails, or still runs 4 minutes after it began
(it is then ended), is that start's failure: `exited` 125. The machine's bootstrap pulls the
serve image it copies the agent out of, so only a server pinned to a newer image than its
machine's waits for one.

A desired container whose base or expansion tree the machine lacks waits the same way for
the tree's fetch (`tree_store.h`): the store fetches one tree at a time, in the
background, into `<trees root>/<id>.partial/` (`aws s3 sync <url> <dir> --no-progress
--only-show-errors` as a child of its own; a `file:///` URL is a directory copy), then
checks every file of the manifest is there with its size (an interrupted sync is caught;
the sync itself is trusted for the bytes), removes any other file but `manifest.json`,
renames the directory to `<trees root>/<id>/` and marks it with a `.ready` file. A fetch
or a check that fails leaves the tree `failed` with the reason, and the store tries again
a minute later while a reply still names the tree, 3 tries in all: the containers wanting
it keep waiting meanwhile (a passing S3 failure costs a minute, not one of the server's
restarts; the service's start timeout allows for the three), and only once the tries are
used up do their starts fail (`exited` 125, the last reason as the report's `reason`),
the tree staying failed until no reply names it. A present tree no reply has named for 30 minutes is removed, so
a machine holds the trees of the servers it ran lately and nothing else; a restarted
agent finds its marked trees again and removes an unfinished one.

No pass holds the agent up for long, since the service ends a machine whose agent is silent
for a minute and more (`hosted/supervisor.h`): every docker call is cut at 20 s, the listing
and the stats before a sync share 15 s, and the reply's acts 10 s; a stop, removal or start
the act's time does not reach waits for the next pass, which comes at the busy cadence.

A sync that fails changes nothing on the machine: the service down or refusing, a reply
that does not read or speaks another protocol. Docker not answering the listing is no
silence: the pass still syncs, with `"listing_failed": true` and no container, so the
service keeps the slots' last reports and does not take the machine for lost, and the agent
acts on nothing of the reply. The containers keep running as they are and the agent keeps
trying, logging each warning when it changes and every 5 minutes while it lasts.

## The containers

```
docker run --pull never -d --name <name> --label opennova.hostd=1 --label opennova.slot=<k>
  --network host --user <uid>:<uid> --cap-drop ALL --security-opt no-new-privileges
  --read-only --tmpfs /tmp:size=64m,nosuid,nodev,noexec --cpus <cpus> --memory <mb>m
  --memory-swap <mb>m
  --pids-limit <pids> --stop-timeout 60 <the logs, below>
  --mount type=bind,source=/opt/game/<base tree>,target=/opt/game,readonly
  [--mount type=bind,source=/opt/game/<expansion tree>,target=/opt/game/expansion/<code>,readonly]
  --mount type=bind,source=/srv/slots/slot-<k>,target=/srv/serve
  [--env-file <file>] <image> <opennova-serve's arguments>
```

The machine's own network is required: NovaWorld lists a server at the source address and
port of its datagrams, and a bridge's NAT would rewrite the port. The base tree is the
container's `/opt/game`; its expansion tree, when it has one, sits inside it at
`/opt/game/expansion/<code>`, where `opennova-serve` looks for an expansion, and the agent
makes that mount point under the base tree on the host before the start (a read-only bind
cannot have one made in it). Only containers with the
agent's label are ever listed, stopped or removed.

## Logs

On a cloud machine the servers' output and the agent's own log go to the deployment's
machines log group, `/opennova/<deployment>/machines` (`infra/aws`, 30 days), one stream
per slot and one for the agent, by the machine's id (the service's `fleet_machines` row,
the id the admin fleet view shows beside each server's slot):

| Stream | What |
| --- | --- |
| `<machine id>/slot-<k>` | slot k's container output, standard output and error: each server on the slot in turn |
| `<machine id>/hostd` | the agent's own journal: its lines, its unit's starts and exits (systemd's) |

A slot's containers log through docker's `awslogs` driver: `--log-group` and `--log-region`
(the user-data passes both) make the start's log options

```
--log-driver awslogs --log-opt awslogs-region=<region> --log-opt awslogs-group=<group>
--log-opt awslogs-stream=<machine id>/slot-<k> --log-opt mode=non-blocking
--log-opt max-buffer-size=4m
```

Non-blocking, so CloudWatch slow or out of reach never holds up a server's write (lines past
the 4 MB buffer are dropped) or its start (docker makes the stream in the background). The
docker daemon writes with the machine role's credentials (which reach that log group
alone); the slots' users are fenced off the metadata
service and never hold them. Docker also keeps a local copy (its dual logging), so `docker
logs <name>` reads a container's output on the machine (a Session Manager shell) until the
agent removes the container; CloudWatch keeps it after. Without `--log-group` (a
development machine) the output goes to the journal: `--log-driver journald --log-opt
tag=<name>` (`journalctl CONTAINER_NAME=<name>`).

The agent's own lines go to the journal (its unit's standard error), and the user-data's
`opennova-hostd-log.service` ships them with the same driver: `journalctl
--unit=opennova-hostd.service --follow` (from its cursor, this boot's start the first time)
piped into a container of the hostd image that copies its input to its output (`cat`, no
network, no capability, user nobody), which docker ships to `<machine id>/hostd`. The
unit restarts with docker (and every 5 s while docker is down); stopped, journalctl
writes its cursor, so the next start sends what came after it. A relay whose container
ended alone stops without one and sends again from the last cursor (or the boot's start),
so lines may come twice. On the machine, `journalctl -u opennova-hostd` reads it all.

Reading them needs no shell on the machine: `./deploy/run.sh --deployment <deployment> fleet
logs <machine id> [<slot>|hostd] [--since 1h] [--follow]` (DEPLOY.md, "Hosted game
servers").

## The process runtime

`--runtime process` (`process_runtime.h`) runs each server as a child process of the agent
rather than a docker container: the runtime of a machine that is itself a container and runs
no docker, the local stack's (`apps/novaworld_common/hosted/docker_machine_driver.h`, which
starts the serve image with `--runtime process --iptables none --ip6tables none`). A start
is `<--serve> --resource-dir <the data set's mount path> <opennova-serve's arguments>` in the
slot's directory, its environment the system's variables (`PATH`, `HOME`, `LANG`, `LC_*`,
`TZ`, `TMPDIR`) and the env file's, its output where the agent's own goes (the machine
container's log). The rest of the agent is the same: the bootstrap, the syncs, the slots'
directories, the starts, stops (SIGTERM), kills (SIGKILL) and removals the reply asks for,
each server reported by name with its exit code (one a signal ended reads 128 plus the
signal) until the reply drops it. What a container gave and a process does not: no slot user
(the agent's own; the uid, CPUs, memory and pids cap are not applied), no egress counters,
no CPU or memory figure (0), and every image is the machine's own (never pulled). The
servers end with the agent (the parent-death signal), so a machine whose agent ended runs
nothing, as its container ends with it.

## Egress

A chain of its own, `OPENNOVA-EGRESS`, that `OUTPUT` jumps to first, opens with one rule per
private destination (`-d <range> -j RETURN`: 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16,
169.254.0.0/16 and 127.0.0.0/8; for IPv6 fc00::/7, fe80::/10 and ::1), then holds one rule
per slot (`-m owner --uid-owner <10001 + k> -j RETURN`), so each packet a slot's user sends
to the internet is counted once and then meets the rest of `OUTPUT` (the metadata fence
among it). What goes to the metadata service, the service's internal port or its admin
links, and so all the fences refuse, is never counted. The agent puts the chain, the jump
and each rule in place only where missing (checked with `-C`; a private destination's
inserted at the chain's head), so the counters survive its restarts and count since the
machine booted; it reads them with `-L -n -v -x` every pass and puts them back (counting
from 0) when a rule went missing. Every call waits up to 5 s for the xtables lock docker
also takes.

## What the machine provides

The machine's user-data (`apps/novaworld_common/hosted/machine/`) is the agent's contract
with the machine: docker with no user-namespace remapping (the slot's UID in the container
is the machine's, which the egress counters and the metadata fence match); the AWS CLI v2
at `/usr/local/bin/aws` (the baked fleet image installs it, `deploy/host/fleet-image.sh`)
and an instance role that reads the deployment's trees in the object store
(`s3:GetObject` on `data/<deployment>/trees/*`, `s3:ListBucket`); the trees root
`/opt/game` on the root volume (sized for several trees); ports 32768-32831 reserved out of
the ephemeral range; the metadata fence widened to UIDs 10001-10032; the slots' UIDs unable
to reach the service's internal port; the token file and the unit above; the agent's log
relay ("Logs").

## Development and tests

Run it on a Linux system with docker against a development service:

```
sudo opennova-hostd --service http://127.0.0.1:8070 --machine 1 --iptables none \
  --state-dir /tmp/hostd --slots-root /tmp/slots --trees-root /tmp/trees \
  --token-file /tmp/hostd.token --verbose
```

Not as root, it warns and leaves the slots' files its own user's. A development service
over a local object store (`file:///...`) names tree URLs the agent copies, so no AWS CLI
is needed.

The ctests (`tests/hostd`, Linux only; none runs docker, iptables or the AWS CLI):

- `opennova_hostd_runtime` drives the docker runtime, the egress counters and the command
  line on a stub command runner that checks every argv: the exact `docker run` (the base
  and expansion trees' binds, with the journal, and with a log group: the slot's stream,
  non-blocking), the
  listings and their partial answers, each call's timeout, the image check, docker's texts
  read, the chain's rules put in place only where missing and the counts summed; the
  background pull runs stand-in programs for the CLI. And the process runtime over a
  stand-in `opennova-serve` script: its argv, its working directory, the env file's
  variables and none of the agent's own, running then exited with its code, SIGTERM and
  SIGKILL, removal, and a start that cannot run (125).
  background pull runs stand-in programs for the CLI.
- `opennova_hostd_tree_store` drives the tree store on a fake fetcher and a hand-stepped
  clock: a tree fetched, checked against its manifest and marked present (extras removed),
  a size or a file off, a fetch that fails or does not start, the retry wait and
  the three tries, one fetch at a time, the removal of unnamed trees, a restart's scan of
  the root, the expansion mount point; then the real fetcher's `file://` copy on a real
  directory and its `aws s3 sync` run by stand-in programs.
- `opennova_hostd_agent` drives the bootstrap and the sync passes against an in-process fake
  of the service's routes, a fake container runtime and a fake tree fetcher on a
  hand-stepped clock: the saved
  bootstrap, starts and their files, stops, kills, removals, the slot handover, failed
  starts, the background pulls (done, failed, past their bound), the trees fetched while
  their containers wait, a start with both binds, a fetch that fails the start with its
  reason on the report, the trees let go of, the pass's time shares,
  refusals, every failing sync, and a log that never holds the secret, the token or a
  file's bytes.

The protocol itself is `hosted_agent_protocol` (`tests/novaworld`). The real binary runs
end to end, with the process runtime, in `hosted_agent_route` (against the service's agent
routes, a stand-in `opennova-serve`) and `hosted_local_e2e` (under the service's supervisor,
real servers), and in the local stack's smoke test (`scripts/dev_stack.sh smoke`), its
machines containers of the serve image.
