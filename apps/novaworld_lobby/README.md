# opennova-novaworld-lobby

The NovaWorld service's lobby: one lobby instance, NovaWorld UDP on `ONNET_NW_UDP_PORT`
(64206), as many as the deployment runs. It registers its instance at the endpoint it
announces (`ONNET_LOBBY_ENDPOINT`, or on ECS its task's public IPv4, found from the task
metadata and EC2: `apps/novaworld_common/ecs_metadata.h`), holds its sessions in its own
memory, writes their rows and takes the lobby commands the other programs address to them.
Its stop drains its instance first. `GET /livez` on `ONNET_HEALTH_PORT` (8081). The image
carries the AWS CLI for the discovery.

The service as a whole, its five programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `lobby` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `nw_udp_listener.*`: the listener and the instance's lifecycle (`opennova_novaworld_lobby_core`).
- `main.cpp`: the program (`opennova_novaworld_lobby`, binary `opennova-novaworld-lobby`).
