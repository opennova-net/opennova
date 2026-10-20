# opennova-novaworld-lobby

The NovaWorld service's lobby: one lobby instance, NovaWorld UDP on `ONNET_NW_UDP_PORT`
(64206), as many as the deployment runs. It registers its instance, a row of its own, at
the one lobby address every lobby announces (`ONNET_LOBBY_ENDPOINT`: on ECS the load
balancer's, the gate's Elastic IP and UDP 64206, which spreads the clients across the lobby
tasks), holds its sessions in its own memory, writes their rows and takes the lobby
commands the other programs address to them. Its stop drains its instance and stops at once
(the balancer drained its task first), leaving the goodbye it owes each session it held
(`lobby_goodbyes`) for the lobby that session's next packet reaches, which sends it, so the
host reconnects there at once. `GET /livez` on `ONNET_HEALTH_PORT` (8081).

The service as a whole, its six programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `lobby` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `nw_udp_listener.*`: the listener and the instance's lifecycle (`opennova_novaworld_lobby_core`).
- `main.cpp`: the program (`opennova_novaworld_lobby`, binary `opennova-novaworld-lobby`).
