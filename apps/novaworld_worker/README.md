# opennova-novaworld-worker

The NovaWorld service's worker: the job pool (`ONNET_WORKER_THREADS` threads) and its
handlers: the prunes, the account jobs, the mail it sends (SES), the billing sync, the fleet's
machine driver (EC2) and, while the static addresses are on, their driver (Elastic IPs) and
their zone (Route 53; `address.*` jobs, `apps/novaworld_common/hosted/cloud/address_job_handlers.h`);
and the scheduler of its own periodic kinds (every one but the data sets' `tree.prune`, the
data program's), in every worker process with no election: a schedule key is unique, so a bucket
gets one job however many workers tick. Its at-start jobs (the billing reconcile) are keyed by
the release its tasks run (its ECS task definition's revision), so a release's tasks run each
once. Any number share the queue. `GET /livez` on `ONNET_HEALTH_PORT` (8081). The image
carries the AWS CLI and the machine script. Built with `BUILD_NOVAWORLD_HTTP`.

The service as a whole, its programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `worker` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `main.cpp`: the program (`opennova_novaworld_worker`, binary `opennova-novaworld-worker`):
  the handlers it registers, the machine and address drivers it builds and its scheduler,
  over the service's libraries (the cloud's its alone).
- `worker_schedule.*`: its periodic jobs (`opennova_novaworld_worker_core`, a library web
  links too: the admin's jobs page shows and runs them from the same settings).
