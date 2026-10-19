# ---------------------------------------------------------------------------
# Cloudflare: the deployment's site records and its origin's TLS (DEPLOY.md,
# section 4, "Cloudflare: TLS to the origin"), and the static addresses' zone
# ---------------------------------------------------------------------------
#
# Cloudflare proxies the site's hostnames and talks to the origin (the portal's
# nginx, port 443, which admits Cloudflare's ranges alone) over TLS it verifies
# ("Full (strict)"): the origin presents a Cloudflare Origin CA certificate
# made here, whose key Terraform keeps in its state and hands web's portal
# container through SSM (SecureString, /opennova/<deployment>/tls/*, which ECS
# puts in the container's environment at its start; its entrypoint writes them
# under ORIGIN_TLS_DIR). Site passwords never cross the internet in clear. The
# retail HTTP port (8080) is no part of this: stock clients speak plain HTTP to
# it.
#
# The zone's own settings ("Full (strict)", Always Use HTTPS), its entrypoint
# rulesets (edge authentication, one rule per deployment with the deployment's
# own secret; the webhook's WAF skip) and its Email Routing are the zone's, one
# set for every deployment: infra/zone's stack, not this one.

# The site's records, both at the public load balancer (load_balancer.tf):
# prod's @ and www; staging's `staging` (locals.tf); and the update service's,
# below. CNAMEs to its
# DNS name (Cloudflare flattens the apex's), whose answers are its two zones'
# Elastic IPs. Proxied by default so the site gets Cloudflare TLS. Retail
# clients follow no name to the gate (gs.novaworld.net, the Switcher's hosts
# line) and the gate announces the balancer's zone-a address (ONNET_WEB_HOST,
# service.tf); OpenNova's own client follows the gate's name, below.
resource "cloudflare_record" "web_root" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.is_prod ? "@" : local.deployment
  content = aws_lb.public.dns_name
  type    = "CNAME"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web"
}

resource "cloudflare_record" "web_www" {
  count   = local.dns_enabled && local.is_prod ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = "www"
  content = aws_lb.public.dns_name
  type    = "CNAME"
  ttl     = var.cloudflare_web_proxied ? 1 : 300
  proxied = var.cloudflare_web_proxied

  comment = "Managed by Terraform - OpenNova web (www)"
}

# The update service's name (local.update_host), never proxied: retail
# UPDATE.EXE speaks plain TCP on the updater's ports (load_balancer.tf), which
# Cloudflare's proxy would not pass, so the name resolves straight to the
# balancer. The service's SERVER.LST sends the updater here for a data set
# served by OpenNova's update service; one served the retail way names
# ftp.novaworld.com, which the Switcher's managed block maps to the gate's
# address, the balancer's zone-a address (ADR 0064).
resource "cloudflare_record" "update" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.is_prod ? "update" : "update.${local.deployment}"
  content = aws_lb.public.dns_name
  type    = "CNAME"
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - OpenNova update service (plain TCP, never proxied)"
}

# The gate's name (local.novaworld_host), never proxied: the gate speaks UDP,
# which Cloudflare's proxy would not pass. An A record at the balancer's zone-a
# address, the one the gate announces and /api/server-info reports (the
# Switcher's hosts line), so it names the gate alone, never zone b. OpenNova's
# own client dials it for its OpenNova NovaWorld (ADR 0010 d2), and a
# self-run opennova-serve lists on it with --master-host.
resource "cloudflare_record" "novaworld" {
  count   = local.dns_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.is_prod ? "nw" : "nw.${local.deployment}"
  content = local.service_public_ip
  type    = "A"
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - OpenNova NovaWorld gate (UDP, never proxied)"
}

locals {
  # Cloudflare in front of the origin: proxied records on a configured zone.
  edge_proxied = local.dns_enabled && var.cloudflare_web_proxied

  # The deployment's web hostnames: prod's domain and www, staging's
  # staging.<domain>. Its origin certificate names these alone, so staging's
  # key is good for no name of prod's.
  site_hosts = local.is_prod ? [var.cloudflare_domain, "www.${var.cloudflare_domain}"] : [local.web_host]
}

# ---- the origin's certificate --------------------------------------------------

resource "tls_private_key" "origin" {
  count       = local.edge_proxied ? 1 : 0
  algorithm   = "ECDSA"
  ecdsa_curve = "P256"
}

resource "tls_cert_request" "origin" {
  count           = local.edge_proxied ? 1 : 0
  private_key_pem = tls_private_key.origin[0].private_key_pem

  subject {
    common_name  = local.web_host
    organization = "OpenNova"
  }
}

# A Cloudflare Origin CA certificate (trusted by Cloudflare alone, which is the
# point: only Cloudflare talks to this origin) for the deployment's web
# hostnames alone, 15 years. The API token needs Zone / SSL and Certificates /
# Edit. Renewing is a -replace of it, then an app deploy.
resource "cloudflare_origin_ca_certificate" "origin" {
  count              = local.edge_proxied ? 1 : 0
  csr                = tls_cert_request.origin[0].cert_request_pem
  hostnames          = local.site_hosts
  request_type       = "origin-ecc"
  requested_validity = 5475
}

# The certificate and its key for the portal, base64 (one line each, as its
# entrypoint takes them): ECS reads /opennova/<deployment>/tls/* into the
# portal container's environment beside its secrets (programs.tf).
resource "aws_ssm_parameter" "origin_tls_cert" {
  count       = local.edge_proxied ? 1 : 0
  name        = "${local.ssm_prefix}/tls/ORIGIN_TLS_CERT"
  description = "The portal's Cloudflare Origin CA certificate (base64 PEM; infra/aws/edge.tf)"
  type        = "SecureString"
  tier        = "Standard"
  value       = base64encode(cloudflare_origin_ca_certificate.origin[0].certificate)

  tags = var.tags
}

resource "aws_ssm_parameter" "origin_tls_key" {
  count       = local.edge_proxied ? 1 : 0
  name        = "${local.ssm_prefix}/tls/ORIGIN_TLS_KEY"
  description = "The portal's origin certificate's private key (base64 PEM; infra/aws/edge.tf)"
  type        = "SecureString"
  tier        = "Standard"
  value       = base64encode(tls_private_key.origin[0].private_key_pem)

  tags = var.tags
}

# ---- the static addresses' zone -------------------------------------------------

# The static-address add-on (DEPLOY.md, "Hosted game servers"): an own
# machine's Elastic IP and its name, <name>.<zone>, an A record the worker
# writes and deletes. The zone is a Route 53 public zone of the deployment's
# own, play.<domain> on prod and play.<deployment>.<domain> off it
# (local.play_zone_name), delegated from the Cloudflare zone by its four name
# servers: Route 53, because IAM holds the worker to the zone's A records
# (fleet.tf), where a Cloudflare token would reach the whole zone. Made while
# the add-on is sold (fleet_max_addresses above 0) with hosting on and a
# Cloudflare zone, and never destroyed by a plan (prevent_destroy stops one
# that would, fleet_max_addresses back at 0 among them): a new zone gets other
# name servers, and every address's name stops resolving until the delegation
# follows them.
locals {
  play_zone_enabled = local.dns_enabled && local.hosted_enabled && var.fleet_max_addresses > 0
}

resource "aws_route53_zone" "play" {
  count   = local.play_zone_enabled ? 1 : 0
  name    = local.play_zone_name
  comment = "OpenNova ${local.deployment}: the static addresses' names (infra/aws/edge.tf)"

  tags = var.tags

  lifecycle {
    prevent_destroy = true
  }
}

# Route 53 gives a zone four name servers, fixed for its life.
resource "cloudflare_record" "play_ns" {
  count   = local.play_zone_enabled ? 4 : 0
  zone_id = var.cloudflare_zone_id
  name    = local.is_prod ? "play" : "play.${local.deployment}"
  content = aws_route53_zone.play[0].name_servers[count.index]
  type    = "NS"
  ttl     = 300

  comment = "Managed by Terraform - OpenNova static addresses (delegation to Route 53)"

  lifecycle {
    prevent_destroy = true
  }
}
