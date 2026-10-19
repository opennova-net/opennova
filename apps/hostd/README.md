# opennova-hostd

The machine agent of the hosted servers' fleet (pre-1.0, experimental). Each machine the
NovaWorld service launches runs one `opennova-hostd`, the fleet cluster's ECS daemon. It fetches
the game data trees the service's replies name (ADR 0064), starts the `opennova-serve`
containers the service places on the machine's slots, stops the ones the service no
longer wants, and reports every container's state, CPU, memory, its slot's network and its
server's tick health back, with the machine's own image, use and trees. It never decides
anything on its own: the service places
servers, pins their trees, counts restarts and ends machines; the agent does what the
last reply said and keeps doing it while the service cannot be reached.

Linux only. Target `opennova_hostd`, binary `opennova-hostd`, built from `apps/hostd` over
the service's two lowest libraries, which build without the service and reach no database:
its core (`opennova_novaworld_core`: the ECS metadata reader,
`apps/novaworld_common/core/ecs_metadata.*`, and the kernel's files, `core/proc_stats.*`) and
the agents' protocol (`opennova_novaworld_hosting_protocol`, the service's half of it:
`apps/novaworld_common/hosted/protocol/agent_protocol.*`, `slots.h`). It ships in an image of its own, `opennova-hostd` (`apps/serve/Dockerfile`'s `hostd`
target: the agent at `/usr/local/bin/opennova-hostd`, `opennova-serve` beside it, the static
docker CLI, the AWS CLI v2 and nft, every one pinned). On a cloud machine it is the ECS
daemon of the deployment's fleet cluster (`infra/aws/fleet_agent.tf`): one task per machine,
root on the machine's network, which `app deploy` rolls with each release, before the fleet
program, at most half the machines at once (`DEPLOY.md`). The servers' containers are
docker's, started by the agent, never ECS tasks, so a new agent takes over the running
servers as they are. The local stack's machines are themselves containers of that image,
the agent their own process, with the process runtime (below).

## Vocabulary

A **machine** is the EC2 virtual machine; a **shared machine** packs up to K servers, an
**own machine** runs exactly one (the paid add-on). A **slot** is one server's position on a
machine (`hosted/protocol/slots.h`): slot k is the game's UDP port 32768 + k (open to everyone), the
remote admin's TCP port 32800 + k (open to the service alone), the container's user
10001 + k and the working directory `/srv/slots/slot-k` (0700, that user's). At most 32
slots; an own machine has slot 0 alone. A **tree** is one immutable version of a data
set's files in the object store (CONTEXT.md "Tree"): a server is pinned to a base tree
(stock JO, DFX2) and, with an expansion, to an expansion tree (Escalation's), and the
machine holds each tree it needs at `/opt/game/<tree id>/`.

## Command line

```
opennova-hostd --service http://<host>:<port> (--machine <id> | --machine-file <path>) [options]
opennova-hostd --check-alive [--state-dir <path>]
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--service` | required | The fleet's internal listener as the machine reaches it (`ONNET_INTERNAL_URL`: the deployment's private name for the fleet program, resolved at each request, so the agent follows the fleet program to a new instance; each request tries every address the name lists until one takes the connection, since during a fleet leader's replacement it lists the standby, which does not listen, beside the leader), `http://` only. Its port is one the slots' users are fenced off ("Fences and counters"). |
| `--machine` | | This machine's id, the service's `fleet_machines` row (the local stack's driver names it). |
| `--machine-file` | | Or the file holding it, one line: the user-data's `/etc/opennova/machine-id` (the fleet cluster's task definition is the same for every machine). One of the two is required. |
| `--ecs-config` | none | The ECS agent's config (`/etc/ecs/ecs.config`): the agent refuses to run (exit 1) unless its `ECS_INSTANCE_ATTRIBUTES` name this machine's id as `machine-id`, the attribute the ECS agent registered the machine with, so a container instance found by its attribute (`fleet shell`, `fleet status`) is always the machine the service names. |
| `--token-file` | `/etc/opennova/hostd.token` | The one-time bootstrap token the machine's user-data wrote (0600). Deleted once redeemed. |
| `--state-dir` | `/var/lib/opennova-hostd` | The saved bootstrap (`machine-<id>.json`, 0600), the lock, the alive file and `scratch/` (the CLIs' output files, a pull's error file, a start's environment file, the nft ruleset). Made 0700. |
| `--slots-root` | `/srv/slots` | The slots' working directories. Made 0755 when missing. |
| `--runtime` | `docker` | How the servers run: `docker`, each a docker container (a cloud machine); `process`, each an `opennova-serve` child of the agent (a machine that is itself a container, the local stack's; "The process runtime" below). |
| `--trees-root` | `/opt/game` | The trees' directories (`<root>/<tree id>/`, `.partial` while one fetches). Made 0755 when missing. |
| `--docker` | `/usr/bin/docker` | The docker CLI, run as given (no `PATH` search). |
| `--serve` | `/usr/local/bin/opennova-serve` | The process runtime's `opennova-serve`. |
| `--aws` | `/usr/local/bin/aws` | The AWS CLI a tree's `s3 sync` runs, as given. |
| `--region` | none | The deployment's AWS region: a tree's `s3 sync` names it (`--region`; unset, the CLI's own, a developer's profile), and so does `--log-group`'s driver. |
| `--nft` | `/usr/sbin/nft` | The slots' fences and counters (`table inet opennova`, "Fences and counters"), or `none` for neither (a machine with no slot users: the process runtime's). |
| `--log-group` | none | With `--region` (the group's), the docker runtime's: the CloudWatch log group the containers' output goes to (the servers' group), the stream the service names for each start ("Logs" below). Unset, the machine's journal. |
| `--log-dir` | none | The process runtime's (the local stack's): the log directory each server's output goes to, `servers/<server id>/g<generation>.log`, beside a copy of the agent's own lines, `machines/<machine id>/hostd.log`, each after its time ("Logs" below). |
| `--once` | | One pass, then exit: 0 when the sync was answered and read. |
| `--check-alive` | | The ECS health check: exit 0 when a running agent's pass loop touched its alive file (`<state-dir>/alive`) within 120 s, 1 otherwise. The loop touches it at every pass and every 10 s of a wait, whether or not the service answers: a stuck loop is unhealthy, an unreachable service is not. |
| `--verbose` | | Debug lines too. |
| `--version` | | The build (`OPENNOVA_HOSTD_VERSION` at configure time; the image passes the commit). |

The one variable the agent reads is the ECS agent's, `ECS_CONTAINER_METADATA_URI_V4`: on
ECS it reads its own container's metadata once at its start, its `Image` (the reference the
task definition names, a release's pinned by digest), which it reports in every sync's
envelope (`agent_image`; "" off ECS), and its log stream (its awslogs options), which every
sync reports (`log_stream`). It also reads, once at its start with the docker runtime, the
machine's image (the instance metadata service's `ami-id`, IMDSv2). The CLIs it runs get
the system's variables alone (`PATH`, `HOME`, `LANG`, `LC_*`, `TZ`, `TMPDIR`, `DOCKER_*`,
and `AWS_*` for a developer's profile and region; on a machine the AWS CLI's credentials
are the hostd task role's, through the ECS agent's credentials endpoint).

The fleet cluster's task definition (`infra/aws/fleet_agent.tf`) runs it as

```
opennova-hostd --service http://fleet.prod.opennova.internal:8070 \
  --machine-file /etc/opennova/machine-id --ecs-config /etc/ecs/ecs.config \
  --region us-east-1 --log-group /opennova/prod/servers
```

as user 0 with docker's default capabilities and `NET_ADMIN` added (never privileged), on
the host's network, with the docker socket, `/etc/opennova`, `/var/lib/opennova-hostd`,
`/srv/slots`, `/opt/game` and `/etc/ecs` (read-only) bound at the machine's own paths (the
servers' `docker run --mount` names host paths), its health check `opennova-hostd
--check-alive`. Two agents never share a machine: the agent holds a lock on its state
directory for its life (`flock`), and a replacement that starts before the task it replaces
has ended waits for it. Replacing the agent (a release, a restart, a crash) never touches a
container: they are docker's, not the agent's children, and the new agent reads the saved
bootstrap and takes them over.

## The protocol

Both routes are on the service's internal listener and answer every refusal with a bare
404. The message shapes, their checks and their caps are
`apps/novaworld_common/hosted/protocol/agent_protocol.h`; the agent and the service build the same
encoders and strict decoders.

**Bootstrap**, once per machine:

```
POST /hosted/machines/<id>/bootstrap      Authorization: Bearer <one-time token>
{"protocol": 3, "agent_version": "<build>"}
-> 200 {"protocol": 3, "agent_secret", "serve_image", "slots"}
```

What the reply holds is saved owner-only at `<state-dir>/machine-<id>.json`, in a format of
the agent's own (`saved_machine.h`: the machine's id, the secret, the serve image and the
slots, and no protocol), and the token file deleted, so a restarted agent never needs the
token again, whichever protocol it speaks: an agent of a later protocol reads what an earlier
one saved. The service spends the token at the
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
{"protocol": 3, "agent_version", "agent_image", "image_id", "log_stream",
  ["machine": {"cpu_percent", "memory_used_bytes", "memory_total_bytes", "disk_free_bytes",
               "disk_total_bytes", "net_in_bytes", "net_out_bytes"},]
  "trees": {"<tree id>": {"state", "bytes", "tries", ["reason"]}},
  "containers": [{"name", "slot", "state", "waiting", "exit_code", "oom", "started_at",
  ["reason",] "cpu_percent", "memory_bytes", "egress_bytes", "net_in_bytes",
  ["tick": {"p50_ms", "p99_ms", "max_ms", "late"}]}],
  ["listing_failed": true]}
-> 200 {"protocol": 3, "drain", "desired": [{"name", "slot", "image", "uid", "base_tree",
  "expansion_tree", "expansion_code", "args", "files", "env", "cpus", "memory_mb", "pids",
  "log": {"stream"}}],
  "trees": {"<tree id>": {"url", "version", "files": [{"name", "size"}]}}}
```

What the agent says of the machine: `image_id` (its AMI, `""` off EC2) and `log_stream`
(its own ECS task's stream, `""` off ECS); `machine`, once it has read `/proc` twice
(`apps/novaworld_common/core/proc_stats.h`: the CPU busy between the readings as a percent of
the whole machine, the memory in use of the machine's, the trees root's filesystem, every
interface's bytes but loopback's since the machine booted); `trees`, every tree its store
holds (`fetching`, a failure with a try left among them, `ready`, or `failed` once its
tries are spent; its bytes on disk, the fetch's so far while it fetches; the fetches made;
the last failure's reason). Of each container: `state` is `starting`, `running` or
`exited`, and a starting one's `waiting` says what for (`image`, `trees`, or `""`);
`started_at` Unix seconds; `reason` rides an exited container when the agent knows why:
the line `opennova-serve` wrote at its non-zero exit (its `--exit-file`, `exit.txt` in the
slot's directory), or why one never started (`its data could not be fetched: ` and the
fetch's error: the service counts no restart for it, and shows the line to an admin alone);
`egress_bytes` and `net_in_bytes` the slot's counts since the
machine booted, what its user sent to the internet and what reached its ports (a lower
count than the last is a counter that started over); `tick` a running server's last minute
of ticks (its `--stats-file`, `tick-stats.json` in the slot's directory, read at most every
5 s and only while it is less than 3 minutes old). The agent, root, reads both files as
files the slot's user controls: opened from the slot's directory with `O_NOFOLLOW` and
`O_NONBLOCK`, a regular file of at most 4 KiB alone, its age its open file's own; a link, a
FIFO, a directory or a larger file reads as nothing written. Each desired container names
its log stream (`log`, "Logs" below). `files` (each file's bytes in base64) is sent until
the agent reports the container running or exited (one it reports `starting` waits for its image or
its trees, and an agent replaced meanwhile needs them again); `uid` is always the slot's. The reply's `trees` names every tree its
desired containers use, with where to fetch it (`url`: `s3://<bucket>/<root>/trees/<id>/`,
or `file:///` on a developer's machine) and its manifest (each file's name and size);
`expansion_tree` is null and `expansion_code` `""` for a server without an expansion.

Every sync opens with the same **envelope**, whatever its protocol: `protocol`,
`agent_version` and `agent_image`. The service reads it first and records it at every sync.
One of another protocol, older or newer, is held: the bare 404 (the agent acts on nothing,
its servers run on as they are), the machine heard all the same (never lost for it, its
slots' last reports kept), and ended only once its syncs have spoken another protocol
for 10 minutes: terminated, its servers placed again elsewhere with no restart counted (a
drain would leave each server it moved running here too, the held agent stopping none). So
the agent rolls before the fleet program within that window and nothing restarts.

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
(it is then ended), is that start's failure: `exited` 125. A fresh machine holds no serve
image, so its first server waits for one pull.

The machine's serve images are pruned at a pass with no work under way, at most every 10
minutes: every image of a repository the replies named (`docker image ls` by digest) that no
desired or present container uses, nor a pull under way, is removed (`docker image rm`),
but the two the replies named last (the release's current and its previous, so a restart on
either pulls nothing). The agent's own image is ECS's to clean up.

A desired container whose base or expansion tree the machine lacks waits the same way for
the tree's fetch (`tree_store.h`): the store fetches one tree at a time, in the
background, into `<trees root>/<id>.partial/` (`aws s3 sync <url> <dir> --no-progress
--only-show-errors --region <the deployment's>` as a child of its own, `--region` left out
without one; a `file:///` URL is a directory copy), then
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

On a cloud machine each server's output goes to the deployment's servers log group,
`/opennova/<deployment>/servers` (`infra/aws`, 14 days), a stream per start that the
service names in the container's desired entry and keeps on the server's start row
(`hosted_server_starts.log_stream`), and the agent's own log to the machines log group,
`/opennova/<deployment>/machines` (30 days), a stream per hostd task (its lines name the
machine's id, the service's `fleet_machines` row the admin fleet view shows beside each
server's slot):

| Group | Stream | What |
| --- | --- | --- |
| servers | `<server id>/g<generation>` | one start's container output, standard output and error (the server's status page is off, `--status-page off`) |
| machines | `hostd/hostd/<task id>` | the agent's own lines, one stream per hostd task (its ECS task definition's awslogs driver) |

A start's container logs through docker's `awslogs` driver: `--log-group` and `--region`
(the servers' group and its region) make its log options

```
--log-driver awslogs --log-opt awslogs-region=<region> --log-opt awslogs-group=<group>
--log-opt awslogs-stream=<server id>/g<generation> --log-opt mode=non-blocking
--log-opt max-buffer-size=4m
```

Non-blocking, so CloudWatch slow or out of reach never holds up a server's write (lines past
the 4 MB buffer are dropped) or its start (docker makes the stream in the background). The
docker daemon writes with the machine role's credentials (which reach that log group
alone); the slots' users are fenced off the metadata
service and never hold them. Docker also keeps a local copy (its dual logging), so `docker
logs <name>` reads a container's output on the machine (`fleet shell`) until the
agent removes the container; CloudWatch keeps it after. Without `--log-group` (a
development machine) the output goes to the journal: `--log-driver journald --log-opt
tag=<name>` (`journalctl CONTAINER_NAME=<name>`).

The agent's own lines go to its standard error, which its task definition's awslogs driver
ships to the machines group (non-blocking, a stream per task: `hostd/hostd/<task id>`), with
the instance role (the task has no execution role). On the machine, `docker logs` of the
agent's ECS container reads them too.

Reading them needs no shell on the machine: `./deploy/run.sh --deployment <deployment> fleet
logs <machine id> [<slot>|hostd] [--since 1h] [--follow]`, and `app logs hostd` for every
agent's (DEPLOY.md, "Hosted game servers").

## The process runtime

`--runtime process` (`process_runtime.h`) runs each server as a child process of the agent
rather than a docker container: the runtime of a machine that is itself a container and runs
no docker, the local stack's (`apps/novaworld_common/hosted/cloud/docker_machine_driver.h`, which
starts the hostd image as its serve user with `--runtime process --nft none --log-dir
/var/log/opennova`, the stack's log volume). A start
is `<--serve> --resource-dir <the data set's mount path> <opennova-serve's arguments>` in the
slot's directory, its environment the system's variables (`PATH`, `HOME`, `LANG`, `LC_*`,
`TZ`, `TMPDIR`) and the env file's, its output where the agent's own goes (the machine
container's log), or with `--log-dir` (the local stack's log volume) to that start's own
file, `<dir>/servers/<server id>/g<generation>.log`, beside the agent's own lines in
`<dir>/machines/<machine id>/hostd.log` (each after its time, `2026-10-18T14:05:03.123Z`),
which the site's admin pages read as another user (web's), so the agent makes each directory
there 0755 and each file 0644 whatever its umask (`stamped_output.h` open_log_file). The rest of the agent is the same: the bootstrap, the syncs, the slots'
directories, the starts, stops (SIGTERM), kills (SIGKILL) and removals the reply asks for,
each server reported by name with its exit code (one a signal ended reads 128 plus the
signal) until the reply drops it. What a container gave and a process does not: no slot user
(the agent's own; the uid, CPUs, memory and pids cap are not applied), no fences and no
counters, no CPU or memory figure (0), and every image is the machine's own (never pulled
nor pruned). The
servers end with the agent (the parent-death signal), so a machine whose agent ended runs
nothing, as its container ends with it.

## Fences and counters

The agent owns one nftables table, `table inet opennova` (`nft_table.h`), IPv4 and IPv6 in
one, applied whole at its start in one `nft -f` transaction (the table, its counters and its
two chains added where missing, the chains' rules flushed and written again), so the rules
are always exactly these and the counts carry on across its restarts:

- **output**: every slot user (UIDs 10001-10032, `meta skuid`) refused the instance metadata
  service (169.254.169.254, Nitro's fd00:ec2::254: the instance role's credentials, the
  user-data with its token), the ECS task metadata and credentials address 169.254.170.2,
  and, on any destination, a new TCP connection to the fleet's port (the agents' routes), to
  the slots' remote-admin ports 32800-32831 (a sibling's: admin.cfg admits the fleet's whole
  network, the machine's own address in it) and to the ECS agent's 51678 (introspection) and
  51679 (the task credentials 169.254.170.2:80 is DNAT'ed to); then a packet to a private
  destination (RFC 1918, link-local, loopback; unique-local, link-local and loopback for IPv6)
  goes uncounted; then slot k's user's is counted in the named counter `egress-<k>`;
- **input**: a packet to slot k's game port (UDP 32768 + k) or its admin port (TCP 32800 + k)
  is counted in `ingress-<k>`.

What the fences refuse is never counted. The counters are named objects, a pair for each of
the 32 slots, so they count since the machine booted; the agent reads them every pass (`nft
-j list counters table inet opennova`) and applies the table again (at most every minute)
when a read fails. Nothing else on the machine (docker's tables, the ECS agent's) shares the
table, so neither the Amazon Linux 2023 image's iptables-nft nor docker's rules meet it.

At its start, the table applied, the agent probes its own fences as slot 0's user
(`fence_probe.h`, a child dropped to UID 10001): a TCP connection to the instance metadata
service, the ECS credentials address, the ECS agent's 127.0.0.1:51678 and every address of
the fleet's internal listener must be refused (or go unanswered); one that connects ends the
agent with exit 1 ("UID 10001 still reaches ..."), so no server starts behind a fence that
does not hold. ECS starts it again; the machine's servers, if any, run on as they are.

## What the machine provides

The machine (`apps/novaworld_common/hosted/cloud/machine/`, the user-data on the pinned
ECS-optimized Amazon Linux 2023 image) is the agent's contract with the machine: docker with
no user-namespace remapping (the slot's UID in the container is the machine's, which the
fences and the counters match) and the ECS agent, both the image's own; the token and the
machine id in `/etc/opennova` (0700, each file 0600); `/etc/ecs/ecs.config` naming the fleet
cluster, the instance attributes `machine-id` and `kind`, task IAM roles in host network mode
and no privileged task (never the ECS agent's cleanup of images it did not pull: it would
delete the servers' images); ports 32768-32831 reserved out of the ephemeral range. The
fleet cluster's daemon service brings the rest: the agent's image (the docker CLI, the AWS
CLI v2, nft), its task role (`s3:GetObject` on `data/<deployment>/trees/*`,
`s3:ListBucket` on that prefix, ECS Exec's channels) and its paths. The trees root
`/opt/game` and the slots' root are made by the agent on the root volume. Never drain the
fleet cluster through ECS: a drain stops the agent, not the servers (the service drains a
machine itself).

## Development and tests

Run it on a Linux system with docker against a development service:

```
sudo opennova-hostd --service http://127.0.0.1:8070 --machine 1 --nft none \
  --state-dir /tmp/hostd --slots-root /tmp/slots --trees-root /tmp/trees \
  --token-file /tmp/hostd.token --verbose
```

Not as root, it warns and leaves the slots' files its own user's. A development service
over a local object store (`file:///...`) names tree URLs the agent copies, so no AWS CLI
is needed.

The ctests (`tests/hostd`, Linux only; none runs docker, nft or the AWS CLI; each test
says what it pins): `opennova_hostd_runtime` (the docker and process runtimes, the nft table's
ruleset and its counters' JSON on a stub command runner that checks every argv, the fences'
probe against loopback, the machine id file, the ECS attribute check and the alive file, the
process runtime's log directory, the machine's image and the agent's log stream from
stand-in metadata endpoints), `opennova_hostd_tree_store` (the
tree store on a fake fetcher and a hand-stepped clock, then the real fetcher's `file://` copy
and its `aws s3 sync` run by stand-in programs) and `opennova_hostd_agent` (the bootstrap,
its saved file read again across a protocol bump, and the sync passes against an in-process
fake of the service's routes, the envelope, the files sent again to a replaced agent, the
images pruned, the machine's use from fixture `/proc` files, the servers' tick and exit
files (a FIFO, a link, a directory and a sparse oversize file among them, none of which
blocks or reads), never logging the secret, the token or a file's bytes).

The protocol itself is `hosted_agent_protocol` (`tests/novaworld`). The real binary runs
end to end, with the process runtime, in `hosted_agent_route` (against the service's agent
routes, a stand-in `opennova-serve`) and `hosted_local_e2e` (under the service's supervisor,
real servers), and in the local stack's smoke test (`scripts/dev_stack.sh smoke`), its
machines containers of the hostd image.
