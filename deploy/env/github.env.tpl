# The 1Password references for a run of the GitHub stack (infra/github, the
# organization opennova-net): the token the github provider reads from
# GITHUB_TOKEN, and the inputs kept out of this public repository as TF_VAR_*
# (the billing email, the members as JSON, the secrets' values). `op run`
# resolves them against the vault at run time; the values reach terraform's
# environment alone. The toolbox fills in @VAULT@ (OP_VAULT, default
# OpenNova-Deploy). DEPLOY.md, "GitHub", says what each item holds; edit the
# vault, not this file.
GITHUB_TOKEN=op://@VAULT@/github-terraform/token
TF_VAR_billing_email=op://@VAULT@/github-terraform/billing_email
TF_VAR_members=op://@VAULT@/github-terraform/members
TF_VAR_reference_assets_token=op://@VAULT@/reference-assets/token
