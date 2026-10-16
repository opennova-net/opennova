# A member account's baseline (../accounts.tf calls it once per deployment,
# with that account's provider): infra/aws's state bucket, the role boundary,
# the deploy user the toolbox acts as there, and the role the deploy workflow
# assumes there.
#
#   state.tf   the state bucket, the account's S3 public access block
#   deploy.tf  the role boundary, the six deploy policies, the deploy user and
#              its key, its state access and its guard
#   github.tf  GitHub's OIDC provider and the deploy workflow's role
#   outputs.tf what the org stack hands the toolbox

terraform {
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.100"
    }
  }
}
