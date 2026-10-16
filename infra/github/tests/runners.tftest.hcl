# The CI runners (runners.tf), planned against a mock provider: offline, no
# token, nothing reaches GitHub. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/github hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/github hashicorp/terraform:<the toolbox's> test

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

run "runners" {
  command = plan

  assert {
    condition     = github_actions_runner_group.ci.allows_public_repositories && github_actions_runner_group.ci.visibility == "selected"
    error_message = "the group lets this public repository use the runners, and no other repository"
  }
  assert {
    condition     = github_actions_runner_group.ci.selected_repository_ids == toset([123456789])
    error_message = "the group's one repository is opennova-net/opennova"
  }
  assert {
    condition     = output.windows_runner == "windows-16core" && output.linux_runner == "ubuntu-16core"
    error_message = "the runners' names are ci.yml's runs-on labels"
  }
  assert {
    condition     = github_actions_hosted_runner.windows.size == "16-core" && github_actions_hosted_runner.linux.size == "16-core"
    error_message = "both runners are 16-core"
  }
  assert {
    condition     = github_actions_hosted_runner.windows.image[0].id == "2307" && github_actions_hosted_runner.linux.image[0].id == "2306"
    error_message = "the runners run windows-latest's and ubuntu-latest's images"
  }
  assert {
    condition     = github_actions_hosted_runner.windows.maximum_runners == 10 && github_actions_hosted_runner.linux.maximum_runners == 5
    error_message = "the scale-up caps are 10 Windows and 5 Linux machines"
  }
}
