# ---------------------------------------------------------------------------
# The organization and its member accounts
# ---------------------------------------------------------------------------
#
# The maintainer made the organization and its accounts by hand (one per
# deployment, and ci, which holds the CI runners: infra/ci); the import blocks
# adopt them, so an apply against the existing organization never makes one
# again (and a state rebuilt from nothing adopts them the same way). A new
# deployment's account is a change here (a member, its provider in
# versions.tf, its module call in accounts.tf, its vault item) and an apply.
# Nothing here can be destroyed by a plan: prevent_destroy, and an account
# leaving the stack is never closed (close_on_deletion off).

import {
  to = aws_organizations_organization.org
  id = var.organization_id
}

# Every feature (consolidated billing and the rest). The trusted services and
# the policy types stay as the console sets them: this stack adopts the
# organization, it does not decide those.
resource "aws_organizations_organization" "org" {
  feature_set = "ALL"

  lifecycle {
    prevent_destroy = true
    ignore_changes  = [aws_service_access_principals, enabled_policy_types]
  }
}

import {
  for_each = local.members
  to       = aws_organizations_account.member[each.key]
  id       = each.value.account_id
}

# Each member account. role_name and iam_user_access_to_billing are set
# at creation alone and cannot be read back, so an imported account ignores
# them.
resource "aws_organizations_account" "member" {
  for_each = local.members

  name                       = each.value.name
  email                      = each.value.email
  role_name                  = "OrganizationAccountAccessRole"
  iam_user_access_to_billing = "ALLOW"
  close_on_deletion          = false

  tags = {
    "opennova:deployment" = "opennova-${each.key}"
  }

  lifecycle {
    prevent_destroy = true
    ignore_changes  = [role_name, iam_user_access_to_billing]
  }
}
