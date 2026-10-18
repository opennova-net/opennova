# opennova-novaworld-data

The NovaWorld service's data program: the job pool (`ONNET_WORKER_THREADS` threads) over the
hosted data sets' jobs alone (`tree.finalize`, `release.validate`, `release.apply`,
`tree.prune`; `../novaworld_common/hosted/data_set_jobs.h`, ADR 0064) on the game data's
object store (`ONNET_OBJECT_STORE_URL`), and the scheduler of their prune, in every data
process with no election. A tree's files and a release's package pass through its disk (40 GiB
of task storage in a deployment); it holds the store's credentials and nothing of the fleet,
billing or mail, so an admin's upload never queues a machine launch behind it. With no store it
has no kind to run and idles. `GET /livez` on `ONNET_HEALTH_PORT` (8081). The image carries the
AWS CLI. Built with `BUILD_NOVAWORLD_HTTP`.

The service as a whole, its programs, their settings, the database, Redis, the health checks
and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The image is
the `data` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `main.cpp`: the program (`opennova_novaworld_data`, binary `opennova-novaworld-data`): the
  store it builds, the data sets' handlers it registers, its pool (worker ids `data-<tag>-<n>`)
  and its scheduler (`../novaworld_common/parts.h` `data_schedule`).
