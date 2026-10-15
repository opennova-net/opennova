# The NovaWorld service's AWS + Cloudflare infrastructure (DEPLOY.md, section 3).
#
# Flat files by concern, no child modules:
#   versions.tf    Terraform and provider pins, provider configuration
#   locals.tf      the deployment (workspace) and the names derived from it
#   variables.tf   every input
#   network.tf     the VPC: public subnets a/b (service, machines), private a/b
#   service.tf     the service host
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
  required_version = ">= 1.9.0"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
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
