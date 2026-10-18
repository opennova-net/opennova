# Outbound mail through Amazon SES (mail.tf), planned against mock providers
# whose values reach the plan: offline, no credentials, nothing reaches AWS or
# Cloudflare. From the repo root:
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> init -backend=false
#   docker run --rm -v "$PWD:/w" -w /w/infra/aws hashicorp/terraform:<the toolbox's> test
# With a sender, prod makes, in its own account, the domain's SES identity with
# its custom MAIL FROM domain, the three DKIM CNAMEs and the MAIL FROM's MX and
# SPF records in the zone (DNS only), an identity for the forward_to address
# (the sandbox's recipient), the worker's one permission (ses:SendEmail through
# those identities, from the sender alone; web queues the mail and sends none)
# and the sender and region in web's and the worker's environment. With none
# (the default), or without Cloudflare, none of it; a malformed sender is
# refused. (staging_mail.tftest.hcl: staging's own identities, under
# staging.<domain>.)

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

override_resource {
  target          = aws_sesv2_email_identity.domain
  override_during = plan
  values = {
    dkim_signing_attributes = { tokens = ["tok1", "tok2", "tok3"] }
  }
}

variables {
  deployment                = "prod"
  aws_account_id            = "123456789012"
  region                    = "us-east-1"
  service_eip_allocation_id = "eipalloc-0123456789abcdef0"
  db_master_password        = "0123456789abcdef0123456789abcdef"
  cache_auth_token          = "fedcba9876543210fedcba9876543210"
  cloudflare_zone_id        = "0123456789abcdef0123456789abcdef"
  cloudflare_domain         = "example.com"
  email_forward_to          = "maintainer@mail.example.org"
  mail_sender               = "support"
}

run "mail" {
  command = plan

  assert {
    condition = (
      aws_sesv2_email_identity.domain[0].email_identity == "example.com" &&
      aws_sesv2_email_identity_mail_from_attributes.domain[0].mail_from_domain == "bounce.example.com" &&
      aws_sesv2_email_identity_mail_from_attributes.domain[0].behavior_on_mx_failure == "USE_DEFAULT_VALUE"
    )
    error_message = "the domain's identity, its MAIL FROM domain bounce.<domain>"
  }
  assert {
    condition = (
      length(cloudflare_record.mail_dkim) == 3 &&
      alltrue([for i, r in cloudflare_record.mail_dkim :
        r.type == "CNAME" && !r.proxied && r.zone_id == "0123456789abcdef0123456789abcdef" &&
        r.name == "tok${i + 1}._domainkey" && r.content == "tok${i + 1}.dkim.amazonses.com"
      ])
    )
    error_message = "Easy DKIM's three CNAMEs, DNS only, in the zone"
  }
  assert {
    condition = (
      cloudflare_record.mail_from_mx[0].name == "bounce" && cloudflare_record.mail_from_mx[0].type == "MX" &&
      cloudflare_record.mail_from_mx[0].content == "feedback-smtp.us-east-1.amazonses.com" &&
      cloudflare_record.mail_from_mx[0].priority == 10 &&
      cloudflare_record.mail_from_spf[0].name == "bounce" && cloudflare_record.mail_from_spf[0].type == "TXT" &&
      cloudflare_record.mail_from_spf[0].content == "v=spf1 include:amazonses.com ~all"
    )
    error_message = "the MAIL FROM domain's MX (SES's feedback endpoint) and SPF"
  }
  assert {
    condition     = aws_sesv2_email_identity.forward_to[0].email_identity == "maintainer@mail.example.org"
    error_message = "the forward_to address is an identity (the sandbox's recipient)"
  }
  assert {
    condition = (
      length(data.aws_iam_policy_document.mail[0].statement) == 1 &&
      one([for st in data.aws_iam_policy_document.mail[0].statement :
        toset(st.actions) == toset(["ses:SendEmail"]) &&
        toset(st.resources) == toset(["arn:aws:ses:us-east-1:123456789012:identity/*"]) &&
        one([for c in st.condition : c.test == "StringEquals" && c.variable == "ses:FromAddress" && toset(c.values) == toset(["support@example.com"])])
      ]) &&
      toset(keys(aws_iam_role_policy.mail)) == toset(["worker"]) &&
      aws_iam_role_policy.mail["worker"].name == "opennova-prod-worker-mail" &&
      aws_iam_role_policy.mail["worker"].role == aws_iam_role.program["worker"].id
    )
    error_message = "the worker alone sends, through any identity of the account (SES also authorizes a verified recipient), from support@<domain> alone (web queues mail.send jobs)"
  }
  assert {
    condition = alltrue([
      for p in ["web", "worker"] :
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_MAIL_FROM"] == "support@example.com" &&
      { for e in jsondecode(aws_ecs_task_definition.program[p].container_definitions)[0].environment : e.name => e.value }["ONNET_MAIL_REGION"] == "us-east-1"
    ]) && alltrue([for p in ["gate", "lobby", "controller"] : !strcontains(aws_ecs_task_definition.program[p].container_definitions, "ONNET_MAIL_")])
    error_message = "web's and the worker's environment name the sender and SES's region, no other program's"
  }
}

run "no_forward_to" {
  command = plan

  variables {
    email_forward_to = "REPLACE_ME"
  }

  assert {
    condition = (
      length(aws_sesv2_email_identity.domain) == 1 && length(aws_sesv2_email_identity.forward_to) == 0 &&
      one([for st in data.aws_iam_policy_document.mail[0].statement :
        toset(st.resources) == toset(["arn:aws:ses:us-east-1:123456789012:identity/example.com"])
      ])
    )
    error_message = "with no destination, the domain's identity alone"
  }
}

run "off" {
  command = plan

  variables {
    mail_sender = ""
  }

  assert {
    condition = (
      length(aws_sesv2_email_identity.domain) == 0 && length(aws_sesv2_email_identity.forward_to) == 0 &&
      length(aws_sesv2_email_identity_mail_from_attributes.domain) == 0 && length(cloudflare_record.mail_dkim) == 0 &&
      length(cloudflare_record.mail_from_mx) == 0 && length(cloudflare_record.mail_from_spf) == 0 &&
      length(aws_iam_role_policy.mail) == 0 && alltrue([for p, d in aws_ecs_task_definition.program : !strcontains(d.container_definitions, "ONNET_MAIL_")])
    )
    error_message = "no sender: no mail, nothing made"
  }
}

run "no_cloudflare" {
  command = plan

  variables {
    cloudflare_zone_id = ""
    cloudflare_domain  = ""
  }

  assert {
    condition     = length(aws_sesv2_email_identity.domain) == 0 && length(aws_iam_role_policy.mail) == 0
    error_message = "without Cloudflare: no mail"
  }
}

run "uppercase_sender_refused" {
  command = plan

  variables {
    mail_sender = "Support"
  }

  expect_failures = [var.mail_sender]
}
