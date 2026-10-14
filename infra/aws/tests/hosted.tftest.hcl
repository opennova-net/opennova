# The hosted game servers' resources (hosted.tf), planned against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare.
#   docker run --rm -v "$PWD/infra/aws:/tf" -w /tf hashicorp/terraform:1.9.5 init -backend=false
#   docker run --rm -v "$PWD/infra/aws:/tf" -w /tf hashicorp/terraform:1.9.5 test
# Hosting off (no snapshot) plans nothing of it and leaves the server's security
# group its six rules; hosting on plans the launch template (terminate on
# shutdown, IMDSv2, the encrypted data volume from the snapshot), the game and
# admin ports, the service role's policy and the outputs app deploy reads; the
# data volume takes the snapshot's size; the policy pins a launch to the
# template's machine type, IMDSv2, gp3 volumes of at most 16 GiB and
# Canonical's images, and the machine role holds Session
# Manager's five actions alone (applied against the mocks, so the template's
# ARN the policy names is known); a burstable machine type is refused.

mock_provider "aws" {
  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/sda1" }
  }
  mock_data "aws_ebs_snapshot" {
    defaults = { volume_size = 2 }
  }
  mock_data "aws_subnet" {
    defaults = { cidr_block = "10.44.0.0/24", map_public_ip_on_launch = true }
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
  # Well-formed ARNs for what an applied run hands to an ARN argument (the
  # provider's own validation reads them).
  mock_resource "aws_iam_instance_profile" {
    defaults = { arn = "arn:aws:iam::123456789012:instance-profile/mock" }
  }
  mock_resource "aws_iam_policy" {
    defaults = { arn = "arn:aws:iam::123456789012:policy/mock" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::123456789012:role/mock" }
  }
  mock_resource "aws_launch_template" {
    defaults = { arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0", id = "lt-0123456789abcdef0" }
  }
}

mock_provider "aws" {
  alias = "us_east_1"
}

mock_provider "cloudflare" {}

variables {
  ssh_public_key = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAItest test"
}

run "hosting_off" {
  command = plan

  assert {
    condition     = length(aws_launch_template.hosted) == 0 && length(aws_security_group.hosted) == 0
    error_message = "hosting off must create no launch template or security group"
  }
  assert {
    condition     = length(aws_iam_role.hosted_machine) == 0 && length(aws_iam_policy.hosted_service) == 0 && length(aws_iam_role_policy_attachment.hosted_service) == 0 && length(aws_iam_role_policy.hosted_machine_ssm) == 0
    error_message = "hosting off must create no IAM"
  }
  assert {
    condition     = length(aws_vpc_security_group_ingress_rule.hosted_game) == 0 && length(aws_vpc_security_group_ingress_rule.hosted_admin) == 0
    error_message = "hosting off must create no rule"
  }
  assert {
    condition     = length(aws_security_group.server.ingress) == 6
    error_message = "hosting off must leave the server SG's six rules"
  }
  assert {
    condition     = output.hosted_enabled == false && output.hosted_launch_template_id == null && output.hosted_subnet_cidr == null
    error_message = "hosting off outputs"
  }
}

run "hosting_on" {
  command = plan

  variables {
    hosted_data_snapshot_id = "snap-0123456789abcdef0"
    vpc_id                  = "vpc-0123456789abcdef0"
    subnet_id               = "subnet-0123456789abcdef0"
  }

  assert {
    condition     = length(aws_launch_template.hosted) == 1
    error_message = "a launch template"
  }
  assert {
    condition     = aws_launch_template.hosted[0].instance_initiated_shutdown_behavior == "terminate" && aws_launch_template.hosted[0].instance_type == "c7a.medium"
    error_message = "terminate on shutdown, c7a.medium"
  }
  assert {
    condition     = aws_launch_template.hosted[0].metadata_options[0].http_tokens == "required"
    error_message = "IMDSv2"
  }
  assert {
    condition     = length([for b in aws_launch_template.hosted[0].block_device_mappings : b if b.device_name == "/dev/sdf" && b.ebs[0].snapshot_id == "snap-0123456789abcdef0" && b.ebs[0].encrypted == "true" && b.ebs[0].volume_size == 2]) == 1
    error_message = "the data volume from the snapshot, encrypted, its size the snapshot's"
  }
  assert {
    condition     = length(aws_vpc_security_group_ingress_rule.hosted_game) == 1 && aws_vpc_security_group_ingress_rule.hosted_game["0.0.0.0/0"].from_port == 32768
    error_message = "the game port"
  }
  assert {
    condition     = aws_vpc_security_group_ingress_rule.hosted_admin[0].from_port == 32800 && aws_vpc_security_group_ingress_rule.hosted_admin[0].ip_protocol == "tcp"
    error_message = "the admin port"
  }
  assert {
    condition     = output.hosted_enabled == true && output.hosted_deployment == "opennova-server" && output.hosted_subnet_cidr == "10.44.0.0/24"
    error_message = "hosting on outputs"
  }
  assert {
    condition     = length(aws_iam_role_policy_attachment.hosted_service) == 1
    error_message = "the service role takes the hosted policy"
  }
}

run "hosted_policy" {
  command = apply

  variables {
    hosted_data_snapshot_id = "snap-0123456789abcdef0"
    vpc_id                  = "vpc-0123456789abcdef0"
    subnet_id               = "subnet-0123456789abcdef0"
  }

  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachines" && length([
        for c in st.condition : c
        if(c.test == "StringEquals" && c.variable == "ec2:InstanceType" && contains(c.values, "c7a.medium")) ||
        (c.test == "StringEquals" && c.variable == "ec2:MetadataHttpTokens" && contains(c.values, "required"))
      ]) == 2
    ]) == 1
    error_message = "a launch must be pinned to the template's machine type and IMDSv2"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachineVolumes" && length([
        for c in st.condition : c
        if(c.test == "StringEqualsIfExists" && c.variable == "ec2:VolumeType" && contains(c.values, "gp3")) ||
        (c.test == "NumericLessThanEqualsIfExists" && c.variable == "ec2:VolumeSize" && contains(c.values, "16"))
      ]) == 2
    ]) == 1
    error_message = "a launch's volumes must be gp3 of at most 16 GiB"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachineImage" && length([
        for c in st.condition : c
        if c.test == "StringEquals" && c.variable == "ec2:Owner" && contains(c.values, "099720109477")
      ]) == 1
    ]) == 1
    error_message = "a launch's image must be Canonical's"
  }
  assert {
    condition     = length(aws_iam_role_policy.hosted_machine_ssm) == 1
    error_message = "the machine role takes the Session Manager policy"
  }
  assert {
    condition = toset(flatten([for st in data.aws_iam_policy_document.hosted_machine_ssm[0].statement : st.actions])) == toset([
      "ssm:UpdateInstanceInformation",
      "ssmmessages:CreateControlChannel",
      "ssmmessages:CreateDataChannel",
      "ssmmessages:OpenControlChannel",
      "ssmmessages:OpenDataChannel",
    ])
    error_message = "the machine role holds Session Manager's five actions alone"
  }
}

run "burstable_refused" {
  command = plan

  variables {
    hosted_data_snapshot_id = "snap-0123456789abcdef0"
    hosted_instance_type    = "t3.medium"
    vpc_id                  = "vpc-0123456789abcdef0"
    subnet_id               = "subnet-0123456789abcdef0"
  }

  expect_failures = [var.hosted_instance_type]
}
