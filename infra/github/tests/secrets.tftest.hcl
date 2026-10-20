# The repository's Actions secrets (secrets.tf), planned against a mock
# provider: offline, no token, nothing reaches GitHub.

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
  stripe_test_secret_key = "rk_test_mock"
  aws_account_id_prod    = "111111111111"
  aws_account_id_staging = "222222222222"
  aws_region_prod        = "us-east-1"
  aws_region_staging     = "us-east-2"
}

run "stripe_test_secret_key" {
  command = plan

  assert {
    condition     = github_actions_secret.stripe_test_secret_key.repository == "opennova" && github_actions_secret.stripe_test_secret_key.secret_name == "STRIPE_TEST_SECRET_KEY"
    error_message = "test-linux reads STRIPE_TEST_SECRET_KEY from opennova-net/opennova"
  }
  assert {
    condition     = github_actions_secret.stripe_test_secret_key.value == var.stripe_test_secret_key
    error_message = "the secret's value is the vault item stripe-test's restricted_key"
  }
}
