variable "cloudflare_api_token" {
  description = "Cloudflare API token with the zone permissions DEPLOY.md lists (section 1): Zone Settings, Zone WAF, Transform Rules, Email Routing Rules, and the account's Email Routing Addresses. From the vault (TF_VAR_cloudflare_api_token: item cloudflare, field api_token)."
  type        = string
  default     = ""
  sensitive   = true
}

variable "cloudflare_zone_id" {
  description = "The zone's id (vault item cloudflare, field zone_id)."
  type        = string

  validation {
    condition     = can(regex("^[0-9a-f]{32}$", var.cloudflare_zone_id))
    error_message = "cloudflare_zone_id is the zone's 32 hex digit id."
  }
}

variable "cloudflare_domain" {
  description = "The zone's domain (vault item cloudflare, field domain): prod's site is the domain and www.<domain>, staging's staging.<domain>."
  type        = string

  validation {
    condition     = can(regex("^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$", var.cloudflare_domain))
    error_message = "cloudflare_domain is a lowercase domain name."
  }
}

# Edge authentication: a Transform Rule stamps X-OpenNova-Edge: <secret> on the
# requests for a deployment's web hostnames, and that deployment's portal
# nginx (EDGE_AUTH_SECRET, the same value) answers /api/ only to requests
# carrying it. Each deployment has its own: staging's never opens prod's
# origin, nor the reverse. Letters and digits, at least 32 (web/docker-entrypoint.sh's
# rule); `secrets init` makes 64 hex digits. The state holds them in plain text.
variable "edge_auth_secret_prod" {
  description = "Prod's edge authentication secret (TF_VAR_edge_auth_secret_prod: vault item app-prod, field edge_auth_secret)."
  type        = string
  sensitive   = true

  validation {
    condition     = can(regex("^[A-Za-z0-9]{32,}$", var.edge_auth_secret_prod))
    error_message = "edge_auth_secret_prod must be at least 32 letters and digits (secrets init makes it in app-prod)."
  }
}

variable "edge_auth_secret_staging" {
  description = "Staging's edge authentication secret (TF_VAR_edge_auth_secret_staging: vault item app-staging, field edge_auth_secret)."
  type        = string
  sensitive   = true

  validation {
    condition     = can(regex("^[A-Za-z0-9]{32,}$", var.edge_auth_secret_staging))
    error_message = "edge_auth_secret_staging must be at least 32 letters and digits (secrets init --deployment staging makes it in app-staging)."
  }
}

variable "cloudflare_webhook_skip_phases" {
  description = "What the Stripe webhook's custom rule skips besides the remaining custom rules: these phases. Drop one the zone's plan does not offer (the apply says which)."
  type        = list(string)
  default     = ["http_ratelimit", "http_request_firewall_managed", "http_request_sbfm"]
}

variable "cloudflare_webhook_skip_products" {
  description = "And these legacy security products. Drop one the zone's plan does not offer."
  type        = list(string)
  default     = ["bic", "hot", "securityLevel", "uaBlock", "zoneLockdown"]
}

variable "email_forward_to" {
  description = "The mailbox Cloudflare Email Routing forwards the domain's addresses to (email.tf): support@<domain> and every email_routes local part. From the vault (TF_VAR_email_forward_to: item email, field forward_to); empty or REPLACE_ME turns email routing off. Cloudflare emails it a verification link once; nothing is forwarded before it is clicked."
  type        = string
  default     = ""

  validation {
    condition     = contains(["", "REPLACE_ME"], var.email_forward_to) || can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.email_forward_to))
    error_message = "email_forward_to must be an email address, or empty or REPLACE_ME (email routing off)."
  }
}

variable "email_routes" {
  description = "The domain's local parts that forward to email_forward_to: support@<domain>, and aws-prod@, aws-staging@ and aws-ci@, the member accounts' root addresses (AWS mails each account's root user there: billing, security, recovery). Lowercase; every other address is refused."
  type        = list(string)
  default     = ["support", "aws-prod", "aws-staging", "aws-ci"]

  validation {
    condition     = alltrue([for l in var.email_routes : can(regex("^[a-z0-9]([a-z0-9._+-]{0,62}[a-z0-9])?$", l))])
    error_message = "email_routes takes lowercase local parts (letters, digits, '.', '_', '+' and '-', not at either end), at most 64 each."
  }
}
