# The organization's settings, as they stood when the stack adopted them, apart
# from the billing email (a vault input).
resource "github_organization_settings" "org" {
  billing_email = var.billing_email
  name          = "opennova.net"

  has_organization_projects                = true
  has_repository_projects                  = true
  default_repository_permission            = "write"
  members_can_create_repositories          = true
  members_can_create_public_repositories   = true
  members_can_create_private_repositories  = true
  members_can_create_internal_repositories = false
  members_can_create_pages                 = true
  members_can_create_public_pages          = true
  members_can_create_private_pages         = true
  members_can_fork_private_repositories    = true
  web_commit_signoff_required              = false

  advanced_security_enabled_for_new_repositories               = false
  dependabot_alerts_enabled_for_new_repositories               = false
  dependabot_security_updates_enabled_for_new_repositories     = false
  dependency_graph_enabled_for_new_repositories                = false
  secret_scanning_enabled_for_new_repositories                 = false
  secret_scanning_push_protection_enabled_for_new_repositories = false
}

# The members and their roles: a vault input, since every member keeps the
# membership private and this repository is public. A login left out of the
# map is removed from the organization at the next apply.
resource "github_membership" "member" {
  for_each = var.members
  username = each.key
  role     = each.value
}

# Actions: every repository may run workflows, of any action; a workflow's
# GITHUB_TOKEN reads by default and never approves a pull request.
resource "github_actions_organization_permissions" "org" {
  enabled_repositories = "all"
  allowed_actions      = "all"
}

resource "github_actions_organization_workflow_permissions" "org" {
  organization_slug                = var.owner
  default_workflow_permissions     = "read"
  can_approve_pull_request_reviews = false
}
