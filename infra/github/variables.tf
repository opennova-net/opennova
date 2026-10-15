variable "owner" {
  description = "The GitHub organization the runners belong to."
  type        = string
  default     = "opennova-net"
}

variable "repository" {
  description = "The repository (in owner) whose workflows may use the runners; it is public, so the runner group allows public repositories."
  type        = string
  default     = "opennova"
}

variable "size" {
  description = "Both runners' machine size (GET /orgs/{org}/actions/hosted-runners/machine-sizes). It can change in place."
  type        = string
  default     = "16-core"
}

# The images the standard windows-latest and ubuntu-latest labels run (GET
# /orgs/{org}/actions/hosted-runners/images/github-owned): the build legs on
# these runners share compiler caches with jobs that stay on the standard ones
# (package jobs, fork pull requests), and a cache entry only serves the same
# compiler. An image cannot change in place: a new one replaces the runner.
variable "windows_image_id" {
  description = "The Windows runner's GitHub-owned image: 2307, Windows Latest (2025)."
  type        = string
  default     = "2307"
}

variable "linux_image_id" {
  description = "The Linux runner's GitHub-owned image: 2306, Ubuntu Latest (24.04)."
  type        = string
  default     = "2306"
}

# The scale-up caps, which also cap the spend: a pull request's run takes up
# to 6 Windows machines at once (the two core shards and the retail leg while
# the Godot tests and the package follow the gdext build) and a master push 7
# (the gdext build's release leg); jobs past the cap queue.
variable "windows_maximum_runners" {
  description = "How many Windows machines may run at once."
  type        = number
  default     = 10
}

variable "linux_maximum_runners" {
  description = "How many Linux machines may run at once."
  type        = number
  default     = 5
}
