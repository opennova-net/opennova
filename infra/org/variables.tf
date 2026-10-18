# Every input is the vault's (through the toolbox): no tracked file names an
# account, the organization or an address.

variable "region" {
  description = "The region of every account's resources (vault item aws-org, field region)."
  type        = string

  validation {
    condition     = can(regex("^[a-z]{2}(-[a-z]+)+-[0-9]$", var.region))
    error_message = "region is an AWS region name (us-east-1)."
  }
}

variable "organization_id" {
  description = "The AWS Organization the management account runs (vault item aws-org, field organization_id): the import block adopts it."
  type        = string

  validation {
    condition     = can(regex("^o-[a-z0-9]{10,32}$", var.organization_id))
    error_message = "organization_id is an organization id (o-...)."
  }
}

variable "management_account_id" {
  description = "The organization's management account, where opennova-org runs this stack (vault item aws-org, field account_id): every run refuses credentials of another account."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.management_account_id))
    error_message = "management_account_id is a 12-digit AWS account id."
  }
}

variable "aws_account_id_prod" {
  description = "opennova-prod's account id (vault item aws-prod, field account_id): the import block adopts it, and its provider assumes its OrganizationAccountAccessRole."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id_prod))
    error_message = "aws_account_id_prod is a 12-digit AWS account id."
  }
}

variable "aws_account_id_staging" {
  description = "opennova-staging's account id (vault item aws-staging, field account_id)."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id_staging))
    error_message = "aws_account_id_staging is a 12-digit AWS account id."
  }
}

variable "aws_account_id_ci" {
  description = "opennova-ci's account id (vault item aws-ci, field account_id): the account of the CI runners (infra/ci), adopted like the deployments' accounts."
  type        = string

  validation {
    condition     = can(regex("^[0-9]{12}$", var.aws_account_id_ci))
    error_message = "aws_account_id_ci is a 12-digit AWS account id."
  }
}

variable "account_name_prod" {
  description = "opennova-prod's account name (vault item aws-prod, field account_name)."
  type        = string

  validation {
    condition     = can(regex("^[A-Za-z0-9][A-Za-z0-9 ._-]{0,49}$", var.account_name_prod))
    error_message = "account_name_prod is an account name (letters, digits, space, '.', '_', '-'; at most 50)."
  }
}

variable "account_name_staging" {
  description = "opennova-staging's account name (vault item aws-staging, field account_name)."
  type        = string

  validation {
    condition     = can(regex("^[A-Za-z0-9][A-Za-z0-9 ._-]{0,49}$", var.account_name_staging))
    error_message = "account_name_staging is an account name (letters, digits, space, '.', '_', '-'; at most 50)."
  }
}

variable "account_name_ci" {
  description = "opennova-ci's account name (vault item aws-ci, field account_name)."
  type        = string

  validation {
    condition     = can(regex("^[A-Za-z0-9][A-Za-z0-9 ._-]{0,49}$", var.account_name_ci))
    error_message = "account_name_ci is an account name (letters, digits, space, '.', '_', '-'; at most 50)."
  }
}

variable "root_email_prod" {
  description = "opennova-prod's root user's address (vault item aws-prod, field root_email; infra/zone forwards it)."
  type        = string

  validation {
    condition     = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.root_email_prod))
    error_message = "root_email_prod is an email address."
  }
}

variable "root_email_staging" {
  description = "opennova-staging's root user's address (vault item aws-staging, field root_email)."
  type        = string

  validation {
    condition     = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.root_email_staging))
    error_message = "root_email_staging is an email address."
  }
}

variable "root_email_ci" {
  description = "opennova-ci's root user's address (vault item aws-ci, field root_email; infra/zone forwards it)."
  type        = string

  validation {
    condition     = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.root_email_ci))
    error_message = "root_email_ci is an email address."
  }
}

variable "github_repository" {
  description = "The repository (owner/name) whose deploy workflow may assume each account's GitHub deploy role, from its environment of the deployment's name alone."
  type        = string
  default     = "opennova-net/opennova"

  validation {
    condition     = can(regex("^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$", var.github_repository))
    error_message = "github_repository takes owner/name."
  }
}

# The maintainer, Identity Center's one user (identity_center.tf).
variable "sso_given_name" {
  description = "The maintainer's given name in IAM Identity Center (vault item aws-org, field sso_given_name)."
  type        = string

  validation {
    condition     = length(trimspace(var.sso_given_name)) > 0
    error_message = "sso_given_name is a name."
  }
}

variable "sso_family_name" {
  description = "The maintainer's family name in IAM Identity Center (vault item aws-org, field sso_family_name)."
  type        = string

  validation {
    condition     = length(trimspace(var.sso_family_name)) > 0
    error_message = "sso_family_name is a name."
  }
}

variable "sso_email" {
  description = "The maintainer's address in IAM Identity Center, also the user name the portal signs in with (vault item aws-org, field sso_email)."
  type        = string

  validation {
    condition     = can(regex("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$", var.sso_email))
    error_message = "sso_email is an email address."
  }
}
