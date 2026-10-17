# The deployment's operations bucket (ops_bucket.tf), planned against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare. From
# the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# Versioned and encrypted, private, named after the deployment and its
# account; the dumps expire and the releases' old versions go as before. The
# game data (data/, ADR 0064; one bucket per deployment, so no deployment
# segment): an upload abandoned 7 days aborted, the trees' old versions kept
# 7 days and the packages' 30 (a rule per prefix, since of two rules on one
# key S3 keeps the shorter); no CORS rule (the admin page's uploads pass
# through the site). Staging's bucket carries the same rules.

mock_provider "aws" {
  override_during = plan

  mock_data "aws_availability_zones" {
    defaults = { names = ["us-east-1a", "us-east-1b", "us-east-1c"] }
  }
  mock_data "aws_ami" {
    defaults = { id = "ami-0123456789abcdef0", root_device_name = "/dev/sda1" }
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
  target          = aws_security_group.program["controller"]
  override_during = plan
  values          = { id = "sg-0controller" }
}
override_resource {
  target          = aws_security_group.program["worker"]
  override_during = plan
  values          = { id = "sg-0worker" }
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
  target          = aws_ecs_task_definition.program["controller"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-controller:1" }
}
override_resource {
  target          = aws_ecs_task_definition.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:ecs:us-east-1:123456789012:task-definition/opennova-prod-worker:1" }
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
  target          = aws_iam_role.program["controller"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-controller" }
}
override_resource {
  target          = aws_iam_role.program["worker"]
  override_during = plan
  values          = { arn = "arn:aws:iam::123456789012:role/opennova-prod-worker" }
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

variables {
  deployment                = "prod"
  aws_account_id            = "123456789012"
  region                    = "us-east-1"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
}

run "bucket" {
  command = plan

  assert {
    condition = (
      aws_s3_bucket.ops.bucket == "opennova-prod-ops-123456789012" &&
      aws_s3_bucket_versioning.ops.versioning_configuration[0].status == "Enabled" &&
      aws_s3_bucket_public_access_block.ops.block_public_acls && aws_s3_bucket_public_access_block.ops.restrict_public_buckets
    )
    error_message = "the deployment's bucket, named after it and its account: versioned, private"
  }
  assert {
    condition = (
      toset([for r in aws_s3_bucket_lifecycle_configuration.ops.rule : r.id]) == toset([
        "expire-old-backups", "release-old-versions", "data-abandoned-uploads",
        "data-trees-old-versions", "data-updates-old-versions",
      ]) &&
      alltrue([for r in aws_s3_bucket_lifecycle_configuration.ops.rule : r.status == "Enabled"])
    )
    error_message = "the dumps' and the releases' rules as before, and the game data's: the abandoned uploads, the trees and the packages"
  }
  assert {
    condition = alltrue([
      for r in aws_s3_bucket_lifecycle_configuration.ops.rule :
      one(r.filter).prefix == "data/" && one(r.abort_incomplete_multipart_upload).days_after_initiation == 7 && length(r.noncurrent_version_expiration) == 0 && length(r.expiration) == 0
      if r.id == "data-abandoned-uploads"
    ])
    error_message = "an upload abandoned under data/ is aborted after 7 days, and that rule expires nothing"
  }
  assert {
    condition = alltrue(concat(
      [for r in aws_s3_bucket_lifecycle_configuration.ops.rule : one(r.filter).prefix == "data/trees/" && one(r.noncurrent_version_expiration).noncurrent_days == 7 && length(r.expiration) == 0 if r.id == "data-trees-old-versions"],
      [for r in aws_s3_bucket_lifecycle_configuration.ops.rule : one(r.filter).prefix == "data/updates/" && one(r.noncurrent_version_expiration).noncurrent_days == 30 && length(r.expiration) == 0 if r.id == "data-updates-old-versions"],
    ))
    error_message = "the trees' old versions go after 7 days, the packages' after 30; neither rule expires a current object"
  }
  assert {
    condition = alltrue([
      for r in aws_s3_bucket_lifecycle_configuration.ops.rule :
      length(r.abort_incomplete_multipart_upload) == 0
      if r.id != "data-abandoned-uploads"
    ])
    error_message = "no other rule aborts uploads"
  }
}

# Staging's bucket carries the same rules.
run "staging" {
  command = plan

  variables {
    deployment                = "staging"
    service_eip_allocation_id = ""
  }

  assert {
    condition = (
      aws_s3_bucket.ops.bucket == "opennova-staging-ops-123456789012" &&
      length(aws_s3_bucket_lifecycle_configuration.ops.rule) == 5
    )
    error_message = "staging's own bucket, the same five rules"
  }
}
