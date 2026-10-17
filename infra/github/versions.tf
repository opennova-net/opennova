# The GitHub organization opennova-net, all of it under Terraform (DEPLOY.md,
# "GitHub"): the organization's settings, members and Actions permissions,
# every repository (settings, default branch, vulnerability alerts), master's
# branch protection, the Actions secrets and the deploy workflow's
# environments. Org-wide, so one stack with no deployments: the toolbox runs it
# as `./deploy/run.sh github plan|apply`,
# its token and inputs from the vault (deploy/env/github.env.tpl), its state in
# prod's state bucket (prod's account keeps what the deployments share), key
# infra/github/terraform.tfstate, reached as prod's deploy user. imports.tf adopts what existed before the stack.
#
#   versions.tf      Terraform and provider pins, provider configuration
#   variables.tf     every input
#   org.tf           the organization's settings, members, Actions permissions
#   repositories.tf  the repositories, their default branches and alerts
#   protection.tf    the branch protection
#   secrets.tf       the Actions secrets
#   environments.tf  the deploy workflow's environments, prod and staging
#   imports.tf       the objects the stack adopted

terraform {
  # The toolbox's Terraform (deploy/Dockerfile), as infra/aws; 1.7 for an
  # import block's for_each.
  required_version = ">= 1.11.0"

  required_providers {
    github = {
      source  = "integrations/github"
      version = "~> 6.13"
    }
  }

  # The toolbox gives bucket (the vault's aws-prod/state_bucket), key
  # infra/github/terraform.tfstate, region, use_lockfile and encrypt.
  backend "s3" {}
}

# The token comes from GITHUB_TOKEN (the provider reads it), which the toolbox
# sets from the vault for the run alone.
provider "github" {
  owner = var.owner
}
