# The VPC (network.tf), planned against mock providers: offline, no
# credentials, nothing reaches AWS or Cloudflare (the "Infra" workflow runs it;
# from the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# ). Two public and two private subnets in two zones carved from 10.44.0.0/16,
# public a keeping the range and zone the single-file layout gave its one subnet;
# the private subnets route nowhere but the VPC; the zones can be pinned.

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

# Prod adopts its Elastic IP (service.tf).
variables {
  db_master_password        = "0123456789abcdef0123456789abcdef"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
}

run "subnets" {
  command = plan

  assert {
    condition     = aws_vpc.main.cidr_block == "10.44.0.0/16"
    error_message = "the VPC keeps 10.44.0.0/16"
  }
  assert {
    condition     = aws_subnet.public["a"].cidr_block == "10.44.0.0/24" && aws_subnet.public["a"].availability_zone == "us-east-1a"
    error_message = "public a keeps the single-file layout's subnet: 10.44.0.0/24 in the region's first zone"
  }
  assert {
    condition     = aws_subnet.public["b"].cidr_block == "10.44.1.0/24" && aws_subnet.public["b"].availability_zone == "us-east-1b"
    error_message = "public b: 10.44.1.0/24 in the second zone"
  }
  assert {
    condition     = aws_subnet.private["a"].cidr_block == "10.44.16.0/24" && aws_subnet.private["b"].cidr_block == "10.44.17.0/24"
    error_message = "private a/b: 10.44.16.0/24 and 10.44.17.0/24"
  }
  assert {
    condition     = aws_subnet.private["a"].availability_zone == aws_subnet.public["a"].availability_zone && aws_subnet.private["b"].availability_zone == aws_subnet.public["b"].availability_zone
    error_message = "private a shares public a's zone (the database beside the service instance), b b's"
  }
  assert {
    condition     = alltrue([for s in aws_subnet.public : s.map_public_ip_on_launch]) && alltrue([for s in aws_subnet.private : !s.map_public_ip_on_launch])
    error_message = "public subnets hand out public addresses, private ones never"
  }
  assert {
    condition     = length(aws_route_table.private.route) == 0
    error_message = "the private route table has the VPC's local route alone (no internet gateway, no NAT)"
  }
  assert {
    condition     = length([for r in aws_route_table.public.route : r if r.cidr_block == "0.0.0.0/0"]) == 1
    error_message = "the public route table routes the internet through the gateway"
  }
  assert {
    condition     = length(aws_route_table_association.public) == 2 && length(aws_route_table_association.private) == 2
    error_message = "each subnet is associated with its table"
  }
}

run "pinned_zones" {
  command = plan

  variables {
    azs = ["us-east-1d", "us-east-1f"]
  }

  assert {
    condition     = aws_subnet.public["a"].availability_zone == "us-east-1d" && aws_subnet.private["b"].availability_zone == "us-east-1f"
    error_message = "pinned zones place a and b"
  }
}

run "one_zone_refused" {
  command = plan

  variables {
    azs = ["us-east-1a"]
  }

  expect_failures = [var.azs]
}
