# opennova-novaworld-worker

The NovaWorld service's worker: the job pool (`ONNET_WORKER_THREADS` threads) and its
handlers: the prunes, the account jobs, the mail it sends (SES), the billing sync, the fleet's
machine driver (EC2). Any number share the queue. `GET /livez` on `ONNET_HEALTH_PORT` (8081).
The image carries the AWS CLI and the machine script. Built with `BUILD_NOVAWORLD_HTTP`.

The service as a whole, its five programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `worker` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `main.cpp`: the program (`opennova_novaworld_worker`, binary `opennova-novaworld-worker`):
  the handlers it registers and the machine driver it builds, over the shared library's jobs,
  hosted servers, billing and mail.
