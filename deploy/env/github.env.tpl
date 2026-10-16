# The 1Password references for a run of the GitHub stack (infra/github, the
# organization opennova-net): the token the github provider reads from
# GITHUB_TOKEN, and the inputs kept out of this public repository as TF_VAR_*
# (the billing email, the members as JSON, the secrets' values, and each
# deployment's AWS account and region, from which the deploy environments'
# variables name their roles). `op run` resolves them against the vault at run
# time; the values reach terraform's environment alone. The toolbox fills in
# @VAULT@ (OP_VAULT, default OpenNova-Deploy). DEPLOY.md, "GitHub", says what
# each item holds; edit the vault, not this file.
GITHUB_TOKEN=op://@VAULT@/github-terraform/token
TF_VAR_billing_email=op://@VAULT@/github-terraform/billing_email
TF_VAR_members=op://@VAULT@/github-terraform/members
TF_VAR_reference_assets_token=op://@VAULT@/reference-assets/token
TF_VAR_stripe_test_secret_key=op://@VAULT@/stripe-test/restricted_key
TF_VAR_aws_account_id_prod=op://@VAULT@/aws-prod/account_id
TF_VAR_aws_account_id_staging=op://@VAULT@/aws-staging/account_id
TF_VAR_aws_region_prod=op://@VAULT@/aws-prod/region
TF_VAR_aws_region_staging=op://@VAULT@/aws-staging/region
