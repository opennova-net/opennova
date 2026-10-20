variable "owner" {
  description = "The GitHub organization the stack manages."
  type        = string
  default     = "opennova-net"
}

variable "repository" {
  description = "The repository (in owner) whose Actions secrets and deploy environments the stack makes."
  type        = string
  default     = "opennova"
}

variable "billing_email" {
  description = "The organization's billing email (org.tf): a vault input (the vault item github-terraform, field billing_email), kept out of this public repository."
  type        = string
}

variable "members" {
  description = "The organization's members, login => role (member or admin; org.tf): a vault input (the vault item github-terraform, field members, as JSON), since every member keeps the membership private and this repository is public."
  type        = map(string)

  validation {
    condition     = alltrue([for role in values(var.members) : contains(["member", "admin"], role)])
    error_message = "A member's role is member or admin."
  }
}

variable "stripe_test_secret_key" {
  description = "The STRIPE_TEST_SECRET_KEY secret's value (secrets.tf): a Stripe test-mode key (rk_test_ restricted to Customers, Checkout Sessions, Subscriptions and Customer portal write and Prices and Products read; or sk_test_), or the vault's REPLACE_ME, with which the billing end-to-end test skips. The toolbox reads it from the vault item stripe-test, field restricted_key (deploy/env/github.env.tpl)."
  type        = string
  sensitive   = true
}

variable "aws_account_id_prod" {
  description = "prod's AWS account (environments.tf: the prod environment's AWS_ACCOUNT_ID and its deploy role's ARN): a vault input (the vault item aws-prod, field account_id)."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id_prod))
    error_message = "aws_account_id_prod is a 12-digit AWS account id."
  }
}

variable "aws_region_prod" {
  description = "prod's region (the prod environment's AWS_REGION): a vault input (the vault item aws-prod, field region)."
  type        = string

  validation {
    condition     = can(regex("^[a-z]{2}(-[a-z]+)+-[0-9]$", var.aws_region_prod))
    error_message = "aws_region_prod is an AWS region name (us-east-1)."
  }
}

variable "aws_account_id_staging" {
  description = "staging's AWS account (environments.tf: the staging environment's AWS_ACCOUNT_ID and its deploy role's ARN): a vault input (the vault item aws-staging, field account_id)."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id_staging))
    error_message = "aws_account_id_staging is a 12-digit AWS account id."
  }
}

variable "aws_region_staging" {
  description = "staging's region (the staging environment's AWS_REGION): a vault input (the vault item aws-staging, field region)."
  type        = string

  validation {
    condition     = can(regex("^[a-z]{2}(-[a-z]+)+-[0-9]$", var.aws_region_staging))
    error_message = "aws_region_staging is an AWS region name (us-east-1)."
  }
}
