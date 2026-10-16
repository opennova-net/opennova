# The Cloudflare zone's shared settings (DEPLOY.md, "Deployments"): what every
# deployment's site goes through and no deployment owns. A zone holds one
# entrypoint ruleset per phase and one set of settings, so one stack holds them
# for prod and staging alike, the way infra/github holds the organization:
#
#   versions.tf  Terraform and provider pins, the provider
#   variables.tf every input
#   edge.tf      the zone's TLS settings, edge authentication (a rule per
#                deployment, each stamping its own secret on its own hostnames),
#                the Stripe webhook's way past the WAF
#   email.tf     Email Routing: support@<domain> to one mailbox
#
# Each deployment's own records (its site, its mail's DKIM and MAIL FROM) and
# its origin certificate stay in infra/aws, in that deployment's state. This
# stack has no deployment and makes nothing in AWS: the toolbox runs it as
# `./deploy/run.sh zone plan|apply|import|output|list`, its inputs from the
# vault (deploy/env/zone.env.tpl), its state in prod's state bucket (prod's
# account keeps what the deployments share), key infra/zone/terraform.tfstate,
# reached as prod's deploy user.

terraform {
  # The toolbox's Terraform (deploy/Dockerfile), as infra/aws.
  required_version = ">= 1.11.0"

  required_providers {
    cloudflare = {
      source  = "cloudflare/cloudflare"
      version = "~> 4.0"
    }
  }

  # The toolbox gives bucket (the vault's aws-prod/state_bucket), key
  # infra/zone/terraform.tfstate, region, use_lockfile and encrypt.
  backend "s3" {}
}

provider "cloudflare" {
  api_token = var.cloudflare_api_token
}
