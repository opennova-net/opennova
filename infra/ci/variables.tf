# Every input but the defaults is the vault's (through the toolbox,
# deploy/env/ci.env.tpl): no tracked file names the account or holds the key.

variable "region" {
  description = "The ci account's region (vault item aws-ci, field region)."
  type        = string

  validation {
    condition     = can(regex("^[a-z]{2}(-[a-z]+)+-[0-9]$", var.region))
    error_message = "region is an AWS region name (us-east-1)."
  }
}

variable "aws_account_id_ci" {
  description = "opennova-ci's account id (vault item aws-ci, field account_id): the provider assumes its OrganizationAccountAccessRole and acts there alone."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id_ci))
    error_message = "aws_account_id_ci is a 12-digit AWS account id."
  }
}

variable "license_key" {
  description = "RunsOn's license key (vault item runs-on, field key)."
  type        = string
  sensitive   = true

  validation {
    condition     = !contains(["", "REPLACE_ME"], trimspace(var.license_key))
    error_message = "license_key is RunsOn's license key (runs-on.com)."
  }
}

variable "alert_email" {
  description = "Where RunsOn's alerts and weekly cost report and the account's budget write (vault item alerts, field email). RunsOn's alert topic emails it a subscription to confirm once."
  type        = string

  validation {
    condition     = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.alert_email))
    error_message = "alert_email is an email address."
  }
}

variable "github_organization" {
  description = "The GitHub organization whose workflows the runners serve: the GitHub App the setup page registers is its, installed on the repositories chosen there."
  type        = string
  default     = "opennova-net"
}

variable "budget_monthly_usd" {
  description = "The ci account's monthly cost budget, in USD: an email at 80% spent and at a 100% forecast. The account holds the runners alone, so this is CI's whole AWS bill."
  type        = number
  default     = 300
}
