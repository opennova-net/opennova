# The service database (database.tf), planned and applied against mock
# providers: offline, no credentials, nothing reaches AWS or Cloudflare. From the
# repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# PostgreSQL 17 in the private subnets, single-AZ beside the service host,
# never public, encrypted, TLS forced, 14 days of point-in-time recovery, a
# final snapshot and deletion protection; reached from the service host's
# security group alone; its master password write-only; infra.env hands the
# server a verify-full URL with no password in it.

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

variables {
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
}

run "database" {
  command = plan

  assert {
    condition     = aws_db_instance.main.engine == "postgres" && aws_db_instance.main.engine_version == "17" && aws_db_instance.main.instance_class == "db.t4g.micro"
    error_message = "PostgreSQL 17 on db.t4g.micro"
  }
  assert {
    condition     = !aws_db_instance.main.publicly_accessible && !aws_db_instance.main.multi_az && aws_db_instance.main.availability_zone == aws_subnet.public["a"].availability_zone
    error_message = "private, single-AZ, in the service host's zone"
  }
  assert {
    condition     = aws_db_instance.main.storage_encrypted && aws_db_instance.main.storage_type == "gp3" && aws_db_instance.main.allocated_storage == 20 && aws_db_instance.main.max_allocated_storage == 100
    error_message = "encrypted gp3, 20 GiB growing to 100"
  }
  assert {
    condition     = aws_db_instance.main.backup_retention_period == 14 && !aws_db_instance.main.delete_automated_backups && aws_db_instance.main.deletion_protection && !aws_db_instance.main.skip_final_snapshot
    error_message = "14 days of point-in-time recovery kept past a deletion, deletion protection and a final snapshot"
  }
  assert {
    condition     = aws_db_instance.main.password_wo_version == 1 && aws_db_instance.main.password == null
    error_message = "the master password is write-only (password_wo), never the stored password argument"
  }
  assert {
    condition     = aws_db_instance.main.auto_minor_version_upgrade && aws_db_instance.main.performance_insights_enabled
    error_message = "minor upgrades on, performance insights on"
  }
  assert {
    condition = (
      length([for p in aws_db_parameter_group.main.parameter : p if p.name == "rds.force_ssl" && p.value == "1"]) == 1 &&
      length([for p in aws_db_parameter_group.main.parameter : p if p.name == "log_min_duration_statement" && p.value == "1000"]) == 1 &&
      aws_db_parameter_group.main.family == "postgres17"
    )
    error_message = "the parameter group forces TLS and logs statements over a second"
  }
  assert {
    condition     = toset(aws_db_subnet_group.main.subnet_ids) == toset([aws_subnet.private["a"].id, aws_subnet.private["b"].id])
    error_message = "the database lives in the private subnets"
  }
  assert {
    condition     = aws_vpc_security_group_ingress_rule.db_from_service.from_port == 5432 && aws_vpc_security_group_ingress_rule.db_from_service.cidr_ipv4 == null
    error_message = "the database admits port 5432 by security group, never by range"
  }
}

run "database_wiring" {
  command = plan

  assert {
    condition     = aws_vpc_security_group_ingress_rule.db_from_service.referenced_security_group_id == aws_security_group.service.id && aws_vpc_security_group_ingress_rule.db_from_service.security_group_id == aws_security_group.db.id
    error_message = "the database admits the service host's security group alone"
  }
  assert {
    condition     = strcontains(aws_ssm_parameter.infra_env.value, "DATABASE_URL='postgresql://novaworld@opennova-server-db.abcdefghijkl.us-east-1.rds.amazonaws.com:5432/novaworld?sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem'\n")
    error_message = "infra.env hands the server a verify-full URL with no password in it"
  }
  assert {
    condition     = output.db_endpoint == "opennova-server-db.abcdefghijkl.us-east-1.rds.amazonaws.com" && output.db_port == 5432
    error_message = "the outputs name the endpoint"
  }
}

run "short_master_password_refused" {
  command = plan

  variables {
    db_master_password = "short"
  }

  expect_failures = [var.db_master_password]
}
