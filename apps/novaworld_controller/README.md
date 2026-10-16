# opennova-novaworld-controller

The NovaWorld service's controller: elected among the controller processes on the database;
while it leads, the internal listener (the machine agents' routes, TCP 8070, on its task's
private address in a deployment, found from the ECS task metadata), the hosted servers'
supervisor with its admin links, the console drain and the periodic jobs' scheduler. It calls
no cloud: what it decides it writes as rows and jobs. `GET /livez` on `ONNET_HEALTH_PORT`
(8081). Built with `BUILD_NOVAWORLD_HTTP`.

The service as a whole, its five programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `controller` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `controller.*`: the election (`opennova_novaworld_controller_core`, with
  `hosted/internal_listener.*` and `hosted/agent_route.*`).
- `main.cpp`: the program (`opennova_novaworld_controller`, binary
  `opennova-novaworld-controller`).
