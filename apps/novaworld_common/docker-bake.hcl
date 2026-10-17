# The NovaWorld service's seven images as one bake, run from the repository
# root (.github/workflows/novaworld-images.yml builds and publishes them with
# it; the image names live there):
#   docker buildx bake -f apps/novaworld_common/docker-bake.hcl
# The five programs are one target each of this directory's Dockerfile, so the
# one solve compiles their shared build stage once and finishes the five
# together; beside them the web portal's nginx (web/Dockerfile) and the backup
# task's (deploy/backup).
#
# Tags and labels come from the workflow: docker/metadata-action writes one
# bake file per image whose target is meta-<image>, and each image pushes from
# a bake of its own target (the workflow says why). The empty meta-* targets
# below stand in for those files anywhere else (a local bake builds untagged).

# The GitHub Actions layer cache, a scope per image (each image's own name):
# "read" imports it, "readwrite" also exports it (mode=min: the final image's
# layers only; the build stage's compile changes with every commit). Empty (a
# local bake) uses none.
variable "GHA_CACHE" {
  default = ""
}

function "cache_from" {
  params = [scope]
  result = GHA_CACHE != "" ? ["type=gha,scope=${scope}"] : []
}

function "cache_to" {
  params = [scope]
  result = GHA_CACHE == "readwrite" ? ["type=gha,scope=${scope},mode=min"] : []
}

group "default" {
  targets = ["gate", "lobby", "web", "controller", "worker", "portal", "backup"]
}

target "meta-gate" {}
target "meta-lobby" {}
target "meta-web" {}
target "meta-controller" {}
target "meta-worker" {}
target "meta-portal" {}
target "meta-backup" {}

target "_program" {
  context    = "."
  dockerfile = "apps/novaworld_common/Dockerfile"
}

target "gate" {
  inherits   = ["_program", "meta-gate"]
  target     = "gate"
  cache-from = cache_from("opennova-novaworld-gate")
  cache-to   = cache_to("opennova-novaworld-gate")
}

target "lobby" {
  inherits   = ["_program", "meta-lobby"]
  target     = "lobby"
  cache-from = cache_from("opennova-novaworld-lobby")
  cache-to   = cache_to("opennova-novaworld-lobby")
}

target "web" {
  inherits   = ["_program", "meta-web"]
  target     = "web"
  cache-from = cache_from("opennova-novaworld-web")
  cache-to   = cache_to("opennova-novaworld-web")
}

target "controller" {
  inherits   = ["_program", "meta-controller"]
  target     = "controller"
  cache-from = cache_from("opennova-novaworld-controller")
  cache-to   = cache_to("opennova-novaworld-controller")
}

target "worker" {
  inherits   = ["_program", "meta-worker"]
  target     = "worker"
  cache-from = cache_from("opennova-novaworld-worker")
  cache-to   = cache_to("opennova-novaworld-worker")
}

# The web portal (nginx), web's second container.
target "portal" {
  inherits   = ["meta-portal"]
  context    = "web"
  dockerfile = "Dockerfile"
  cache-from = cache_from("novaworld-web")
  cache-to   = cache_to("novaworld-web")
}

# The nightly backup task's image (pg_dump to S3).
target "backup" {
  inherits   = ["meta-backup"]
  context    = "deploy/backup"
  dockerfile = "Dockerfile"
  cache-from = cache_from("opennova-novaworld-backup")
  cache-to   = cache_to("opennova-novaworld-backup")
}
