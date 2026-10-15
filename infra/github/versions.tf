# The GitHub organization opennova-net, all of it under Terraform (DEPLOY.md,
# "GitHub"): the organization's settings, members and Actions permissions, the
# CI runners and their runner group, every repository (settings, default
# branch, vulnerability alerts), master's branch protection and the Actions
# secrets. Org-wide, so one stack with no deployments (no workspaces): the
# toolbox runs it as `./deploy/run.sh github plan|apply`, its state in the vault
# document `tfstate-github-org` (not the retired expansion stack's
# `tfstate-github`), its token and inputs from the vault
# (deploy/env/github.env.tpl). imports.tf adopts what existed before the stack.
#
#   versions.tf      Terraform and provider pins, provider configuration
#   variables.tf     every input
#   org.tf           the organization's settings, members, Actions permissions
#   runners.tf       the CI runners and their runner group
#   repositories.tf  the repositories, their default branches and alerts
#   protection.tf    the branch protection
#   secrets.tf       the Actions secrets
#   imports.tf       the objects the stack adopted
#   outputs.tf       the runners' names (ci.yml's runs-on labels)

terraform {
  # The toolbox's Terraform (deploy/Dockerfile), as infra/aws; 1.7 for an
  # import block's for_each.
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
