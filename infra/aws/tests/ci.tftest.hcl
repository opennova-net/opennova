# The optional GitHub Actions deploy role (ci.tf), planned against mock
# providers whose values reach the plan: offline, no credentials, nothing
# reaches AWS or Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Off by default. On, the role trusts this repository's production
# environment alone, and may point the release, upload bundles under its
# deployment's prefix and send the converge document to the host tagged as its
# service host, nothing else.

mock_provider "aws" {
  override_during = plan

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/sda1" }
  }
  mock_data "aws_ebs_snapshot" {
    defaults = { volume_size = 2 }
  }
  mock_data "aws_caller_identity" {
    defaults = { account_id = "123456789012" }
  }
  mock_data "aws_partition" {
    defaults = { partition = "aws" }
  }
  mock_data "aws_iam_policy_document" {
    defaults = { json = "{\"Version\":\"2012-10-17\",\"Statement\":[]}" }
  }
  mock_data "aws_eip" {
    defaults = { id = "eipalloc-0123456789abcdef0", public_ip = "54.162.79.147" }
  }
  mock_resource "aws_eip" {
    defaults = { allocation_id = "eipalloc-0fedcba9876543210", public_ip = "3.3.3.3" }
  }
  # Well-formed ARNs and ids for what the configuration hands to an ARN or id
  # argument (the provider's own validation reads them).
  mock_resource "aws_iam_instance_profile" {
    defaults = { arn = "arn:aws:iam::123456789012:instance-profile/mock" }
  }
  mock_resource "aws_iam_policy" {
    defaults = { arn = "arn:aws:iam::123456789012:policy/mock" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::123456789012:role/mock" }
  }
  mock_resource "aws_cloudwatch_log_group" {
    defaults = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/mock" }
  }
  mock_resource "aws_ssm_document" {
    defaults = { arn = "arn:aws:ssm:us-east-1:123456789012:document/opennova-server-converge" }
  }
  mock_resource "aws_sns_topic" {
    defaults = { arn = "arn:aws:sns:us-east-1:123456789012:opennova-server-alerts" }
  }
  mock_resource "aws_db_instance" {
    defaults = { address = "opennova-server-db.abcdefghijkl.us-east-1.rds.amazonaws.com", port = 5432, id = "db-MOCK" }
  }
  mock_resource "aws_launch_template" {
    defaults = { arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0", id = "lt-0123456789abcdef0" }
  }
  mock_resource "aws_instance" {
    defaults = { id = "i-0123456789abcdef0" }
  }
}

mock_provider "cloudflare" {
  override_during = plan
}

# The ids the assertions compare, known at plan.
override_resource {
  target          = aws_subnet.public["a"]
  override_during = plan
  values          = { id = "subnet-0public0a" }
}
override_resource {
  target          = aws_subnet.public["b"]
  override_during = plan
  values          = { id = "subnet-0public0b" }
}
override_resource {
  target          = aws_subnet.private["a"]
  override_during = plan
  values          = { id = "subnet-0private0a" }
}
override_resource {
  target          = aws_subnet.private["b"]
  override_during = plan
  values          = { id = "subnet-0private0b" }
}
override_resource {
  target          = aws_security_group.service
  override_during = plan
  values          = { id = "sg-0service" }
}
override_resource {
  target          = aws_security_group.db
  override_during = plan
  values          = { id = "sg-0db" }
}
override_resource {
  target          = aws_network_interface.service
  override_during = plan
  values          = { id = "eni-0service" }
}

override_resource {
  target          = aws_iam_openid_connect_provider.github[0]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:oidc-provider/token.actions.githubusercontent.com" }
}
override_resource {
  target          = aws_ssm_parameter.release
  override_during = plan
  values          = { arn = "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/release" }
}
override_resource {
  target          = aws_ssm_parameter.infra_env
  override_during = plan
  values          = { arn = "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/infra.env" }
}

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
}

run "off_by_default" {
  command = plan

  assert {
    condition     = length(aws_iam_role.github_deploy) == 0 && length(aws_iam_openid_connect_provider.github) == 0 && output.github_deploy_role_arn == null
    error_message = "no deploy role unless github_deploy"
  }
}

run "deploy_role" {
  command = plan

  variables {
    github_deploy = true
  }

  assert {
    condition = length([
      for st in data.aws_iam_policy_document.github_deploy_trust[0].statement : st
      if contains(st.actions, "sts:AssumeRoleWithWebIdentity") && length([
        for c in st.condition : c
        if(c.variable == "token.actions.githubusercontent.com:sub" && length(c.values) == 1 && contains(c.values, "repo:opennova-net/opennova-godot:environment:production")) ||
        (c.variable == "token.actions.githubusercontent.com:aud" && contains(c.values, "sts.amazonaws.com"))
      ]) == 2
    ]) == 1
    error_message = "the role trusts this repository's production environment alone"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.github_deploy[0].statement : st
      if st.sid == "SendToServiceHost" && length([
        for c in st.condition : c if c.variable == "ssm:resourceTag/opennova:service" && contains(c.values, "prod")
      ]) == 1
    ]) == 1
    error_message = "the converge document goes to the host tagged as prod's service host alone"
  }
  assert {
    condition = alltrue([
      for st in data.aws_iam_policy_document.github_deploy[0].statement :
      alltrue([for r in st.resources : r == "*" || strcontains(r, "/prod") || strcontains(r, "releases/prod") || strcontains(r, "converge") || r == "arn:aws:s3:::opennova-backups" || strcontains(r, ":instance/*")])
    ])
    error_message = "every resource the role touches is prod's"
  }
  assert {
    condition = toset(flatten([for st in data.aws_iam_policy_document.github_deploy[0].statement : st.actions])) == toset([
      "ssm:GetParameter", "ssm:PutParameter", "s3:ListBucket", "s3:PutObject", "s3:GetObject",
      "ec2:DescribeInstances", "ssm:SendCommand", "ssm:GetCommandInvocation", "ssm:ListCommandInvocations",
    ])
    error_message = "the role does what app deploy and app rollback do, and nothing more"
  }
}
