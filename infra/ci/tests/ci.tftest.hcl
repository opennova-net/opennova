# The CI runners' stack, planned against mock providers: offline, no
# credentials, nothing reaches AWS. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/ci hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/ci hashicorp/terraform:<the toolbox's> test
# The network (three public subnets in three zones behind an internet gateway,
# no NAT), the account's budget writing to the alert address, its S3 public
# access block, and the vault's inputs refused when they are placeholders.
# RunsOn's own module is stood in for: its resources are its tests'.

mock_provider "aws" {
  override_during = plan

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c", "us-east-1d"] }
  }
}

override_module {
  target = module.runs_on
  outputs = {
    ingress = { url = "https://abcdef0123.execute-api.us-east-1.amazonaws.com/" }
  }
}

variables {
  region            = "us-east-1"
  aws_account_id_ci = "444444444444"
  license_key       = "mock-license-key"
  alert_email       = "alerts@example.com"
}

run "network" {
  command = plan

  assert {
    condition = (
      length(aws_subnet.public) == 3 &&
      toset([for s in aws_subnet.public : s.availability_zone]) == toset(["us-east-1a", "us-east-1b", "us-east-1c"]) &&
      toset([for s in aws_subnet.public : s.cidr_block]) == toset(["10.17.0.0/20", "10.17.16.0/20", "10.17.32.0/20"])
    )
    error_message = "a public subnet in each of three zones, apart in the VPC"
  }
  assert {
    condition = (
      one([for r in aws_route_table.public.route : r.cidr_block if r.cidr_block == "0.0.0.0/0"]) == "0.0.0.0/0" &&
      length(aws_route_table_association.public) == 3
    )
    error_message = "every subnet routes to the internet through the gateway (no NAT)"
  }
}

run "budget_and_account" {
  command = plan

  assert {
    condition = (
      aws_budgets_budget.monthly_cost.limit_amount == "300" &&
      aws_budgets_budget.monthly_cost.time_unit == "MONTHLY" &&
      toset([for n in aws_budgets_budget.monthly_cost.notification : n.notification_type]) == toset(["ACTUAL", "FORECASTED"]) &&
      alltrue([for n in aws_budgets_budget.monthly_cost.notification : n.subscriber_email_addresses == toset(["alerts@example.com"])])
    )
    error_message = "the account's monthly budget emails the alert address at 80% spent and a 100% forecast"
  }
  assert {
    condition = (
      aws_s3_account_public_access_block.account.block_public_acls &&
      aws_s3_account_public_access_block.account.block_public_policy &&
      aws_s3_account_public_access_block.account.ignore_public_acls &&
      aws_s3_account_public_access_block.account.restrict_public_buckets
    )
    error_message = "no bucket in the ci account can be made public"
  }
  assert {
    condition     = output.setup_url == "https://abcdef0123.execute-api.us-east-1.amazonaws.com/"
    error_message = "the setup page's address is the stack's output"
  }
}

run "license_key_must_be_set" {
  command = plan

  variables {
    license_key = "REPLACE_ME"
  }

  expect_failures = [var.license_key]
}

run "account_must_be_an_id" {
  command = plan

  variables {
    aws_account_id_ci = "REPLACE_ME"
  }

  expect_failures = [var.aws_account_id_ci]
}

run "alert_email_must_be_an_address" {
  command = plan

  variables {
    alert_email = "REPLACE_ME"
  }

  expect_failures = [var.alert_email]
}
