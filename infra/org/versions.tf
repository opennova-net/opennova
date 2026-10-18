# The org stack (infra/aws/iam/README.md; DEPLOY.md, "The Organization and the accounts"): the AWS
# Organization, its member accounts, and each account's baseline. Run from the
# management account as opennova-org, the one principal left there (it may
# make Organizations calls and assume each member account's
# OrganizationAccountAccessRole, nothing else: infra/aws/iam/opennova-org.json):
#
#   versions.tf      Terraform and provider pins; the management account's
#                    provider and one per member account, each assuming that
#                    account's OrganizationAccountAccessRole
#   variables.tf     every input (the vault's, through the toolbox)
#   organization.tf  the organization and its member accounts, adopted by
#                    import blocks
#   accounts.tf      each deployment account's baseline (account/: its state
#                    bucket, the role boundary, the deploy user, its policies
#                    and key, the GitHub deploy role); the ci account, which
#                    holds the CI runners alone, has none (infra/ci makes
#                    what it holds)
#   identity_center.tf  the maintainer's console access to the member accounts
#                    (IAM Identity Center: two permission sets, one user)
#   outputs.tf       what the toolbox writes back to the vault
#
# Run as `./deploy/run.sh org plan|apply` (no --deployment: the stack is the
# organization's). No tracked file names an account, the organization or an
# address: every identifier comes from the vault (items aws-org, aws-prod,
# aws-staging, aws-ci), and what the stack makes that later runs need goes back there.
# Its state is org/terraform.tfstate in prod's state bucket, which this stack
# makes, reached through prod's OrganizationAccountAccessRole (the backend's
# assume_role): the first run is local (the toolbox's backend override), then
# `init -migrate-state` moves the state in. Nothing of OpenNova's stays in the
# management account but the organization and opennova-org.

terraform {
  # The toolbox's Terraform (deploy/Dockerfile), as infra/aws.
  required_version = ">= 1.11.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.100"
    }
  }

  # The toolbox gives bucket (the vault's aws-prod/state_bucket), key
  # org/terraform.tfstate, region, use_lockfile, encrypt and assume_role (prod's
  # OrganizationAccountAccessRole).
  backend "s3" {}
}

locals {
  members = {
    prod    = { account_id = var.aws_account_id_prod, name = var.account_name_prod, email = var.root_email_prod }
    staging = { account_id = var.aws_account_id_staging, name = var.account_name_staging, email = var.root_email_staging }
    ci      = { account_id = var.aws_account_id_ci, name = var.account_name_ci, email = var.root_email_ci }
  }
}

# The management account, as opennova-org: the organization and its accounts.
provider "aws" {
  region              = var.region
  allowed_account_ids = [var.management_account_id]
}

# Each member account, as its OrganizationAccountAccessRole (the role
# Organizations made with the account, trusting the management account).
provider "aws" {
  alias               = "prod"
  region              = var.region
  allowed_account_ids = [var.aws_account_id_prod]

  assume_role {
    role_arn     = "arn:aws:iam::${var.aws_account_id_prod}:role/OrganizationAccountAccessRole"
    session_name = "opennova-org"
  }

  default_tags {
    tags = {
      "opennova:deployment" = "opennova-prod"
    }
  }
}

provider "aws" {
  alias               = "staging"
  region              = var.region
  allowed_account_ids = [var.aws_account_id_staging]

  assume_role {
    role_arn     = "arn:aws:iam::${var.aws_account_id_staging}:role/OrganizationAccountAccessRole"
    session_name = "opennova-org"
  }

  default_tags {
    tags = {
      "opennova:deployment" = "opennova-staging"
    }
  }
}
