# ---------------------------------------------------------------------------
# Email for the domain: Cloudflare Email Routing (DEPLOY.md, "Email")
# ---------------------------------------------------------------------------
#
# Mail to support@<domain>, and to each local part var.email_routes names,
# forwards to one mailbox, var.email_forward_to (from the vault: item email,
# field forward_to). Turning Email Routing on makes Cloudflare add and lock the
# zone's MX and SPF records; the destination receives a verification email
# from Cloudflare, and nothing is forwarded until its link is clicked.
# Receive-only: replies leave from the destination mailbox, and sending as
# support@ needs an outbound mail provider, which this stack does not make.
# Addresses no rule names are refused (the zone's catch-all stays off).
#
# The zone's, so prod's alone (manage_shared); off without Cloudflare, and while
# the destination is empty or REPLACE_ME. The API token needs Zone / Email
# Routing Rules / Edit (the rules), Zone / Zone Settings / Edit (turning it on)
# and Account / Email Routing Addresses / Edit (the destination).

locals {
  email_forward_to = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.email_forward_to)) ? var.email_forward_to : ""
  email_enabled    = local.manage_shared && local.dns_enabled && local.email_forward_to != ""
}

# The zone's account: destination addresses are the account's, not the zone's.
data "cloudflare_zone" "email" {
  count   = local.email_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
}

resource "cloudflare_email_routing_settings" "zone" {
  count   = local.email_enabled ? 1 : 0
  zone_id = var.cloudflare_zone_id
  enabled = true
}

resource "cloudflare_email_routing_address" "forward_to" {
  count      = local.email_enabled ? 1 : 0
  account_id = data.cloudflare_zone.email[0].account_id
  email      = local.email_forward_to

  lifecycle {
    # Forwarding into the domain itself would loop.
    precondition {
      condition     = !endswith(lower(local.email_forward_to), "@${lower(var.cloudflare_domain)}")
      error_message = "email_forward_to is an address at ${var.cloudflare_domain} itself: Email Routing would forward its mail back to itself. Name a mailbox elsewhere (vault item email, field forward_to)."
    }
  }
}

# One rule per local part: a literal match on the recipient, forwarded to the
# destination.
resource "cloudflare_email_routing_rule" "route" {
  for_each = local.email_enabled ? toset(var.email_routes) : toset([])
  zone_id  = var.cloudflare_zone_id
  name     = "OpenNova: ${each.key}@${var.cloudflare_domain}"
  enabled  = true

  matcher {
    type  = "literal"
    field = "to"
    value = "${each.key}@${var.cloudflare_domain}"
  }

  action {
    type  = "forward"
    value = [cloudflare_email_routing_address.forward_to[0].email]
  }

  depends_on = [cloudflare_email_routing_settings.zone]
}
