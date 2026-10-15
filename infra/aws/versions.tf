# The NovaWorld service's AWS + Cloudflare infrastructure (DEPLOY.md, section 3).
#
# Flat files by concern, no child modules:
#   versions.tf    Terraform and provider pins, provider configuration
#   locals.tf      the deployment (workspace) and the names derived from it
#   variables.tf   every input
#   network.tf     the VPC: public subnets a/b (service, machines), private a/b
#   service.tf     the service host
#   service_ssm.tf what drives the host: SSM parameters and the converge document
#   database.tf    PostgreSQL 17 on RDS
#   edge.tf        Cloudflare: DNS records, edge authentication
#   ops_bucket.tf  the operations bucket (backups) and the account's S3 block
#   hosted.tf      the hosted game servers' machines (ADR 0053)
#   outputs.tf     what the deploy toolbox and operators read
#   moved.tf       address moves from the single-file layout
#
# Workspaces are deployments: `default` is prod (its state and the vault's
# `tfstate` document predate workspaces), any other workspace the deployment
# of its name (`staging`, with the vault document `tfstate-staging`). The deploy
# toolbox selects the workspace from --deployment (deploy/bin/on-deploy).

terraform {
  # 1.11: write-only arguments (the database's password_wo) and ephemeral
  # variables. deploy/Dockerfile and .github/workflows/infra.yml pin the same
  # Terraform.
  required_version = ">= 1.11.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.100"
    }
    cloudflare = {
      source  = "cloudflare/cloudflare"
      version = "~> 4.0"
    }
  }
}

provider "aws" {
  region = var.region
}

provider "cloudflare" {
  api_token = var.cloudflare_api_token
}
