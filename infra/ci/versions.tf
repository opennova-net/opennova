# The CI runners (DEPLOY.md, "CI runners"): RunsOn's ephemeral EC2 runners for
# ci.yml's build and test legs, in the Organization's ci account, which holds
# them alone. Each job gets an instance of its own, spot by default, launched
# for it and terminated after it; GitHub reaches the stack through a GitHub App
# registered once from its setup page.
#
#   versions.tf  Terraform and provider pins, the ci account's provider
#   variables.tf every input
#   network.tf   the runners' VPC: public subnets, an internet gateway, no NAT
#   runners.tf   RunsOn's Flex stack, the account's monthly budget and its S3
#                public access block
#   outputs.tf   the setup page's address
#
# The ci account has no deploy user: the toolbox runs this stack as the
# management account's opennova-org user through the ci account's
# OrganizationAccountAccessRole (`./deploy/run.sh ci plan|apply|import|output|list`,
# no --deployment), its inputs from the vault (deploy/env/ci.env.tpl), its state
# in prod's state bucket beside the org stack's (key infra/ci/terraform.tfstate),
# reached the same way.

terraform {
  # The toolbox's Terraform (deploy/Dockerfile), as infra/aws.
  required_version = ">= 1.11.0"

  required_providers {
    # RunsOn's module asks for 6.45 or later; the other stacks' pins stay theirs.
    aws = {
      source  = "hashicorp/aws"
      version = "~> 6.45"
    }
  }

  # The toolbox gives bucket (the vault's aws-prod/state_bucket), key
  # infra/ci/terraform.tfstate, region, use_lockfile, encrypt and assume_role
  # (prod's OrganizationAccountAccessRole).
  backend "s3" {}
}

# The ci account, as its OrganizationAccountAccessRole (the role Organizations
# made with the account, trusting the management account).
provider "aws" {
  region              = var.region
  allowed_account_ids = [var.aws_account_id_ci]

  assume_role {
    role_arn     = "arn:aws:iam::${var.aws_account_id_ci}:role/OrganizationAccountAccessRole"
    session_name = "opennova-ci"
  }

  default_tags {
    tags = {
      "opennova:deployment" = "opennova-ci"
    }
  }
}
