# The fleet (fleet.tf, fleet_agent.tf): the hosted game servers' machines and
# their agent, planned against mock providers whose values reach the plan:
# offline, no credentials, nothing reaches AWS or Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Hosting off (hosted_enabled false, the default) plans no machine resource,
# no fleet cluster and no hostd, and tells the service so, with no store.
# Hosting on plans two launch templates of the pinned ECS-optimized Amazon
# Linux 2023 image (fleet_ami, by name, Amazon's): shared (t3a.medium,
# unlimited CPU credits) and own (c7a.medium, no credit setting), each
# terminating on shutdown, IMDSv2 with a hop limit of 1, one encrypted root
# volume (40 GiB: the data trees hostd fetches live there) and no other; the
# slots' game ports to everyone and admin ports to the fleet program alone,
# the fleet's internal port to the machines alone. The worker's task role's
# policy pins a launch to the two templates and their two types, IMDSv2, this
# deployment's tags, gp3 volumes no larger than the root volume, the pinned
# image by id; it ends only machines carrying both tags. It allocates a static
# address only with this deployment's tags and no other tag, tags one only as
# it is allocated, associates one carrying both tags only with a machine
# carrying both, disassociates and releases only those, and describes them;
# while the play zone exists (edge.tf) it writes A records under that zone
# alone (UPSERT and DELETE) and reads a change's state, and holds no Route 53
# action without it. The fleet cluster
# runs hostd as a DAEMON service on the EC2 launch type: host network, user 0,
# NET_ADMIN beside the defaults, never privileged, the machine's paths bound
# at their own paths (the docker socket among them), a task role reading the
# trees, no execution role, its health its loop; the machine role holds the
# ECS agent's calls on that cluster and the machines' and servers' logs alone.
# The user data's ecs.config disables privileged containers and never turns on
# ECS's cleanup of images it did not pull. Web's, the fleet's and the worker's
# environment carry the fleet's policy (the pinned image's id, the addresses
# it sells and their zone's name among it) and the store's URL, the worker's
# alone the machine driver's own settings (the fleet cluster and the play
# zone's id among them), web the fleet cluster's name for its reads, infra.env
# what the toolbox's fleet commands need. A t2 or Graviton type, a slot count
# past 32, more than 64 addresses and hosting with no fleet_ami are refused.

mock_provider "aws" {
  override_during = plan

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/xvda" }
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
  # The deployment's parameters as the toolbox left them: no release yet (a run
  # naming one overrides data.aws_ssm_parameters_by_path.deployment).
  mock_data "aws_ssm_parameters_by_path" {
    defaults = { names = [], values = [] }
  }
  mock_resource "aws_eip" {
    defaults = { allocation_id = "eipalloc-0fedcba9876543210", public_ip = "3.3.3.3" }
  }
  # Well-formed ARNs and ids for what the configuration hands to an ARN or id
  # argument (the provider's own validation reads them).
  mock_resource "aws_iam_instance_profile" {
    defaults = { arn = "arn:aws:iam::123456789012:instance-profile/mock" }
  }
  mock_resource "aws_iam_role" {
    defaults = { arn = "arn:aws:iam::123456789012:role/mock" }
  }
  mock_resource "aws_cloudwatch_log_group" {
    defaults = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/mock" }
  }
  mock_resource "aws_sns_topic" {
    defaults = { arn = "arn:aws:sns:us-east-1:123456789012:opennova-prod-alerts" }
  }
  mock_resource "aws_ssm_parameter" {
    defaults = { arn = "arn:aws:ssm:us-east-1:123456789012:parameter/opennova/prod/mock" }
  }
  mock_resource "aws_db_instance" {
    defaults = { address = "opennova-prod-db.abcdefghijkl.us-east-1.rds.amazonaws.com", port = 5432, id = "db-MOCK" }
  }
  mock_resource "aws_elasticache_replication_group" {
    defaults = { primary_endpoint_address = "master.opennova-prod-cache.abcdef.use1.cache.amazonaws.com" }
  }
  mock_resource "aws_lb" {
    defaults = {
      arn        = "arn:aws:elasticloadbalancing:us-east-1:123456789012:loadbalancer/net/opennova-prod/0123456789abcdef"
      arn_suffix = "net/opennova-prod/0123456789abcdef"
      dns_name   = "opennova-prod-0123456789abcdef.elb.us-east-1.amazonaws.com"
    }
  }
  mock_resource "aws_lb_target_group" {
    defaults = {
      arn        = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-mock/0123456789abcdef"
      arn_suffix = "targetgroup/opennova-prod-mock/0123456789abcdef"
    }
  }
  mock_resource "aws_launch_template" {
    defaults = { arn = "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0123456789abcdef0", id = "lt-0123456789abcdef0" }
  }
  mock_resource "aws_ecs_cluster" {
    defaults = { id = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod", arn = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod" }
  }
  mock_resource "aws_ecs_task_definition" {
    defaults = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/mock:1" }
  }
  mock_resource "aws_ecs_service" {
    defaults = { id = "arn:aws:ecs:us-east-1:123456789012:service/opennova-prod/mock" }
  }
  mock_resource "aws_service_discovery_private_dns_namespace" {
    defaults = { id = "ns-0123456789abcdef", arn = "arn:aws:servicediscovery:us-east-1:123456789012:namespace/ns-0123456789abcdef" }
  }
  mock_resource "aws_service_discovery_service" {
    defaults = { id = "srv-0123456789abcdef", arn = "arn:aws:servicediscovery:us-east-1:123456789012:service/srv-0123456789abcdef" }
  }
  # Route 53's answer for a new zone: its id and its four name servers.
  mock_resource "aws_route53_zone" {
    defaults = {
      zone_id      = "Z0123456789PLAYZONE"
      arn          = "arn:aws:route53:::hostedzone/Z0123456789PLAYZONE"
      name_servers = ["ns-1.awsdns-01.com", "ns-2.awsdns-02.net", "ns-3.awsdns-03.org", "ns-4.awsdns-04.co.uk"]
    }
  }
}

mock_provider "cloudflare" {
  override_during = plan

  mock_data "cloudflare_ip_ranges" {
    defaults = {
      ipv4_cidr_blocks = ["173.245.48.0/20", "103.21.244.0/22"]
      ipv6_cidr_blocks = ["2400:cb00::/32"]
    }
  }
  mock_data "cloudflare_zone" {
    defaults = { account_id = "fedcba9876543210fedcba9876543210", name = "example.com" }
  }
  mock_resource "cloudflare_origin_ca_certificate" {
    defaults = { certificate = "-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n" }
  }
}

# The ids and ARNs the assertions compare, known at plan.
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
  target          = aws_security_group.program["gate"]
  override_during = plan
  values          = { id = "sg-0gate" }
}
override_resource {
  target          = aws_security_group.program["lobby"]
  override_during = plan
  values          = { id = "sg-0lobby" }
}
override_resource {
  target          = aws_security_group.program["web"]
  override_during = plan
  values          = { id = "sg-0web" }
}
override_resource {
  target          = aws_security_group.program["fleet"]
  override_during = plan
  values          = { id = "sg-0fleet" }
}
override_resource {
  target          = aws_security_group.program["worker"]
  override_during = plan
  values          = { id = "sg-0worker" }
}
override_resource {
  target          = aws_security_group.program["data"]
  override_during = plan
  values          = { id = "sg-0data" }
}
override_resource {
  target          = aws_security_group.lb
  override_during = plan
  values          = { id = "sg-0lb" }
}
override_resource {
  target          = aws_security_group.db
  override_during = plan
  values          = { id = "sg-0db" }
}
override_resource {
  target          = aws_security_group.backup
  override_during = plan
  values          = { id = "sg-0backup" }
}
override_resource {
  target          = aws_lb_target_group.public["gate"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-gate/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-gate/0123456789abcdef" }
}
override_resource {
  target          = aws_lb_target_group.public["https"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-https/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-https/0123456789abcdef" }
}
override_resource {
  target          = aws_lb_target_group.public["http"]
  override_during = plan
  values          = { arn = "arn:aws:elasticloadbalancing:us-east-1:123456789012:targetgroup/opennova-prod-http/0123456789abcdef", arn_suffix = "targetgroup/opennova-prod-http/0123456789abcdef" }
}
override_resource {
  target          = aws_eip.lb_b
  override_during = plan
  values          = { allocation_id = "eipalloc-0000000000000000b", public_ip = "5.5.5.5" }
}
override_resource {
  target          = aws_ecs_task_definition.program["gate"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-gate:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-lobby:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["web"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-web:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-fleet:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-data:1" }
}
override_resource {
  target          = aws_ecs_task_definition.backup
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-backup:1" }
}
override_resource {
  target          = aws_iam_role.program["gate"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-gate" }
}
override_resource {
  target          = aws_iam_role.program["lobby"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-lobby" }
}
override_resource {
  target          = aws_iam_role.program["web"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-web" }
}
override_resource {
  target          = aws_iam_role.program["fleet"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-fleet" }
}
override_resource {
  target          = aws_iam_role.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-worker" }
}
override_resource {
  target          = aws_iam_role.program["data"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-data" }
}
override_resource {
  target          = aws_iam_role.execution
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-execution" }
}
override_resource {
  target          = aws_iam_role.backup
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-backup" }
}
override_resource {
  target          = aws_iam_role.backup_schedule
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-backup-schedule" }
}

override_resource {
  target          = aws_ecs_cluster.fleet[0]
  override_during = plan
  values          = { id = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod-fleet", arn = "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod-fleet" }
}
override_resource {
  target          = aws_iam_role.hostd[0]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-hostd" }
}
override_resource {
  target          = aws_ecs_task_definition.hostd[0]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-hostd:1" }
}

override_resource {
  target          = aws_cloudwatch_log_group.machines
  override_during = plan
  values          = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/machines" }
}
override_resource {
  target          = aws_cloudwatch_log_group.servers
  override_during = plan
  values          = { arn = "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/servers" }
}

variables {
  deployment                = "prod"
  aws_account_id            = "123456789012"
  region                    = "us-east-1"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
  fleet_ami                 = "al2023-ami-ecs-hvm-2023.0.20261009-kernel-6.1-x86_64"
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
    condition     = length(aws_vpc_security_group_ingress_rule.machines_game) == 0 && length(aws_vpc_security_group_ingress_rule.fleet_from_machines) == 0
    error_message = "hosting off: no rule, and nothing admitted to the fleet's internal port"
  }
  assert {
    condition = (
      length(aws_ecs_cluster.fleet) == 0 && length(aws_ecs_service.hostd) == 0 && length(aws_ecs_task_definition.hostd) == 0 &&
      length(aws_iam_role.hostd) == 0 && length(aws_iam_role_policy.web_fleet_read) == 0 && length(data.aws_ami.fleet) == 0
    )
    error_message = "hosting off: no fleet cluster, no hostd, no image looked up"
  }
  assert {
    condition = (alltrue([
      for p in ["web", "fleet", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_PROVIDER"] == "none" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_SHARED_SLOTS"] == "6" &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INTERNAL_URL") &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_MAX_ADDRESSES"] == "0" &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_ADDRESS_ZONE") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INTERNAL_HTTP_BIND") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_EC2_") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_FLEET_IMAGE_ID") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_OBJECT_STORE")
      ]) && strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_PROVIDER='none'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_FLEET_CLUSTER=''\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_FLEET_AMI_NAME=''\n") &&
    !strcontains(aws_ssm_parameter.infra_env.value, "hostd"))
    error_message = "hosting off: the programs' environment turns the provider off, names no store, no machine driver, no image and no address zone, and still names the fleet's knobs, the addresses it sells (0) among them (the fleet binds its own task's address); infra.env says so to the toolbox: no fleet cluster, no hostd"
  }
  assert {
    condition     = strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_MACHINES_LOG_GROUP='/opennova/prod/machines'\n")
    error_message = "infra.env names the machines' log group whether or not hosting is on (fleet logs reads what machines wrote)"
  }
  assert {
    condition     = strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_SERVERS_LOG_GROUP='/opennova/prod/servers'\n")
    error_message = "infra.env names the servers' log group whether or not hosting is on"
  }
  assert {
    condition     = output.hosted_enabled == false && length(output.fleet_launch_templates) == 0 && output.hosted_subnet_cidrs == null && output.fleet_cluster == null && output.fleet_image_id == null
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
    hosted_enabled = true
  }

  assert {
    condition     = toset(keys(aws_launch_template.fleet)) == toset(["shared", "own"])
    error_message = "two templates: shared and own"
  }
  assert {
    condition     = aws_launch_template.fleet["shared"].instance_type == "t3a.medium" && one(aws_launch_template.fleet["shared"].credit_specification).cpu_credits == "unlimited"
    error_message = "shared machines: t3a.medium with unlimited CPU credits"
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
      length(t.block_device_mappings) == 1 &&
      length([for b in t.block_device_mappings : b if b.device_name == "/dev/xvda" && b.ebs[0].volume_size == 40 && b.ebs[0].volume_type == "gp3" && b.ebs[0].encrypted == "true"]) == 1
    ])
    error_message = "every machine: one encrypted 40 GiB gp3 root volume (the trees it fetches live there), no data volume"
  }
  assert {
    condition     = aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].from_port == 32768 && aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].to_port == 32773 && aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].ip_protocol == "udp"
    error_message = "the 6 slots' game UDP ports to everyone"
  }
  assert {
    condition = (
      aws_vpc_security_group_ingress_rule.machines_admin[0].from_port == 32800 && aws_vpc_security_group_ingress_rule.machines_admin[0].to_port == 32805 &&
      aws_vpc_security_group_ingress_rule.machines_admin[0].ip_protocol == "tcp" && aws_vpc_security_group_ingress_rule.machines_admin[0].referenced_security_group_id == "sg-0fleet"
    )
    error_message = "the 6 slots' admin TCP ports to the fleet program alone"
  }
  assert {
    condition = (
      aws_vpc_security_group_ingress_rule.fleet_from_machines[0].from_port == 8070 &&
      aws_vpc_security_group_ingress_rule.fleet_from_machines[0].security_group_id == "sg-0fleet" &&
      aws_vpc_security_group_ingress_rule.fleet_from_machines[0].referenced_security_group_id == "sg-0machines"
    )
    error_message = "the fleet's internal port admits the machines alone"
  }
  assert {
    condition = alltrue([
      for p in ["web", "fleet", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_PROVIDER"] == "ec2" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_BOOTSTRAP_CIDRS"] == "10.44.0.0/24,10.44.1.0/24" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_IMAGE_ID"] == "ami-0123456789abcdef0" &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_EC2_LOG_GROUP") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_HOSTD_IMAGE") &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_OBJECT_STORE_URL"] == "s3://opennova-prod-ops-123456789012/data" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_OBJECT_STORE_REGION"] == "us-east-1" &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_DATA_SETS") &&
      !strcontains(aws_ecs_task_definition.program[p].container_definitions, "OPENNOVA_FLEET")
      ]) && (
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_EC2_LAUNCH_TEMPLATES"] == "{\"own\":\"lt-0own\",\"shared\":\"lt-0shared\"}" &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_EC2_SUBNETS"] == "subnet-0public0a,subnet-0public0b" &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_DEPLOYMENT"] == "opennova-prod" &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_EC2_CLUSTER"] == "opennova-prod-fleet" &&
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_INTERNAL_URL"] == "http://fleet.prod.opennova.internal:8070"
      ) && alltrue([
        for p in ["web", "fleet", "data"] :
        !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_EC2_") &&
        !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_DEPLOYMENT") &&
        !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_INTERNAL_URL")
    ])
    error_message = "web, the fleet and the worker get the provider, the agents' networks, the pinned image's id and the game data's store (the deployment's bucket under data/); the worker alone the templates by kind, the fleet cluster, both zones' subnets, the deployment tag and the fleet's URL; no log group and no hostd image (hostd's task definition names both)"
  }
  assert {
    condition = (
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_PROVIDER='ec2'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_FLEET_CLUSTER='opennova-prod-fleet'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_FLEET_AMI_NAME='al2023-ami-ecs-hvm-2023.0.20261009-kernel-6.1-x86_64'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_DEPLOYMENT='opennova-prod'\n") &&
      strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_MACHINES_LOG_GROUP='/opennova/prod/machines'\n") &&
      jsondecode(regex("OPENNOVA_TASK_DEFINITIONS='([^']*)'", aws_ssm_parameter.infra_env.value)[0]).hostd == "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-hostd:1" &&
      !strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_FLEET_IMAGE_PARAMETER") &&
      !strcontains(aws_ssm_parameter.infra_env.value, "OPENNOVA_FLEET_SECURITY_GROUP") &&
      !strcontains(aws_ssm_parameter.infra_env.value, "ONNET_HOSTED_EC2_")
    )
    error_message = "infra.env hands the toolbox the fleet cluster, the image name it is pinned to, hostd's task definition, the machines' tag and their log group (fleet logs)"
  }
  assert {
    condition     = { for e in jsondecode(aws_ecs_task_definition.program["web"].container_definitions)[0].environment : e.name => e.value }["ONNET_INFRA_FLEET_CLUSTER"] == "opennova-prod-fleet"
    error_message = "web reads the fleet cluster for the admin's infrastructure view"
  }
  assert {
    condition     = aws_iam_role_policy.fleet_service[0].role == aws_iam_role.program["worker"].id && aws_iam_role_policy.fleet_service[0].name == "opennova-prod-fleet"
    error_message = "the worker's task role alone launches and ends machines"
  }
  assert {
    condition = (
      alltrue([for t in aws_launch_template.fleet : t.image_id == "ami-0123456789abcdef0"]) &&
      data.aws_ami.fleet[0].owners == tolist(["amazon"]) && data.aws_ami.fleet[0].include_deprecated &&
      length([for f in data.aws_ami.fleet[0].filter : f if f.name == "name" && toset(f.values) == toset(["al2023-ami-ecs-hvm-2023.0.20261009-kernel-6.1-x86_64"])]) == 1 &&
      length([for f in data.aws_ami.fleet[0].filter : f if f.name == "architecture" && toset(f.values) == toset(["x86_64"])]) == 1 &&
      output.fleet_image_id == "ami-0123456789abcdef0" && output.fleet_cluster == "opennova-prod-fleet"
    )
    error_message = "the templates launch the image fleet_ami names exactly, Amazon's x86_64 one (a deprecated one still found)"
  }
  assert {
    condition     = output.hosted_subnet_ids == ["subnet-0public0a", "subnet-0public0b"] && output.hosted_subnet_cidrs == ["10.44.0.0/24", "10.44.1.0/24"]
    error_message = "machines launch into both zones' public subnets"
  }
  assert {
    condition     = output.fleet_launch_templates == { shared = "lt-0shared", own = "lt-0own" } && output.hosted_deployment == "opennova-prod"
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
    hosted_enabled = true
  }

  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachines" && length([
        for c in st.condition : c
        if(c.test == "StringEquals" && c.variable == "ec2:InstanceType" && toset(c.values) == toset(["t3a.medium", "c7a.medium"])) ||
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
        (c.variable == "aws:RequestTag/opennova:deployment" && contains(c.values, "opennova-prod")) ||
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
        (c.test == "NumericLessThanEqualsIfExists" && c.variable == "ec2:VolumeSize" && contains(c.values, "40"))
      ]) == 2
    ]) == 1
    error_message = "a launch's volumes: gp3 of at most the root volume's 40 GiB"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachineResources" &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0shared") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:launch-template/lt-0own") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:security-group/sg-0machines") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:subnet/subnet-0public0a") &&
      contains(st.resources, "arn:aws:ec2:us-east-1:123456789012:subnet/subnet-0public0b") &&
      !anytrue([for r in st.resources : strcontains(r, ":snapshot/")])
    ]) == 1
    error_message = "a launch may touch both templates, the machines' group and the public subnets, and no snapshot"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachineImage" && toset(st.resources) == toset(["arn:aws:ec2:us-east-1::image/*"]) && length([
        for c in st.condition : c if c.test == "StringEquals" && c.variable == "ec2:ImageID" && toset(c.values) == toset(["ami-0123456789abcdef0"])
      ]) == 1
    ]) == 1
    error_message = "a launch may use the pinned image alone, by id"
  }
  assert {
    condition = (
      !anytrue([for st in data.aws_iam_policy_document.fleet_service[0].statement : contains(st.actions, "ssm:GetParameters")]) &&
      length([for st in data.aws_iam_policy_document.fleet_service[0].statement : st if strcontains(st.sid, "Seed") || strcontains(st.sid, "Baked")]) == 0
    )
    error_message = "no image parameter to resolve, no seed or baked image"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "EndMachines" && contains(st.actions, "ec2:TerminateInstances") && length([
        for c in st.condition : c
        if(c.variable == "aws:ResourceTag/opennova:hosted" && contains(c.values, "1")) ||
        (c.variable == "aws:ResourceTag/opennova:deployment" && contains(c.values, "opennova-prod"))
      ]) == 2
    ]) == 1
    error_message = "the service ends only machines carrying both of this deployment's tags"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "AllocateAddresses" && toset(st.actions) == toset(["ec2:AllocateAddress"]) &&
      toset(st.resources) == toset(["arn:aws:ec2:us-east-1:123456789012:elastic-ip/*"]) && length([
        for c in st.condition : c
        if(c.test == "StringEquals" && c.variable == "aws:RequestTag/opennova:hosted" && toset(c.values) == toset(["1"])) ||
        (c.test == "StringEquals" && c.variable == "aws:RequestTag/opennova:deployment" && toset(c.values) == toset(["opennova-prod"])) ||
        (c.test == "ForAllValues:StringEquals" && c.variable == "aws:TagKeys" && toset(c.values) == toset(["opennova:hosted", "opennova:deployment", "opennova:address-id", "Name"]))
      ]) == 3
    ]) == 1
    error_message = "a static address is allocated with this deployment's tags and no other tag"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "TagAddressesAtAllocation" && toset(st.actions) == toset(["ec2:CreateTags"]) &&
      toset(st.resources) == toset(["arn:aws:ec2:us-east-1:123456789012:elastic-ip/*"]) && length([
        for c in st.condition : c if c.test == "StringEquals" && c.variable == "ec2:CreateAction" && toset(c.values) == toset(["AllocateAddress"])
      ]) == 1
    ]) == 1
    error_message = "an address is tagged only as it is allocated"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "AssociateReleaseAddresses" &&
      toset(st.actions) == toset(["ec2:AssociateAddress", "ec2:DisassociateAddress", "ec2:ReleaseAddress"]) &&
      toset(st.resources) == toset(["arn:aws:ec2:us-east-1:123456789012:elastic-ip/*"]) && length([
        for c in st.condition : c
        if(c.test == "StringEquals" && c.variable == "aws:ResourceTag/opennova:hosted" && toset(c.values) == toset(["1"])) ||
        (c.test == "StringEquals" && c.variable == "aws:ResourceTag/opennova:deployment" && toset(c.values) == toset(["opennova-prod"]))
      ]) == 2
      ]) == 1 && length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "AssociateAddressesWithMachines" && toset(st.actions) == toset(["ec2:AssociateAddress"]) &&
      toset(st.resources) == toset(["arn:aws:ec2:us-east-1:123456789012:instance/*"]) && length([
        for c in st.condition : c
        if(c.variable == "aws:ResourceTag/opennova:hosted" && toset(c.values) == toset(["1"])) ||
        (c.variable == "aws:ResourceTag/opennova:deployment" && toset(c.values) == toset(["opennova-prod"]))
      ]) == 2
    ]) == 1
    error_message = "the service associates, disassociates and releases only addresses carrying both of this deployment's tags (never the load balancer's), and associates them only with its machines"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "DescribeAddresses" && toset(st.actions) == toset(["ec2:DescribeAddresses"]) && toset(st.resources) == toset(["*"])
    ]) == 1
    error_message = "the service describes the addresses (no resource scope exists)"
  }
  assert {
    condition     = length([for st in data.aws_iam_policy_document.fleet_service[0].statement : st if anytrue([for a in st.actions : startswith(a, "route53:")])]) == 0
    error_message = "no play zone (no Cloudflare here, no addresses sold): no Route 53 action"
  }
  assert {
    condition = toset(flatten([for st in data.aws_iam_policy_document.machine[0].statement : st.actions])) == toset([
      "ecs:RegisterContainerInstance",
      "ecs:DeregisterContainerInstance",
      "ecs:SubmitContainerStateChange",
      "ecs:SubmitTaskStateChange",
      "ecs:SubmitAttachmentStateChanges",
      "ecs:Poll",
      "ecs:StartTelemetrySession",
      "ecs:DiscoverPollEndpoint",
      "logs:CreateLogStream",
      "logs:PutLogEvents",
    ])
    error_message = "the machine role holds the ECS agent's calls and its logs alone: no Session Manager, no store"
  }
  assert {
    condition = (
      length([
        for st in data.aws_iam_policy_document.machine[0].statement : st
        if st.sid == "EcsAgentCluster" && toset(st.resources) == toset(["arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod-fleet"])
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.machine[0].statement : st
        if st.sid == "EcsAgentInstance" && toset(st.resources) == toset(["arn:aws:ecs:us-east-1:123456789012:container-instance/opennova-prod-fleet/*"])
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.machine[0].statement : st
        if st.sid == "EcsAgentDiscover" && toset(st.actions) == toset(["ecs:DiscoverPollEndpoint"]) && toset(st.resources) == toset(["*"])
      ]) == 1
    )
    error_message = "the ECS agent registers and reports on the fleet cluster alone, and polls as its instances alone"
  }
  assert {
    condition = alltrue([
      for st in data.aws_iam_policy_document.machine[0].statement :
      toset(st.resources) == toset([
        "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/machines", "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/machines:*",
        "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/servers", "arn:aws:logs:us-east-1:123456789012:log-group:/opennova/prod/servers:*",
      ])
      if st.sid == "ShipMachineLogs"
    ])
    error_message = "the machines write the machines' and the servers' log groups alone"
  }
}

# hostd on the fleet cluster: a DAEMON service, one task per machine.
run "hostd_daemon" {
  command = plan

  variables {
    hosted_enabled = true
  }

  assert {
    condition = (
      aws_ecs_cluster.fleet[0].name == "opennova-prod-fleet" &&
      aws_ecs_service.hostd[0].name == "hostd" && aws_ecs_service.hostd[0].cluster == "arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod-fleet" &&
      aws_ecs_service.hostd[0].scheduling_strategy == "DAEMON" && aws_ecs_service.hostd[0].launch_type == "EC2" &&
      aws_ecs_service.hostd[0].desired_count == null &&
      aws_ecs_service.hostd[0].deployment_minimum_healthy_percent == 50 &&
      aws_ecs_service.hostd[0].enable_execute_command
    )
    error_message = "hostd is a DAEMON service of the fleet cluster on the EC2 launch type: no desired count, half the machines at most at once, ECS Exec on"
  }
  assert {
    condition     = !contains(keys(aws_appautoscaling_target.program), "hostd") && !contains(keys(aws_ecs_service.program), "hostd") && !contains(keys(local.programs), "hostd")
    error_message = "hostd is no program: no Auto Scaling target, no Fargate service, outside local.programs"
  }
  assert {
    condition = (
      aws_ecs_task_definition.hostd[0].family == "opennova-prod-hostd" &&
      aws_ecs_task_definition.hostd[0].network_mode == "host" &&
      aws_ecs_task_definition.hostd[0].requires_compatibilities == toset(["EC2"]) &&
      aws_ecs_task_definition.hostd[0].execution_role_arn == null &&
      aws_ecs_task_definition.hostd[0].task_role_arn == "arn:aws:iam::123456789012:role/opennova-prod-hostd"
    )
    error_message = "hostd's task: the host's network, EC2, its own task role and no execution role"
  }
  assert {
    condition = (
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].name == "hostd" &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].image == "ghcr.io/opennova-net/opennova-hostd:latest" &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].user == "0" &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].linuxParameters.capabilities.add == ["NET_ADMIN"] &&
      !contains(keys(jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].linuxParameters.capabilities), "drop") &&
      !contains(keys(jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0]), "privileged")
    )
    error_message = "hostd runs as root with docker's default capabilities and NET_ADMIN (its nft table), never privileged"
  }
  assert {
    condition = (
      length([
        for m in jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].mountPoints : m
        if m.containerPath == "/var/run/docker.sock" && m.readOnly == false
      ]) == 1 &&
      toset([for m in jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].mountPoints : m.containerPath]) ==
      toset(["/var/run/docker.sock", "/etc/opennova", "/var/lib/opennova-hostd", "/srv/slots", "/opt/game", "/etc/ecs"]) &&
      alltrue([
        for m in jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].mountPoints :
        one([for v in aws_ecs_task_definition.hostd[0].volume : v.host_path if v.name == m.sourceVolume]) == m.containerPath
      ]) &&
      alltrue([for m in jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].mountPoints : m.readOnly == (m.containerPath == "/etc/ecs")])
    )
    error_message = "hostd binds the docker socket and the machine's paths at their own paths, /etc/ecs read-only"
  }
  assert {
    condition = (
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].command == [
        "--service", "http://fleet.prod.opennova.internal:8070",
        "--machine-file", "/etc/opennova/machine-id",
        "--ecs-config", "/etc/ecs/ecs.config",
        "--log-group", "/opennova/prod/servers",
        "--log-region", "us-east-1",
      ] &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].healthCheck.command == ["CMD", "/usr/local/bin/opennova-hostd", "--check-alive"] &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].logConfiguration.logDriver == "awslogs" &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].logConfiguration.options["awslogs-group"] == "/opennova/prod/machines" &&
      jsondecode(aws_ecs_task_definition.hostd[0].container_definitions)[0].logConfiguration.options["awslogs-stream-prefix"] == "hostd"
    )
    error_message = "hostd reaches the fleet program by its name, reads its machine id from the user data's file, checks it against the ECS attributes, logs to the machines' group (stream hostd/hostd/<task id>), sends its servers' output to the servers' group and is healthy while its loop turns"
  }
  assert {
    condition = (
      aws_iam_role.hostd[0].name == "opennova-prod-hostd" &&
      aws_iam_role_policy.hostd[0].role == aws_iam_role.hostd[0].id &&
      aws_iam_role_policy.hostd_exec[0].role == aws_iam_role.hostd[0].id &&
      length([
        for st in data.aws_iam_policy_document.hostd[0].statement : st
        if st.sid == "ListDataTrees" && toset(st.actions) == toset(["s3:ListBucket"]) && toset(st.resources) == toset(["arn:aws:s3:::opennova-prod-ops-123456789012"]) &&
        length([for c in st.condition : c if c.test == "StringLike" && c.variable == "s3:prefix" && toset(c.values) == toset(["data/trees/*"])]) == 1
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.hostd[0].statement : st
        if st.sid == "ReadDataTrees" && toset(st.actions) == toset(["s3:GetObject"]) && toset(st.resources) == toset(["arn:aws:s3:::opennova-prod-ops-123456789012/data/trees/*"])
      ]) == 1 &&
      length(data.aws_iam_policy_document.hostd[0].statement) == 2
    )
    error_message = "hostd's task role lists and reads the trees in the store (the deployment's bucket, data/trees/) and nothing else of the bucket, and opens ECS Exec's channels"
  }
  assert {
    condition = (
      aws_iam_role.machine[0].permissions_boundary == "arn:aws:iam::123456789012:policy/opennova-machine-boundary" &&
      aws_iam_role.hostd[0].permissions_boundary == "arn:aws:iam::123456789012:policy/opennova-machine-boundary" &&
      toset(flatten([for st in jsondecode(file("${path.module}/iam/opennova-machine-boundary.json")).Statement : st.Action])) ==
      toset(flatten([
        for d in [data.aws_iam_policy_document.machine[0], data.aws_iam_policy_document.hostd[0], data.aws_iam_policy_document.exec] :
        [for st in d.statement : st.actions]
      ]))
    )
    error_message = "the machines' two roles (the instance role, hostd's task role) carry the machine boundary, which names exactly the actions their policies do"
  }
  assert {
    condition = (
      aws_iam_role_policy.web_fleet_read[0].role == aws_iam_role.program["web"].id &&
      length([
        for st in data.aws_iam_policy_document.web_fleet_read[0].statement : st
        if toset(st.actions) == toset(["ecs:ListContainerInstances"]) && toset(st.resources) == toset(["arn:aws:ecs:us-east-1:123456789012:cluster/opennova-prod-fleet"])
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.web_fleet_read[0].statement : st
        if toset(st.actions) == toset(["ecs:DescribeContainerInstances"]) && toset(st.resources) == toset(["arn:aws:ecs:us-east-1:123456789012:container-instance/opennova-prod-fleet/*"])
      ]) == 1 &&
      length([
        for st in data.aws_iam_policy_document.web_fleet_read[0].statement : st
        if toset(st.actions) == toset(["ecs:DescribeServices"]) && toset(st.resources) == toset(["arn:aws:ecs:us-east-1:123456789012:service/opennova-prod-fleet/*"])
      ]) == 1
    )
    error_message = "web reads the fleet cluster's container instances and services alone"
  }
}

# The user data the worker renders for every machine
# (apps/novaworld_common/hosted/machine/machine_user_data.sh): its
# ecs.config turns privileged containers off and never ECS's cleanup of the
# images it did not pull (the servers', which hostd runs and ECS never sees).
run "machine_ecs_config" {
  command = plan

  assert {
    condition = (
      strcontains(file("${path.module}/../../apps/novaworld_common/hosted/machine/machine_user_data.sh"), "ECS_DISABLE_PRIVILEGED=true") &&
      !strcontains(file("${path.module}/../../apps/novaworld_common/hosted/machine/machine_user_data.sh"), "ECS_ENABLE_UNTRACKED_IMAGE_CLEANUP")
    )
    error_message = "the machines' ecs.config disables privileged containers and never sets ECS_ENABLE_UNTRACKED_IMAGE_CLEANUP (it would delete the servers' images)"
  }
}

# The static-address add-on sold, with Cloudflare: the play zone (edge.tf),
# whose A records under it the worker alone writes.
run "address_policy" {
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
    hosted_enabled      = true
    fleet_max_addresses = 4
    cloudflare_zone_id  = "0123456789abcdef0123456789abcdef"
    cloudflare_domain   = "example.com"
  }

  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "WriteAddressRecords" && toset(st.actions) == toset(["route53:ChangeResourceRecordSets"]) &&
      toset(st.resources) == toset(["arn:aws:route53:::hostedzone/Z0123456789PLAYZONE"]) && length([
        for c in st.condition : c
        if(c.test == "ForAllValues:StringEquals" && c.variable == "route53:ChangeResourceRecordSetsRecordTypes" && toset(c.values) == toset(["A"])) ||
        (c.test == "ForAllValues:StringEquals" && c.variable == "route53:ChangeResourceRecordSetsActions" && toset(c.values) == toset(["UPSERT", "DELETE"])) ||
        (c.test == "ForAllValues:StringLike" && c.variable == "route53:ChangeResourceRecordSetsNormalizedRecordNames" && toset(c.values) == toset(["*.play.example.com"]))
      ]) == 3
    ]) == 1
    error_message = "the worker writes the play zone's A records under it alone, by UPSERT and DELETE alone"
  }
  assert {
    condition = length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "ReadAddressRecordChanges" && toset(st.actions) == toset(["route53:GetChange"]) &&
      toset(st.resources) == toset(["arn:aws:route53:::change/*"])
    ]) == 1
    error_message = "the worker reads a change's state"
  }
  assert {
    condition = toset(flatten([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : [for a in st.actions : a if startswith(a, "route53:")]
    ])) == toset(["route53:ChangeResourceRecordSets", "route53:GetChange"])
    error_message = "the worker's Route 53 actions are those two alone"
  }
  assert {
    condition = alltrue([
      for p in ["web", "fleet", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_MAX_ADDRESSES"] == "4" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_ADDRESS_ZONE"] == "play.example.com"
    ])
    error_message = "the fleet's policy names the addresses the fleet sells and the zone they are named in, to web, the fleet and the worker alike"
  }
  assert {
    condition = (
      { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_HOSTED_ADDRESS_ZONE_ID"] == "Z0123456789PLAYZONE" &&
      alltrue([for p in ["web", "fleet", "gate", "lobby", "data"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_HOSTED_ADDRESS_ZONE_ID")])
    )
    error_message = "the play zone's id, the addresses' driver's own, is the worker's alone"
  }
}

run "too_many_addresses_refused" {
  command = plan

  variables {
    fleet_max_addresses = 65
  }

  expect_failures = [var.fleet_max_addresses]
}

run "four_slots" {
  command = plan

  # The machines' group by id, known at plan.
  override_resource {
    target          = aws_security_group.machines[0]
    override_during = plan
    values          = { id = "sg-0machines" }
  }

  variables {
    hosted_enabled = true
    shared_slots   = 4
  }

  assert {
    condition     = aws_vpc_security_group_ingress_rule.machines_game["0.0.0.0/0"].to_port == 32771 && aws_vpc_security_group_ingress_rule.machines_admin[0].to_port == 32803
    error_message = "the port ranges follow shared_slots"
  }
  assert {
    condition     = { for e in jsondecode(aws_ecs_task_definition.program["worker"].container_definitions)[0].environment : e.name => e.value }["ONNET_FLEET_SHARED_SLOTS"] == "4"
    error_message = "the programs' environment names the slot count"
  }
}

# A larger root volume raises what a launch may make.
run "root_volume" {
  command = plan

  variables {
    hosted_enabled         = true
    machine_root_volume_gb = 64
  }

  assert {
    condition = alltrue([for t in aws_launch_template.fleet : one(t.block_device_mappings).ebs[0].volume_size == 64]) && length([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : st
      if st.sid == "LaunchMachineVolumes" && length([for c in st.condition : c if c.variable == "ec2:VolumeSize" && contains(c.values, "64")]) == 1
    ]) == 1
    error_message = "the root volume's size is the templates' and the launch policy's bound"
  }
}

# A new fleet_ami moves the templates and the launch policy together, in one
# plan.
run "fleet_ami_bump" {
  command = plan

  variables {
    hosted_enabled = true
    fleet_ami      = "al2023-ami-ecs-hvm-2023.0.20261015-kernel-6.1-x86_64"
  }

  override_data {
    target = data.aws_ami.fleet[0]
    values = { id = "ami-0fedcba9876543210", root_device_name = "/dev/xvda" }
  }

  assert {
    condition = alltrue([for t in aws_launch_template.fleet : t.image_id == "ami-0fedcba9876543210"]) && flatten([
      for st in data.aws_iam_policy_document.fleet_service[0].statement : [
        for c in st.condition : c.values if c.variable == "ec2:ImageID"
      ]
    ]) == ["ami-0fedcba9876543210"]
    error_message = "a new pinned image moves the templates and the policy together"
  }
}

# Hosting needs the image named.
run "fleet_ami_required" {
  command = plan

  variables {
    hosted_enabled = true
    fleet_ami      = ""
  }

  expect_failures = [data.aws_ami.fleet]
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
