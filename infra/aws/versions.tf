# The NovaWorld service's AWS + Cloudflare infrastructure for one deployment
# (DEPLOY.md, section 4).
#
# Flat files by concern, no child modules:
#   versions.tf    Terraform and provider pins, the backend, provider configuration
#   locals.tf      the deployment, its account and region, the names derived
#   variables.tf   every input
#   network.tf     the VPC: public subnets a/b (tasks, load balancer, machines),
#                  private a/b
#   service.tf     what the service's programs share; the gate's Elastic IP
#   cluster.tf     the ECS cluster, the fleet program's Cloud Map name, the
#                  task execution role
#   programs.tf    the six programs: gate, lobby, web, fleet, worker, data (an
#                  ECS service on Fargate each), their security groups, task
#                  roles, task definitions and task counts
#   load_balancer.tf  the public network load balancer: the gate, the lobby and web
#   backup.tf      the nightly pg_dump, a scheduled ECS task
#   service_ssm.tf what the deploy toolbox reads (infra.env) and the release
#   database.tf    PostgreSQL 17 on RDS
#   cache.tf       Valkey on ElastiCache: web's ephemeral state
#   edge.tf        Cloudflare: the deployment's site records and its origin
#                  certificate (the zone's own settings and rules are infra/zone's)
#   mail.tf        outbound mail: the deployment's Amazon SES identity, its DKIM
#                  and MAIL FROM records, the service's permission to send
#   ops_bucket.tf  the deployment's operations bucket (releases, dumps)
#   fleet.tf       the hosted game servers' machines: shared and own launch templates
#   observability.tf  logs, metrics, alarms, budgets
#   outputs.tf     what the deploy toolbox and operators read
# (The GitHub deploy workflow's OIDC provider and role are infra/org's, made
# in each account beside the toolbox's deploy user.)
#
# One deployment per AWS account: prod in opennova-prod, staging in
# opennova-staging. The same configuration is applied to each, var.deployment
# saying which and var.aws_account_id and var.region naming its account and
# region (the toolbox fills them, the account and the region from the
# deployment's vault item, aws-<deployment>: no tracked file names an account,
# an ARN or a bucket). Each keeps its state in its own account, in the
# S3 bucket infra/org makes there (key infra/aws/terraform.tfstate, an S3
# lock file), which the deploy toolbox names at init (`./deploy/run.sh
# --deployment NAME infra ...`, deploy/bin/lib/infra.sh). The provider refuses
# credentials of any account but the deployment's.

terraform {
  # 1.11: write-only arguments (the database's password_wo) and ephemeral
  # variables. deploy/Dockerfile and .github/workflows/infra.yml pin the same
  # Terraform.
  required_version = ">= 1.11.0"

  # The deployment's state, in its own account: partial, the toolbox gives
  # the bucket, key and region at init (use_lockfile: S3's own lock file, no
  # DynamoDB table). The offline checks init with -backend=false.
  backend "s3" {}

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.100"
    }
    cloudflare = {
      source  = "cloudflare/cloudflare"
      version = "~> 4.0"
    }
    # The origin certificate's key (edge.tf).
    tls = {
      source  = "hashicorp/tls"
      version = "~> 4.0"
    }
  }
}

provider "aws" {
  region = local.region

  # The deployment's own account and no other: a run holding another
  # account's credentials (staging's role on a prod plan, say) stops before it
  # reads or writes anything.
  allowed_account_ids = [var.aws_account_id]

  # Every resource this stack makes carries the deployment's tag
  # (opennova-prod, opennova-staging), the one the service puts on the machines
  # it launches (fleet.tf, hosted_deployment) and the deploy user's policies
  # scope their creates and changes by (infra/aws/iam).
  default_tags {
    tags = {
      "opennova:deployment" = local.hosted_deployment
    }
  }
}

provider "cloudflare" {
  api_token = var.cloudflare_api_token
}
