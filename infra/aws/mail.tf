# ---------------------------------------------------------------------------
# Outbound mail: Amazon SES (DEPLOY.md, "Outbound mail (SES)")
# ---------------------------------------------------------------------------
#
# The service sends the welcome mail and the password reset links as
# <mail_sender>@<web host> (`sesv2 send-email` through the AWS CLI, with the
# worker's task role: web queues them as mail.send jobs, a worker's to send).
# Each deployment sends through SES identities of its own, in its own account:
# prod as support@<domain>, staging as support@staging.<domain>, so a staging
# mail is never one of prod's and links to staging's site alone
# (ONNET_SITE_URL, programs.tf). The identity is the deployment's web host with
# Easy DKIM (its three CNAMEs in the Cloudflare zone, under that host) and a
# custom MAIL FROM domain, bounce.<web host> (its MX and SPF records), so the
# mail passes SPF and DKIM aligned with it; replies to prod's support address
# reach the support mailbox through Email Routing (infra/zone). A new
# account's SES is in the sandbox: it sends only to verified addresses, 200 a
# day, until AWS grants production access (asked for in the console, once per
# account). So each deployment also makes an identity for the vault's
# email/forward_to address, which AWS asks its owner to verify by mail: mail to
# it works in the sandbox.
#
# The worker may send from <mail_sender>@<web host> alone (the policy's
# ses:FromAddress condition), to any recipient, through any identity of the
# account; web's environment and the worker's name the sender
# (ONNET_MAIL_FROM) and SES's region (ONNET_MAIL_REGION), web's to decide
# whether it queues mail at all. Off while var.mail_sender is empty or there is
# no Cloudflare: no identity, no record, no permission, and the service runs
# with mail off.

locals {
  email_forward_to = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.email_forward_to)) ? var.email_forward_to : ""

  mail_enabled  = local.dns_enabled && var.mail_sender != ""
  mail_identity = local.web_host
  mail_from     = local.mail_enabled ? "${var.mail_sender}@${local.mail_identity}" : ""
  # The records' names in the zone: prod's at the apex, staging's under
  # staging (bounce.staging, <token>._domainkey.staging).
  mail_record_suffix = local.is_prod ? "" : ".${local.deployment}"
  mail_from_label    = "bounce${local.mail_record_suffix}"
  mail_from_domain   = "bounce.${local.mail_identity}"
  # Every identity of the account: SES authorizes SendEmail against the sender's
  # identity AND against each recipient that is itself a verified identity of
  # the account (an address someone verified in the console, say), so a list of
  # named identities refused mail to such a recipient (prod, 2026-10-18:
  # AccessDenied on identity/<a verified address>). The ses:FromAddress
  # condition below is what pins the sender.
  mail_identity_prefix = "arn:${local.partition}:ses:${local.region}:${local.account_id}:identity"
  mail_identities      = local.mail_enabled ? ["${local.mail_identity_prefix}/*"] : []

  mail_env = local.mail_enabled ? {
    ONNET_MAIL_FROM   = local.mail_from
    ONNET_MAIL_REGION = local.region
  } : {}
}

# ---- the deployment's identity ----------------------------------------------------

# Easy DKIM with SES's 2048-bit keys, named here so the plan knows the block
# whose tokens the CNAMEs below take.
resource "aws_sesv2_email_identity" "domain" {
  count          = local.mail_enabled ? 1 : 0
  email_identity = local.mail_identity

  dkim_signing_attributes {
    next_signing_key_length = "RSA_2048_BIT"
  }
}

resource "aws_sesv2_email_identity_mail_from_attributes" "domain" {
  count                  = length(aws_sesv2_email_identity.domain)
  email_identity         = aws_sesv2_email_identity.domain[0].email_identity
  mail_from_domain       = local.mail_from_domain
  behavior_on_mx_failure = "USE_DEFAULT_VALUE"
}

# Easy DKIM: three CNAMEs, <token>._domainkey to <token>.dkim.amazonses.com. DNS
# only (a proxied record carries no CNAME to the outside).
resource "cloudflare_record" "mail_dkim" {
  count   = length(aws_sesv2_email_identity.domain) > 0 ? 3 : 0
  zone_id = var.cloudflare_zone_id
  name    = "${aws_sesv2_email_identity.domain[0].dkim_signing_attributes[0].tokens[count.index]}._domainkey${local.mail_record_suffix}"
  content = "${aws_sesv2_email_identity.domain[0].dkim_signing_attributes[0].tokens[count.index]}.dkim.amazonses.com"
  type    = "CNAME"
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - OpenNova mail (SES DKIM)"
}

# The MAIL FROM domain: SES's feedback endpoint takes its bounces, and its SPF
# names SES (prod's apex SPF is Email Routing's, which Cloudflare locks).
resource "cloudflare_record" "mail_from_mx" {
  count    = length(aws_sesv2_email_identity.domain)
  zone_id  = var.cloudflare_zone_id
  name     = local.mail_from_label
  content  = "feedback-smtp.${local.region}.amazonses.com"
  type     = "MX"
  priority = 10
  ttl      = 300

  comment = "Managed by Terraform - OpenNova mail (SES MAIL FROM)"
}

resource "cloudflare_record" "mail_from_spf" {
  count   = length(aws_sesv2_email_identity.domain)
  zone_id = var.cloudflare_zone_id
  name    = local.mail_from_label
  content = "v=spf1 include:amazonses.com ~all"
  type    = "TXT"
  ttl     = 300

  comment = "Managed by Terraform - OpenNova mail (SES MAIL FROM)"
}

# ---- the sandbox's recipient ------------------------------------------------------

# The vault's email/forward_to address, in this deployment's account. AWS mails
# it a verification link; mail to it works once that is clicked, in the
# sandbox and out.
resource "aws_sesv2_email_identity" "forward_to" {
  count          = local.mail_enabled && local.email_forward_to != "" ? 1 : 0
  email_identity = local.email_forward_to
}

# ---- what the service may send ---------------------------------------------------

data "aws_iam_policy_document" "mail" {
  count = local.mail_enabled ? 1 : 0

  statement {
    sid       = "SendMail"
    effect    = "Allow"
    actions   = ["ses:SendEmail"]
    resources = local.mail_identities

    condition {
      test     = "StringEquals"
      variable = "ses:FromAddress"
      values   = [local.mail_from]
    }
  }
}

# The worker sends every mail (the mail.send jobs web queues for the
# registration and the reset links); web sends none.
resource "aws_iam_role_policy" "mail" {
  for_each = local.mail_enabled ? toset(["worker"]) : toset([])
  name     = "${local.name}-${each.key}-mail"
  role     = aws_iam_role.program[each.key].id
  policy   = data.aws_iam_policy_document.mail[0].json
}
