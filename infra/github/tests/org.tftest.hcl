# The organization, its repositories and master's protection (org.tf,
# repositories.tf, protection.tf), planned against a mock provider: offline, no
# token, nothing reaches GitHub.

mock_provider "github" {
  override_during = plan

  # The prod environment's approvers (environments.tf): a user id is a number.
  mock_data "github_user" {
    defaults = { id = "4242" }
  }
}

# imports.tf adopts these, and a mock provider cannot import: each is stood in
# for at plan instead.
override_resource {
  target          = github_organization_settings.org
  override_during = plan
}
override_resource {
  target          = github_membership.member
  override_during = plan
}
override_resource {
  target          = github_actions_organization_permissions.org
  override_during = plan
}
override_resource {
  target          = github_actions_organization_workflow_permissions.org
  override_during = plan
}
override_resource {
  target          = github_repository.repo
  override_during = plan
  values          = { node_id = "R_mock" }
}
override_resource {
  target          = github_branch_default.repo
  override_during = plan
}
override_resource {
  target          = github_repository_vulnerability_alerts.repo
  override_during = plan
}
override_resource {
  target          = github_branch_protection.opennova_master
  override_during = plan
}

variables {
  billing_email          = "billing@example.com"
  members                = { mock-admin = "admin", mock-member = "member" }
  reference_assets_token = "github_pat_mock"
  stripe_test_secret_key = "rk_test_mock"
  aws_account_id_prod    = "111111111111"
  aws_account_id_staging = "222222222222"
  aws_region_prod        = "us-east-1"
  aws_region_staging     = "us-east-2"
}

run "organization" {
  command = plan

  assert {
    condition     = github_organization_settings.org.billing_email == "billing@example.com" && github_organization_settings.org.default_repository_permission == "write"
    error_message = "the organization's settings take the vault's billing email and keep members' write access"
  }
  assert {
    condition     = length(github_membership.member) == 2 && github_membership.member["mock-admin"].role == "admin" && github_membership.member["mock-member"].role == "member"
    error_message = "one membership per vault member, with its role"
  }
  assert {
    condition     = github_actions_organization_workflow_permissions.org.default_workflow_permissions == "read" && !github_actions_organization_workflow_permissions.org.can_approve_pull_request_reviews
    error_message = "a workflow's token reads by default and never approves a pull request"
  }
}

run "repositories" {
  command = plan

  assert {
    condition     = length(github_repository.repo) == 10 && length(github_branch_default.repo) == 10 && length(github_repository_vulnerability_alerts.repo) == 10
    error_message = "every one of the organization's 10 repositories, with its default branch and alerts"
  }
  assert {
    condition     = github_repository.repo["opennova"].visibility == "public" && github_branch_default.repo["opennova"].branch == "master"
    error_message = "opennova is public, on master"
  }
  assert {
    condition     = github_repository.repo["opennova-reference-assets"].visibility == "private" && github_repository.repo["docs"].visibility == "private"
    error_message = "the reference data and the docs stay private"
  }
  assert {
    condition     = github_repository.repo["WolfRAT2"].fork && github_repository.repo["WolfRAT2"].source_owner == "BadgerLove"
    error_message = "WolfRAT2 is BadgerLove's fork"
  }
  assert {
    condition     = alltrue([for r in github_repository.repo : r.archive_on_destroy])
    error_message = "a repository leaving the stack is archived, never deleted"
  }
}

run "master_protection" {
  command = plan

  assert {
    condition     = !github_branch_protection.opennova_master.allows_deletions && !github_branch_protection.opennova_master.allows_force_pushes
    error_message = "master is never deleted or force-pushed"
  }
  assert {
    condition     = toset(github_branch_protection.opennova_master.required_status_checks[0].contexts) == toset(["godot-tests (windows-latest)", "test (windows-latest)"]) && github_branch_protection.opennova_master.required_status_checks[0].strict
    error_message = "master requires ci.yml's two checks, up to date"
  }
  assert {
    condition     = github_branch_protection.opennova_master.required_pull_request_reviews[0].required_approving_review_count == 1 && !github_branch_protection.opennova_master.enforce_admins
    error_message = "one approval, admins exempt"
  }
  assert {
    condition     = github_branch_protection.opennova_master.lock_branch
    error_message = "master is locked: only the admins merge into it"
  }
}
