# The runner group: this repository alone, and public repositories allowed
# (GitHub's default refuses them, and a job that asks for a runner it may not
# use waits for one forever).
data "github_repository" "ci" {
  full_name = "${var.owner}/${var.repository}"
}

resource "github_actions_runner_group" "ci" {
  name                       = "opennova-ci"
  visibility                 = "selected"
  selected_repository_ids    = [data.github_repository.ci.repo_id]
  allows_public_repositories = true
}

# The runners. A runner's name is the label ci.yml's runs-on names.
resource "github_actions_hosted_runner" "windows" {
  name = "windows-16core"
  image {
    id     = var.windows_image_id
    source = "github"
  }
  size            = var.size
  runner_group_id = github_actions_runner_group.ci.id
  maximum_runners = var.windows_maximum_runners
}

resource "github_actions_hosted_runner" "linux" {
  name = "ubuntu-16core"
  image {
    id     = var.linux_image_id
    source = "github"
  }
  size            = var.size
  runner_group_id = github_actions_runner_group.ci.id
  maximum_runners = var.linux_maximum_runners
}
