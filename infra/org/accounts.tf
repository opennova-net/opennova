# ---------------------------------------------------------------------------
# Each member account's baseline (account/), through that account's provider
# ---------------------------------------------------------------------------
#
# infra/aws's state bucket, the role boundary, the deploy user the toolbox acts
# as for every command of the deployment (its key goes to the vault), and the
# role the deploy workflow's environment of the deployment's name assumes.
# prod's state bucket also holds the shared stacks' states (infra/zone,
# infra/github) and this stack's own, so prod's deploy user reaches the shared
# stacks' keys too (never this stack's).

module "prod" {
  source = "./account"
  providers = {
    aws = aws.prod
  }

  deployment        = "prod"
  account_id        = var.aws_account_id_prod
  region            = var.region
  github_repository = var.github_repository
  state_stacks      = ["aws", "zone", "github"]
}

module "staging" {
  source = "./account"
  providers = {
    aws = aws.staging
  }

  deployment        = "staging"
  account_id        = var.aws_account_id_staging
  region            = var.region
  github_repository = var.github_repository
  state_stacks      = ["aws"]
}
