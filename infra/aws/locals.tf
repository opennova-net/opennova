locals {
  # The deployment this state is: the workspace `default` is prod, any other
  # workspace the deployment of its name.
  deployment = terraform.workspace == "default" ? "prod" : terraform.workspace

  # Resource names. Prod keeps the names its resources were made with; another
  # deployment's carry the deployment's name, so two stacks in one account never
  # collide.
  name = terraform.workspace == "default" ? var.instance_name : "${var.instance_name}-${terraform.workspace}"

  # The account-wide singletons (the account's S3 public access block, the
  # operations bucket) belong to prod alone; another deployment uses them.
  manage_shared = terraform.workspace == "default"

  dns_enabled = var.cloudflare_zone_id != "" && var.cloudflare_domain != ""

  # The site's hostnames: prod the domain itself (and www), another deployment
  # a first-level name of its own (Cloudflare's universal certificate covers
  # first-level names only): staging.<domain>.
  web_host = local.manage_shared ? var.cloudflare_domain : "${local.deployment}.${var.cloudflare_domain}"

  # The two availability zones: var.azs, or the region's first two. Public and
  # private subnet a share the first (the service instance and its database), b the
  # second.
  azs = length(var.azs) == 2 ? var.azs : slice(data.aws_availability_zones.available.names, 0, 2)
}
