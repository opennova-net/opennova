# The GitHub organization's CI runners (DEPLOY.md, "CI runners"): the larger
# GitHub-hosted runners .github/workflows/ci.yml's build legs run on, and the
# runner group that lets this public repository use them. Org-wide, so one
# stack with no deployments (no workspaces): the toolbox runs it as
# `./deploy/run.sh github plan|apply`, its state in the vault document
# `tfstate-github`, its token from the vault item `github`
# (deploy/env/github.env.tpl).
#
#   versions.tf   Terraform and provider pins, provider configuration
#   variables.tf  every input
#   runners.tf    the runner group and the runners
#   outputs.tf    the runners' names (ci.yml's runs-on labels)

terraform {
  # The toolbox's Terraform (deploy/Dockerfile), as infra/aws.
  required_version = ">= 1.11.0"

  required_providers {
    # 6.13: github_actions_hosted_runner.
    github = {
      source  = "integrations/github"
      version = "~> 6.13"
    }
  }
}

# The token comes from GITHUB_TOKEN (the provider reads it), which the toolbox
# sets from the vault for the run alone.
provider "github" {
  owner = var.owner
}
