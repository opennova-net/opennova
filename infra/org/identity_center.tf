# ---------------------------------------------------------------------------
# People in the member accounts: IAM Identity Center
# ---------------------------------------------------------------------------
#
# The maintainer reaches the member accounts' consoles through IAM Identity
# Center's portal, never as a member account's root or an IAM user. The
# organization's Identity Center instance is enabled once by hand, in the
# console, in the stack's region (DEPLOY.md, "The Organization and the accounts"); this stack reads
# it and makes two permission sets (administrator, 4-hour sessions; read only),
# one user (the maintainer: name and address from the vault), and both sets
# assigned to that user in both member accounts. No groups, no service control
# policies.

data "aws_ssoadmin_instances" "org" {}

locals {
  sso_instance_arn  = tolist(data.aws_ssoadmin_instances.org.arns)[0]
  identity_store_id = tolist(data.aws_ssoadmin_instances.org.identity_store_ids)[0]

  permission_sets = {
    admin    = { name = "OpenNovaAdmin", policy = "AdministratorAccess", session = "PT4H", description = "OpenNova: administrator of a member account (4-hour sessions)" }
    readonly = { name = "OpenNovaReadOnly", policy = "ReadOnlyAccess", session = "PT1H", description = "OpenNova: read-only in a member account" }
  }

  # Each permission set in each member account.
  assignments = {
    for pair in setproduct(keys(local.permission_sets), keys(local.members)) :
    "${pair[0]}/${pair[1]}" => { set = pair[0], deployment = pair[1] }
  }
}

resource "aws_ssoadmin_permission_set" "set" {
  for_each         = local.permission_sets
  instance_arn     = local.sso_instance_arn
  name             = each.value.name
  description      = each.value.description
  session_duration = each.value.session
}

resource "aws_ssoadmin_managed_policy_attachment" "set" {
  for_each           = local.permission_sets
  instance_arn       = local.sso_instance_arn
  permission_set_arn = aws_ssoadmin_permission_set.set[each.key].arn
  managed_policy_arn = "arn:aws:iam::aws:policy/${each.value.policy}"
}

resource "aws_identitystore_user" "maintainer" {
  identity_store_id = local.identity_store_id
  user_name         = var.sso_email
  display_name      = "${var.sso_given_name} ${var.sso_family_name}"

  name {
    given_name  = var.sso_given_name
    family_name = var.sso_family_name
  }

  emails {
    value   = var.sso_email
    primary = true
  }
}

resource "aws_ssoadmin_account_assignment" "maintainer" {
  for_each           = local.assignments
  instance_arn       = local.sso_instance_arn
  permission_set_arn = aws_ssoadmin_permission_set.set[each.value.set].arn
  principal_type     = "USER"
  principal_id       = aws_identitystore_user.maintainer.user_id
  target_type        = "AWS_ACCOUNT"
  target_id          = aws_organizations_account.member[each.value.deployment].id

  depends_on = [aws_ssoadmin_managed_policy_attachment.set]
}
