# The hosted game servers' resources (hosted.tf), planned against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare.
#   docker run --rm -v "$PWD/infra/aws:/tf" -w /tf hashicorp/terraform:1.9.5 init -backend=false
#   docker run --rm -v "$PWD/infra/aws:/tf" -w /tf hashicorp/terraform:1.9.5 test
# Hosting off (no snapshot) plans nothing of it and leaves the server's security
# group its six rules; hosting on with two data sets plans a launch template for
# each (terminate on shutdown, IMDSv2, the encrypted data volume from that data
# set's own snapshot, its size the snapshot's), the game and admin ports, the
# service role's policy and the outputs app deploy reads (the templates by data
# set); the policy pins a launch to the templates (every one's ARN), their
# machine type, IMDSv2, gp3 volumes no larger than the largest of 16 GiB and the
# snapshots, the templates' own images, and both snapshots, and the machine
# role holds Session Manager's five actions alone (applied against the mocks,
# so the templates' ARNs the policy names are known); a burstable machine type
# and a data set the service does not know are refused.

mock_provider "aws" {
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
    condition     = output.hosted_enabled == false && length(output.hosted_launch_templates) == 0 && output.hosted_subnet_cidr == null
    error_message = "hosting off outputs"
  }
}

run "hosting_on" {
  command = plan

  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
      dfx2_consumer  = "snap-0fedcba9876543210"
    }
  }

  override_data {
    target = data.aws_ebs_snapshot.hosted["dfx2_consumer"]
    values = { volume_size = 3 }
  }

  assert {
    condition     = length(aws_launch_template.hosted) == 2
    error_message = "a launch template per data set"
  }
  assert {
    condition = alltrue([
      for t in aws_launch_template.hosted : t.instance_initiated_shutdown_behavior == "terminate" && t.instance_type == "c7a.medium" && t.metadata_options[0].http_tokens == "required"
    ])
    error_message = "every template: terminate on shutdown, c7a.medium, IMDSv2"
  }
  assert {
    condition     = aws_launch_template.hosted["jop_2_consumer"].name == "opennova-server-hosted-jop-2-consumer" && aws_launch_template.hosted["dfx2_consumer"].name == "opennova-server-hosted-dfx2-consumer"
    error_message = "a template named after its data set"
  }
  assert {
    condition     = length([for b in aws_launch_template.hosted["jop_2_consumer"].block_device_mappings : b if b.device_name == "/dev/sdf" && b.ebs[0].snapshot_id == "snap-0123456789abcdef0" && b.ebs[0].encrypted == "true" && b.ebs[0].volume_size == 2]) == 1
    error_message = "the JO template: the data volume from JO's snapshot, encrypted, its size the snapshot's"
  }
  assert {
    condition     = length([for b in aws_launch_template.hosted["dfx2_consumer"].block_device_mappings : b if b.device_name == "/dev/sdf" && b.ebs[0].snapshot_id == "snap-0fedcba9876543210" && b.ebs[0].encrypted == "true" && b.ebs[0].volume_size == 3]) == 1
    error_message = "the DFX2 template: the data volume from DFX2's snapshot, encrypted, its size the snapshot's"
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
    condition     = toset(keys(output.hosted_launch_templates)) == toset(["jop_2_consumer", "dfx2_consumer"])
    error_message = "the templates output names each data set"
  }
  assert {
    condition     = length(aws_iam_role_policy_attachment.hosted_service) == 1 && length(aws_security_group.hosted) == 1
    error_message = "one security group, and the service role takes the hosted policy"
  }
}

run "hosted_policy" {
  command = apply

  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
      dfx2_consumer  = "snap-0fedcba9876543210"
    }
  }

  # The DFX2 template is a launch template of its own, and its snapshot the
  # largest volume.
  override_resource {
    target = aws_launch_template.hosted["dfx2_consumer"]
    values = { arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0fedcba9876543210", id = "lt-0fedcba9876543210" }
  }
  override_data {
    target = data.aws_ebs_snapshot.hosted["dfx2_consumer"]
    values = { volume_size = 20 }
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
    error_message = "a launch must be pinned to the templates' machine type and IMDSv2"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachines" && length([
        for c in st.condition : c
        if c.test == "ArnEquals" && c.variable == "ec2:LaunchTemplate" && length(c.values) == 2 &&
        contains(c.values, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0") &&
        contains(c.values, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0fedcba9876543210")
      ]) == 1
    ]) == 1
    error_message = "a launch must come from one of the data sets' templates, every one named"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachineVolumes" && length([
        for c in st.condition : c
        if(c.test == "StringEqualsIfExists" && c.variable == "ec2:VolumeType" && contains(c.values, "gp3")) ||
        (c.test == "NumericLessThanEqualsIfExists" && c.variable == "ec2:VolumeSize" && contains(c.values, "20"))
      ]) == 2
    ]) == 1
    error_message = "a launch's volumes must be gp3 of at most the largest snapshot's 20 GiB"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachineImage" && length([
        for c in st.condition : c
        if c.test == "StringEquals" && c.variable == "ec2:ImageID" && length(c.values) == 1 && contains(c.values, aws_launch_template.hosted["jop_2_consumer"].image_id)
      ]) == 1
    ]) == 1
    error_message = "a launch's image must be the templates' own, each once"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : st
      if st.sid == "LaunchHostedMachineResources" && contains(st.resources, "arn:aws:ec2:us-east-1::snapshot/snap-0123456789abcdef0") && contains(st.resources, "arn:aws:ec2:us-east-1::snapshot/snap-0fedcba9876543210") && contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0") && contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0fedcba9876543210")
    ]) == 1
    error_message = "a launch may touch both data sets' templates and snapshots"
  }
  assert {
    condition     = output.hosted_launch_templates == { jop_2_consumer = "lt-0123456789abcdef0", dfx2_consumer = "lt-0fedcba9876543210" }
    error_message = "the templates output maps each data set to its template"
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

# A newer Canonical image moves neither the template (ignore_changes) nor the
# policy's image pin: both keep the image the template has in state.
run "new_canonical_image" {
  command = plan
  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
    }
  }
  override_data {
    target = data.aws_ami.hosted[0]
    values = { id = "ami-0fedcba9876543210", root_device_name = "/dev/sda1" }
  }
  assert {
    condition = aws_launch_template.hosted["jop_2_consumer"].image_id == "ami-0123456789abcdef0" && flatten([
      for st in data.aws_iam_policy_document.hosted_service[0].statement : [
        for c in st.condition : c.values if c.variable == "ec2:ImageID"
      ]
    ]) == ["ami-0123456789abcdef0"]
    error_message = "a new Canonical image must move neither the template nor the policy"
  }
}

run "burstable_refused" {
  command = plan

  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
    }
    hosted_instance_type = "t3.medium"
  }

  expect_failures = [var.hosted_instance_type]
}

# A data set the service does not know (a typo, or an old field's name) and a
# value that is no snapshot id are refused at plan.
run "unknown_data_set_refused" {
  command = plan

  variables {
    hosted_data_snapshots = {
      jop_2_consumr = "snap-0123456789abcdef0"
    }
  }

  expect_failures = [var.hosted_data_snapshots]
}

run "no_snapshot_id_refused" {
  command = plan

  variables {
    hosted_data_snapshots = {
      dfx2_consumer = "vol-0123456789abcdef0"
    }
  }

  expect_failures = [var.hosted_data_snapshots]
}
