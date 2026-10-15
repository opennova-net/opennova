# The fleet (fleet.tf): the hosted game servers' machines, planned against
# mock providers whose values reach the plan: offline, no credentials, nothing
# reaches AWS or Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Hosting off (no snapshot) plans no machine resource and tells the service so.
# Hosting on plans two launch templates: shared (t3a.large, unlimited CPU
# credits) and own (c7a.medium, no credit setting), each terminating on
# shutdown, IMDSv2 with a hop limit of 1, and mounting every data set's
# snapshot as an encrypted volume of its own; the slots' game ports to everyone
# and admin ports to the service alone, the service's internal port to the
# machines alone. The service's policy pins a launch to the two templates and
# their two types, IMDSv2, this deployment's tags, gp3 volumes no larger than
# the largest the templates make, their images and snapshots; it ends only
# machines carrying both tags. The machine role holds Session Manager and its
# logs alone. A t2 or Graviton type, a slot count past 32, and a data set the
# service does not know are refused.

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

# The apply-time mocks, for the one applied run.
mock_provider "aws" {
  alias = "applied"

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/sda1" }
  }
  mock_data "aws_ebs_snapshot" {
    defaults = { volume_size = 2 }
  }
  mock_data "aws_iam_policy_document" {
    defaults = { json = "{\"Version\":\"2012-10-17\",\"Statement\":[]}" }
  }
  mock_data "aws_eip" {
    defaults = { id = "eipalloc-0123456789abcdef0", public_ip = "54.162.79.147" }
  }
  mock_data "aws_caller_identity" {
    defaults = { account_id = "123456789012" }
  }
  mock_data "aws_partition" {
    defaults = { partition = "aws" }
  }
  mock_resource "aws_iam_instance_profile" {
    defaults = { arn = "arn:aws:iam::123456789012:instance-profile/mock" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::123456789012:role/mock" }
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
  target          = aws_cloudwatch_log_group.machines
  override_during = plan
  values          = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/machines" }
}

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
}

run "hosting_off" {
  command = plan

  assert {
    condition     = length(aws_launch_template.fleet) == 0 && length(aws_security_group.machines) == 0
    error_message = "hosting off: no launch template, no security group"
  }
  assert {
    condition     = length(aws_iam_role.machine) == 0 && length(aws_iam_role_policy.fleet_service) == 0 && length(aws_iam_role_policy.machine) == 0
    error_message = "hosting off: no IAM"
  }
  assert {
    condition     = length(aws_vpc_security_group_ingress_rule.machines_game) == 0 && length(aws_vpc_security_group_ingress_rule.service_from_machines) == 0
    error_message = "hosting off: no rule, and nothing admitted to the service's internal port"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_PROVIDER='none'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_INTERNAL_HTTP_BIND='10.44.0.10:8070'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_FLEET_SHARED_SLOTS='12'\n")
    )
    error_message = "hosting off: infra.env turns the provider off and still names the internal listener and the fleet's knobs"
  }
  assert {
    condition     = output.hosted_enabled == false && length(output.fleet_launch_templates) == 0 && output.hosted_subnet_cidr == null
    error_message = "hosting off outputs"
  }
}

run "hosting_on" {
  command = plan

  # The machines' group and templates by id, known at plan.
  override_resource {
    target          = aws_security_group.machines[0]
    override_during = plan
    values          = { id = "sg-0machines" }
  }
  override_resource {
    target          = aws_launch_template.fleet["shared"]
    override_during = plan
    values          = { id = "lt-0shared", arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0shared" }
  }
  override_resource {
    target          = aws_launch_template.fleet["own"]
    override_during = plan
    values          = { id = "lt-0own", arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0own" }
  }

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
    condition     = toset(keys(aws_launch_template.fleet)) == toset(["shared", "own"])
    error_message = "two templates: shared and own"
  }
  assert {
    condition     = aws_launch_template.fleet["shared"].instance_type == "t3a.large" && one(aws_launch_template.fleet["shared"].credit_specification).cpu_credits == "unlimited"
    error_message = "shared machines: t3a.large with unlimited CPU credits"
  }
  assert {
    condition     = aws_launch_template.fleet["own"].instance_type == "c7a.medium" && length(aws_launch_template.fleet["own"].credit_specification) == 0
    error_message = "own machines: c7a.medium, no credit setting"
  }
  assert {
    condition = alltrue([
      for t in aws_launch_template.fleet :
      t.instance_initiated_shutdown_behavior == "terminate" && t.metadata_options[0].http_tokens == "required" && t.metadata_options[0].http_put_response_hop_limit == 1
    ])
    error_message = "every template: terminate on shutdown, IMDSv2, hop limit 1"
  }
  assert {
    condition = alltrue([
      for t in aws_launch_template.fleet :
      length([for b in t.block_device_mappings : b if b.device_name == "/dev/sdf" && b.ebs[0].snapshot_id == "snap-0fedcba9876543210" && b.ebs[0].volume_size == 3 && b.ebs[0].encrypted == "true"]) == 1 &&
      length([for b in t.block_device_mappings : b if b.device_name == "/dev/sdg" && b.ebs[0].snapshot_id == "snap-0123456789abcdef0" && b.ebs[0].volume_size == 2 && b.ebs[0].encrypted == "true"]) == 1
    ])
    error_message = "every machine mounts every data set: a volume each, encrypted, its size its snapshot's"
  }
  assert {
    condition     = aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].from_port == 32768 && aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].to_port == 32779 && aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].ip_protocol == "udp"
    error_message = "the 12 slots' game UDP ports to everyone"
  }
  assert {
    condition = (
      aws_vpc_security_group_ingress_rule.machines_admin[0].from_port == 32800 && aws_vpc_security_group_ingress_rule.machines_admin[0].to_port == 32811 &&
      aws_vpc_security_group_ingress_rule.machines_admin[0].ip_protocol == "tcp" && aws_vpc_security_group_ingress_rule.machines_admin[0].referenced_security_group_id == "sg-0service"
    )
    error_message = "the 12 slots' admin TCP ports to the service alone"
  }
  assert {
    condition = (
      aws_vpc_security_group_ingress_rule.service_from_machines[0].from_port == 8070 &&
      aws_vpc_security_group_ingress_rule.service_from_machines[0].security_group_id == "sg-0service" &&
      aws_vpc_security_group_ingress_rule.service_from_machines[0].referenced_security_group_id == "sg-0machines"
    )
    error_message = "the service's internal port admits the machines alone"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_PROVIDER='ec2'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_EC2_LAUNCH_TEMPLATES='{\"own\":\"lt-0own\",\"shared\":\"lt-0shared\"}'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_EC2_SUBNET='subnet-0public0a'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_BOOTSTRAP_CIDRS='10.44.0.0/24'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_DEPLOYMENT='opennova-server'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_SERVICE_ADDRESS='10.44.0.10'\n")
    )
    error_message = "infra.env hands the service the provider, the templates by kind, the subnet, the deployment tag and its own address"
  }
  assert {
    condition     = output.fleet_launch_templates == { shared = "lt-0shared", own = "lt-0own" } && output.hosted_deployment == "opennova-server"
    error_message = "the outputs name the templates by kind"
  }
}

run "fleet_policy" {
  command = plan

  # The machines' group and templates by id, known at plan.
  override_resource {
    target          = aws_security_group.machines[0]
    override_during = plan
    values          = { id = "sg-0machines" }
  }
  override_resource {
    target          = aws_launch_template.fleet["shared"]
    override_during = plan
    values          = { id = "lt-0shared", arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0shared" }
  }
  override_resource {
    target          = aws_launch_template.fleet["own"]
    override_during = plan
    values          = { id = "lt-0own", arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0own" }
  }

  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
      dfx2_consumer  = "snap-0fedcba9876543210"
    }
  }

  # The DFX2 snapshot is the largest volume.
  override_data {
    target = data.aws_ebs_snapshot.hosted["dfx2_consumer"]
    values = { volume_size = 20 }
  }

  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachines" && length([
        for c in st.condition : c
        if(c.test == "StringEquals" && c.variable == "ec2:InstanceType" && toset(c.values) == toset(["t3a.large", "c7a.medium"])) ||
        (c.test == "StringEquals" && c.variable == "ec2:MetadataHttpTokens" && contains(c.values, "required")) ||
        (c.test == "ArnEquals" && c.variable == "ec2:LaunchTemplate" && toset(c.values) == toset(["arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0shared", "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0own"]))
      ]) == 3
    ]) == 1
    error_message = "a launch is pinned to the two templates, their two types and IMDSv2"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachines" && length([
        for c in st.condition : c
        if(c.variable == "aws:RequestTag/opennova:hosted" && contains(c.values, "1")) ||
        (c.variable == "aws:RequestTag/opennova:deployment" && contains(c.values, "opennova-server")) ||
        (c.test == "ForAllValues:StringEquals" && c.variable == "aws:TagKeys")
      ]) == 3
    ]) == 1
    error_message = "a launch carries this deployment's tags and no other tag"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachineVolumes" && length([
        for c in st.condition : c
        if(c.test == "StringEqualsIfExists" && c.variable == "ec2:VolumeType" && contains(c.values, "gp3")) ||
        (c.test == "NumericLessThanEqualsIfExists" && c.variable == "ec2:VolumeSize" && contains(c.values, "20"))
      ]) == 2
    ]) == 1
    error_message = "a launch's volumes: gp3 of at most the largest snapshot's 20 GiB"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachineResources" &&
      contains(st.resources, "arn:aws:ec2:us-east-1::snapshot/snap-0123456789abcdef0") &&
      contains(st.resources, "arn:aws:ec2:us-east-1::snapshot/snap-0fedcba9876543210") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0shared") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0own") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:security-group/sg-0machines") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:subnet/subnet-0public0a")
    ]) == 1
    error_message = "a launch may touch both templates, both snapshots, the machines' group and the public subnets"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "EndMachines" && contains(st.actions, "ec2:TerminateInstances") && length([
        for c in st.condition : c
        if(c.variable == "aws:ResourceTag/opennova:hosted" && contains(c.values, "1")) ||
        (c.variable == "aws:ResourceTag/opennova:deployment" && contains(c.values, "opennova-server"))
      ]) == 2
    ]) == 1
    error_message = "the service ends only machines carrying both of this deployment's tags"
  }
  assert {
    condition = toset(flatten([for st in data.aws_iam_policy_document.machine[0].statement : st.actions])) == toset([
      "ssm:UpdateInstanceInformation",
      "ssmmessages:CreateControlChannel",
      "ssmmessages:CreateDataChannel",
      "ssmmessages:OpenControlChannel",
      "ssmmessages:OpenDataChannel",
      "logs:CreateLogStream",
      "logs:PutLogEvents",
      "logs:DescribeLogStreams",
    ])
    error_message = "the machine role holds Session Manager's five actions and its logs alone"
  }
  assert {
    condition = alltrue([
      for st in data.aws_iam_policy_document.machine[0].statement :
      toset(st.resources) == toset(["arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/machines", "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/machines:*"])
      if st.sid == "ShipMachineLogs"
    ])
    error_message = "the machines write their own log group alone"
  }
}

run "four_slots" {
  command = plan

  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
    }
    shared_slots = 4
  }

  assert {
    condition     = aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].to_port == 32771 && aws_vpc_security_group_ingress_rule.machines_admin[0].to_port == 32803
    error_message = "the port ranges follow shared_slots"
  }
  assert {
    condition     = strcontains(aws_ssm_parameter.infra_env.value, "ONNET_FLEET_SHARED_SLOTS='4'\n") && length([for b in aws_launch_template.fleet["own"].block_device_mappings : b if b.device_name == "/dev/sdf"]) == 1
    error_message = "infra.env names the slot count; one data set, one volume"
  }
}

# A newer Canonical image moves neither the templates (ignore_changes) nor the
# policy's image pin: both keep the image the templates have in state. The
# templates are applied first, alone (targeted: the database, which refuses to
# be destroyed, never enters the test's state).
run "templates_applied" {
  command = apply

  variables {
    hosted_data_snapshots = {
      jop_2_consumer = "snap-0123456789abcdef0"
    }
  }

  plan_options {
    target = [aws_launch_template.fleet]
  }

  # Applied with the apply-time mocks (the plan-time ones would hand the apply
  # values other than its plan's).
  providers = {
    aws        = aws.applied
    cloudflare = cloudflare
  }

  assert {
    condition     = aws_launch_template.fleet["shared"].image_id == "ami-0123456789abcdef0"
    error_message = "a template takes the newest image when it is made"
  }
}

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
    condition = aws_launch_template.fleet["shared"].image_id == "ami-0123456789abcdef0" && flatten([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : [
        for c in st.condition : c.values if c.variable == "ec2:ImageID"
      ]
    ]) == ["ami-0123456789abcdef0"]
    error_message = "a new Canonical image must move neither the templates nor the policy"
  }
}

run "t2_refused" {
  command = plan

  variables {
    shared_instance_type = "t2.large"
  }

  expect_failures = [var.shared_instance_type]
}

run "graviton_refused" {
  command = plan

  variables {
    own_instance_type    = "c7g.medium"
    shared_instance_type = "t4g.large"
  }

  expect_failures = [var.own_instance_type, var.shared_instance_type]
}

run "too_many_slots_refused" {
  command = plan

  variables {
    shared_slots = 33
  }

  expect_failures = [var.shared_slots]
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
