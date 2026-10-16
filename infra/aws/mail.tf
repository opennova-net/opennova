# ---------------------------------------------------------------------------
# Outbound mail: Amazon SES (DEPLOY.md, "Outbound mail (SES)")
# ---------------------------------------------------------------------------
#
# The service sends the welcome mail and the password reset links as
# <mail_sender>@<domain> (apps/novaworld_server/mailer.h: `sesv2 send-email`
# through the AWS CLI, on the service instance's role). Prod makes the domain's
# SES identity with Easy DKIM (its three CNAMEs in the Cloudflare zone) and a
# custom MAIL FROM domain, bounce.<domain> (its MX and SPF records), so the mail
# passes SPF and DKIM aligned with the domain; replies reach the support
# mailbox through Email Routing (email.tf). A new account's SES is in the
# sandbox: it sends only to verified addresses, 200 a day, until AWS grants
# production access (asked for in the console, once). So prod also makes an
# identity for the vault's email/forward_to address, which AWS asks its owner
# to verify by mail: mail to it works in the sandbox.
#
# Every deployment's service role may send from <mail_sender>@<domain> alone,
# through those identities (another deployment, such as staging, names prod's
# by ARN), and its infra.env names the sender (ONNET_MAIL_FROM) and SES's
# region (ONNET_MAIL_REGION). Off while var.mail_sender is empty or there is no
# Cloudflare: no identity, no record, no permission, and the service runs with
# mail off.

locals {
  mail_enabled     = local.dns_enabled && var.mail_sender != ""
  mail_from        = local.mail_enabled ? "${var.mail_sender}@${var.cloudflare_domain}" : ""
  mail_from_label  = "bounce"
  mail_from_domain = "${local.mail_from_label}.${var.cloudflare_domain}"
  # The domain's identity, and in the sandbox the forward_to address's (SES
  # checks the sender's identity and, while sandboxed, each recipient's).
  mail_identity_prefix = "arn:${local.partition}:ses:${var.region}:${local.account_id}:identity"
  mail_identities = local.mail_enabled ? concat(
    ["${local.mail_identity_prefix}/${var.cloudflare_domain}"],
    local.email_forward_to != "" ? ["${local.mail_identity_prefix}/${local.email_forward_to}"] : [],
  ) : []

  mail_env = local.mail_enabled ? {
    ONNET_MAIL_FROM   = local.mail_from
    ONNET_MAIL_REGION = var.region
  } : {}
}

# ---- the domain's identity (prod's) ---------------------------------------------

# Easy DKIM with SES's 2048-bit keys, named here so the plan knows the block
# whose tokens the CNAMEs below take.
resource "aws_sesv2_email_identity" "domain" {
  count          = local.mail_enabled && local.manage_shared ? 1 : 0
  email_identity = var.cloudflare_domain

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
  name    = "${aws_sesv2_email_identity.domain[0].dkim_signing_attributes[0].tokens[count.index]}._domainkey"
  content = "${aws_sesv2_email_identity.domain[0].dkim_signing_attributes[0].tokens[count.index]}.dkim.amazonses.com"
  type    = "CNAME"
  ttl     = 300
  proxied = false

  comment = "Managed by Terraform - OpenNova mail (SES DKIM)"
}

# The MAIL FROM domain: SES's feedback endpoint takes its bounces, and its SPF
# names SES (the apex's SPF is Email Routing's, which Cloudflare locks).
resource "cloudflare_record" "mail_from_mx" {
  count    = length(aws_sesv2_email_identity.domain)
  zone_id  = var.cloudflare_zone_id
  name     = local.mail_from_label
  content  = "feedback-smtp.${var.region}.amazonses.com"
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

# ---- the sandbox's recipient (prod's) -------------------------------------------

# The vault's email/forward_to address. AWS mails it a verification link; mail
# to it works once that is clicked, in the sandbox and out.
resource "aws_sesv2_email_identity" "forward_to" {
  count          = local.mail_enabled && local.manage_shared && local.email_forward_to != "" ? 1 : 0
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

resource "aws_iam_role_policy" "mail" {
  count  = local.mail_enabled ? 1 : 0
  name   = "${local.name}-mail"
  role   = aws_iam_role.service.id
  policy = data.aws_iam_policy_document.mail[0].json
}
