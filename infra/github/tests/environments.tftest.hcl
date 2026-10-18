# The deploy workflow's environments (environments.tf), planned against a mock
# provider: offline, no token, nothing reaches GitHub. prod waits for an admin's
# approval and takes protected branches alone; staging runs at once; each
# names the role its deploy assumes in its own account, that account and its
# region (the vault's, as variables).

mock_provider "github" {
  override_during = plan

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

run "prod_waits_for_an_admin" {
  command = plan

  assert {
    condition = (
      github_repository_environment.prod.environment == "prod" &&
      github_repository_environment.prod.repository == "opennova" &&
      !github_repository_environment.prod.can_admins_bypass
    )
    error_message = "opennova's prod environment, which nobody bypasses"
  }
  assert {
    condition     = toset(keys(data.github_user.deploy_approver)) == toset(["mock-admin"]) && github_repository_environment.prod.reviewers[0].users == toset([4242])
    error_message = "the organization's admins, and they alone, approve a prod deploy"
  }
  assert {
    condition = (
      github_repository_environment.prod.deployment_branch_policy[0].protected_branches &&
      !github_repository_environment.prod.deployment_branch_policy[0].custom_branch_policies
    )
    error_message = "a prod deploy runs from a protected branch alone"
  }
}

run "staging_runs_at_once" {
  command = plan

  assert {
    condition = (
      github_repository_environment.staging.environment == "staging" &&
      length(github_repository_environment.staging.reviewers) == 0 &&
      length(github_repository_environment.staging.deployment_branch_policy) == 0
    )
    error_message = "staging's environment has no reviewers and takes any branch (a rehearsal)"
  }
}

run "account_must_be_an_id" {
  command = plan

  variables {
    aws_account_id_staging = "REPLACE_ME"
  }

  expect_failures = [var.aws_account_id_staging]
}

run "each_environment_its_own_account" {
  command = plan

  assert {
    condition     = length(github_actions_environment_variable.deploy) == 6
    error_message = "each environment holds its role, its region and its account"
  }
  assert {
    condition = (
      github_actions_environment_variable.deploy["prod/AWS_DEPLOY_ROLE_ARN"].environment == "prod" &&
      github_actions_environment_variable.deploy["prod/AWS_DEPLOY_ROLE_ARN"].value == "arn:aws:iam::111111111111:role/opennova-prod-github-deploy"
    )
    error_message = "prod's environment names prod's GitHub deploy role, in prod's account"
  }
  assert {
    condition = (
      github_actions_environment_variable.deploy["staging/AWS_DEPLOY_ROLE_ARN"].environment == "staging" &&
      github_actions_environment_variable.deploy["staging/AWS_DEPLOY_ROLE_ARN"].value == "arn:aws:iam::222222222222:role/opennova-staging-github-deploy"
    )
    error_message = "staging's environment names staging's GitHub deploy role, in staging's account"
  }
  assert {
    condition = (
      github_actions_environment_variable.deploy["prod/AWS_REGION"].value == "us-east-1" &&
      github_actions_environment_variable.deploy["staging/AWS_REGION"].value == "us-east-2" &&
      github_actions_environment_variable.deploy["prod/AWS_ACCOUNT_ID"].value == "111111111111" &&
      github_actions_environment_variable.deploy["staging/AWS_ACCOUNT_ID"].value == "222222222222"
    )
    error_message = "each environment names its deployment's region and account"
  }
}
