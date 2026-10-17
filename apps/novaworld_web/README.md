# opennova-novaworld-web

The NovaWorld service's web: the HTTP listener on `ONNET_HTTP_PORT` (8080): the retail
`NW*.dll` routes and `.gsb` feeds, the website's API, billing's routes and webhook,
`GET /livez` and `GET /healthz`; its shared state in Redis (`ONNET_REDIS_URL`). Replicas
behind one load balancer; in a deployment the portal's nginx (`web/`, the site) runs beside it
in its task and proxies `/api` to it on 127.0.0.1. Built with `BUILD_NOVAWORLD_HTTP`.

The service as a whole, its five programs, their settings, the database, Redis, the health
checks and the tests: [`../novaworld_common/README.md`](../novaworld_common/README.md). The
image is the `web` target of [`../novaworld_common/Dockerfile`](../novaworld_common/Dockerfile).

- `http_listener.*` and the route files (`*_routes.*`, `hosted/`, `billing/`, `jobs/`, and
  `infra/`: the admin's infrastructure view and its AWS reads), the
  stores it keeps in Redis (`session_store`, `retail_pins`, `rate_limiter`), the CD keys:
  `opennova_novaworld_web_core`.
- `templates/`, `static/`: the retail pages and images the `NW*.dll` flow downloads (in its
  image).
- `main.cpp`: the program (`opennova_novaworld_web`, binary `opennova-novaworld-web`).
