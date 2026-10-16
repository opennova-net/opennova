locals {
  # The deployment this state is (the toolbox passes TF_VAR_deployment): prod or
  # staging, each in an AWS account of its own.
  deployment = var.deployment
  is_prod    = var.deployment == "prod"

  # The deployment's account and region (the toolbox fills TF_VAR_aws_account_id
  # and TF_VAR_region from the vault: no account id is in a tracked file): the
  # provider refuses credentials of any other account (versions.tf).
  region = var.region

  # Resource names, every one carrying the deployment: opennova-prod-*,
  # opennova-staging-*. The same name is also every resource's
  # opennova:deployment tag (versions.tf) and the hosted machines' (fleet.tf).
  name = "opennova-${var.deployment}"

  dns_enabled = var.cloudflare_zone_id != "" && var.cloudflare_domain != ""

  # The site's hostnames: prod the domain itself (and www), staging a
  # first-level name of its own (Cloudflare's universal certificate covers
  # first-level names only): staging.<domain>.
  web_host = local.is_prod ? var.cloudflare_domain : "${local.deployment}.${var.cloudflare_domain}"

  # The two availability zones: var.azs, or the region's first two. Public and
  # private subnet a share the first (the load balancer's zone-a node, the
  # database's primary when single-AZ), b the second.
  azs = length(var.azs) == 2 ? var.azs : slice(data.aws_availability_zones.available.names, 0, 2)
}
