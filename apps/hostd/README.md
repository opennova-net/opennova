# opennova-hostd

The machine agent of the hosted servers' fleet (pre-1.0, experimental). Each machine the
NovaWorld service launches runs one `opennova-hostd`, as root under systemd. It starts the
`opennova-serve` containers the service places on the machine's slots, stops the ones the
service no longer wants, and reports every container's state, CPU, memory and its slot's
egress back. It never decides anything on its own: the service places servers, counts
restarts and ends machines; the agent does what the last reply said and keeps doing it
while the service cannot be reached.

Linux only. Target `opennova_hostd`, binary `opennova-hostd`, built from `apps/hostd` with
the service's half of the protocol (`apps/novaworld_server/hosted/agent_protocol.*`,
`slots.h`) compiled in. It ships inside the `opennova-serve` image at
`/usr/local/bin/opennova-hostd` (`apps/serve/Dockerfile`) and never runs in a container: a
machine copies it out of the image it pulled at its bootstrap, so each machine's agent is
the build of the serve image it was launched with.

## Vocabulary

A **machine** is the EC2 virtual machine; a **shared machine** packs up to K servers, an
**own machine** runs exactly one (the paid add-on). A **slot** is one server's position on a
machine (`hosted/slots.h`): slot k is the game's UDP port 32768 + k (open to everyone), the
remote admin's TCP port 32800 + k (open to the service alone), the container's user
10001 + k and the working directory `/srv/slots/slot-k` (0700, that user's). At most 32
slots; an own machine has slot 0 alone.

## Command line

```
opennova-hostd --service http://<service private address>:<port> --machine <id> [options]
```

| Flag | Default | Meaning |
| --- | --- | --- |
| `--service` | required | The service's internal listener (`ONNET_INTERNAL_HTTP_BIND`), `http://` only. |
| `--machine` | required | This machine's id, the service's `fleet_machines` row. |
| `--token-file` | `/etc/opennova/hostd.token` | The one-time bootstrap token the machine's user-data wrote (0600). Deleted once redeemed. |
| `--state-dir` | `/var/lib/opennova-hostd` | The saved bootstrap (`machine-<id>.json`, 0600) and `scratch/` (the CLIs' output files, a start's environment file). Made 0700. |
| `--slots-root` | `/srv/slots` | The slots' working directories. Made 0755 when missing. |
| `--docker` | `/usr/bin/docker` | The docker CLI, run as given (no `PATH` search). |
| `--iptables` | `/usr/sbin/iptables` | The egress counters' iptables, or `none` to count nothing. |
| `--ip6tables` | `/usr/sbin/ip6tables` | The same for IPv6, used only where the kernel has IPv6; `none` skips it. |
| `--once` | | One pass, then exit: 0 when the sync was answered and read. |
| `--verbose` | | Debug lines too. |
| `--version` | | The build (`OPENNOVA_HOSTD_VERSION` at configure time; the serve image passes the commit). |

The agent reads no environment variable. The CLIs it runs get the system's variables alone
(`PATH`, `HOME`, `LANG`, `LC_*`, `TZ`, `TMPDIR`, `DOCKER_*`).

A unit the machine's user-data can install:

```
[Unit]
Description=opennova-hostd, the hosted servers' machine agent
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
ExecStart=/usr/local/sbin/opennova-hostd --service http://10.44.0.10:8070 --machine 12
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
`apps/novaworld_server/hosted/agent_protocol.h`; the agent and the service build the same
encoders and strict decoders.

**Bootstrap**, once per machine:

```
POST /hosted/machines/<id>/bootstrap      Authorization: Bearer <one-time token>
{"protocol": 1, "agent_version": "<build>"}
-> 200 {"protocol": 1, "agent_secret", "serve_image", "slots", "data_sets": {"<data set>": "<mount path>"}}
```

The reply is saved owner-only at `<state-dir>/machine-<id>.json` and the token file deleted,
so a restarted agent never needs the token again. Without a saved reply and a token, or
with a token the service refuses (spent, lapsed, unknown), the agent waits and tries again
every 10 s; the service sees a machine that never reports and ends it.

**Sync**, every 5 s, every 1 s while the agent has work (a container to start, one stopping,
one to remove):

```
POST /hosted/machines/<id>/sync           Authorization: Bearer <agent secret>
{"protocol": 1, "agent_version", "containers": [{"name", "slot", "state", "exit_code",
  "oom", "started_at", "cpu_percent", "memory_bytes", "egress_bytes"}], ["listing_failed": true]}
-> 200 {"protocol": 1, "drain", "desired": [{"name", "slot", "image", "uid", "data_set",
  "args", "files", "env", "cpus", "memory_mb", "pids"}]}
```

`state` is `starting`, `running` or `exited`; `started_at` Unix seconds; `egress_bytes` the
slot's count since the machine booted (a lower count than the last is a counter that started
over). `files` (each file's bytes in base64) is sent until the agent has reported the
container once; `uid` is always the slot's.

## What a sync does

The agent lists its containers (`docker ps` on its label, then `docker inspect`), reads the
running ones' CPU and memory (`docker stats --no-stream`, at most every 5 s) and each slot's
egress, posts the report and acts on the reply (`reconcile.h`, a pure function):

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
  were not sent (the agent lost its state), its data set is not mounted here, or its slot is
  past the machine's.
- A start that fails (the image did not pull, a mount is missing) is reported `exited`
  with docker's exit code (125), its leftovers and files removed, and never retried.

A start never pulls (`docker run --pull never`). A desired container whose image the
machine lacks (`docker image inspect`) waits for a `docker pull` the agent runs in the
background, one per image however many containers want it: it is reported `starting` on its
slot meanwhile (so the service stops sending its files, which the agent keeps), and starts
at the first pass after the pull. A pull that fails, or still runs 4 minutes after it began
(it is then ended), is that start's failure: `exited` 125. The machine's bootstrap pulls the
serve image it copies the agent out of, so only a server pinned to a newer image than its
machine's waits for one.

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
  --read-only --tmpfs /tmp --cpus <cpus> --memory <mb>m --memory-swap <mb>m
  --pids-limit <pids> --stop-timeout 60 --log-driver journald --log-opt tag=<name>
  --mount type=bind,source=<data set's mount path>,target=/opt/game,readonly
  --mount type=bind,source=/srv/slots/slot-<k>,target=/srv/serve
  [--env-file <file>] <image> <opennova-serve's arguments>
```

The machine's own network is required: NovaWorld lists a server at the source address and
port of its datagrams, and a bridge's NAT would rewrite the port. Only containers with the
agent's label are ever listed, stopped or removed. Their output is in the journal
(`journalctl CONTAINER_NAME=<name>` or `-t <name>`).

## Egress

A chain of its own, `OPENNOVA-EGRESS`, that `OUTPUT` jumps to first, holds one rule per slot
(`-m owner --uid-owner <10001 + k> -j RETURN`), so each packet a slot's user sends is
counted once and then meets the rest of `OUTPUT` (the metadata fence among it). The agent
puts the chain, the jump and each rule in place only where missing (checked with `-C`), so
the counters survive its restarts and count since the machine booted; it reads them with
`-L -n -v -x` every pass and puts them back (counting from 0) when a rule went missing.
Every call waits up to 5 s for the xtables lock docker also takes.

## What the machine provides

The machine's user-data (`apps/novaworld_server/hosted/machine/`) is the agent's contract
with the machine: docker with no user-namespace remapping (the slot's UID in the container
is the machine's, which the egress counters and the metadata fence match); every data set's
volume mounted read-only at the path the bootstrap names; ports 32768-32831 reserved out of
the ephemeral range; the metadata fence widened to UIDs 10001-10032; the slots' UIDs unable
to reach the service's internal port; the token file and the unit above.

## Development and tests

Run it on a Linux system with docker against a development service:

```
sudo opennova-hostd --service http://127.0.0.1:8070 --machine 1 --iptables none \
  --state-dir /tmp/hostd --slots-root /tmp/slots --token-file /tmp/hostd.token --verbose
```

Not as root, it warns and leaves the slots' files its own user's.

The ctests (`tests/hostd`, Linux only; neither runs docker nor iptables):

- `opennova_hostd_runtime` drives the docker runtime, the egress counters and the command
  line on a stub command runner that checks every argv: the exact `docker run`, the
  listings and their partial answers, each call's timeout, the image check, docker's texts
  read, the chain's rules put in place only where missing and the counts summed; the
  background pull runs stand-in programs for the CLI.
- `opennova_hostd_agent` drives the bootstrap and the sync passes against an in-process fake
  of the service's routes and a fake container runtime on a hand-stepped clock: the saved
  bootstrap, starts and their files, stops, kills, removals, the slot handover, failed
  starts, the background pulls (done, failed, past their bound), the pass's time shares,
  refusals, every failing sync, and a log that never holds the secret, the token or a
  file's bytes.

The protocol itself is `hosted_agent_protocol` (`tests/novaworld`).
