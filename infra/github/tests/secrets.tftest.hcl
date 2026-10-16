# The repository's Actions secrets (secrets.tf), planned against a mock
# provider: offline, no token, nothing reaches GitHub.

mock_provider "github" {
  override_during = plan

  mock_data "github_repository" {
    defaults = { repo_id = 123456789 }
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
}

run "reference_assets_token" {
  command = plan

  assert {
    condition     = github_actions_secret.reference_assets_token.repository == "opennova" && github_actions_secret.reference_assets_token.secret_name == "REFERENCE_ASSETS_TOKEN"
    error_message = "the retail legs read REFERENCE_ASSETS_TOKEN from opennova-net/opennova"
  }
  assert {
    condition     = github_actions_secret.reference_assets_token.value == var.reference_assets_token
    error_message = "the secret's value is the vault item reference-assets"
  }
}

run "stripe_test_secret_key" {
  command = plan

  assert {
    condition     = github_actions_secret.stripe_test_secret_key.repository == "opennova" && github_actions_secret.stripe_test_secret_key.secret_name == "STRIPE_TEST_SECRET_KEY"
    error_message = "test-linux reads STRIPE_TEST_SECRET_KEY from opennova-net/opennova"
  }
  assert {
    condition     = github_actions_secret.stripe_test_secret_key.value == var.stripe_test_secret_key
    error_message = "the secret's value is the vault item stripe-test's ci_secret_key"
  }
}
