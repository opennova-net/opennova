# opennova-novaworld-gate

The NovaWorld service's gate: retail's first contact on UDP 7597 (`ONNET_GATE_UDP_PORT`).
It answers each probe with the lobby address every lobby instance announces (the
deployment's one, its load balancer's), while one takes clients, this gate as `POSTIPADDRESS`
(`ONNET_PUBLIC_HOST`) and the one web endpoint (`ONNET_WEB_HOST`, `ONNET_HTTP_PORT`), and
takes the hosts' status blobs on the same port. `ONNET_LOBBY_ENDPOINT`, optional, is the lobby
it hands out while no instance takes clients; with neither, a probe goes unanswered (logged,
and counted in the heartbeat's `probes_unanswered`, beside `lobbies_live`: the deployment
alarms on it, since such a gate is otherwise healthy).
Its only send is the reply to a probe. `GET /livez` on `ONNET_HEALTH_PORT` (8081).

The service as a whole, its five programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `gate` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `gate_listener.*`: the listener (`opennova_novaworld_gate_core`).
- `main.cpp`: the program (`opennova_novaworld_gate`, binary `opennova-novaworld-gate`).
