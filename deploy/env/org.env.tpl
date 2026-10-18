# 1Password references for a run of the Organization's stack (infra/org): the
# Organization and each member account's name, root address and id (aws-prod,
# aws-staging, and aws-ci, the CI runners' account), which the stack adopts
# and builds on. The toolbox resolves them from its run's cache of
# the vault (each item read once per run; on-deploy, "The vault"); the values
# reach terraform's environment alone. The AWS credentials are
# the management account's opennova-org user's (item aws-org), which the
# toolbox puts in the environment before it runs terraform. The toolbox fills
# in @VAULT@ (OP_VAULT, default OpenNova-Deploy).
TF_VAR_region=op://@VAULT@/aws-org/region
TF_VAR_organization_id=op://@VAULT@/aws-org/organization_id
TF_VAR_management_account_id=op://@VAULT@/aws-org/account_id
TF_VAR_aws_account_id_prod=op://@VAULT@/aws-prod/account_id
TF_VAR_aws_account_id_staging=op://@VAULT@/aws-staging/account_id
TF_VAR_account_name_prod=op://@VAULT@/aws-prod/account_name
TF_VAR_account_name_staging=op://@VAULT@/aws-staging/account_name
TF_VAR_root_email_prod=op://@VAULT@/aws-prod/root_email
TF_VAR_root_email_staging=op://@VAULT@/aws-staging/root_email
TF_VAR_aws_account_id_ci=op://@VAULT@/aws-ci/account_id
TF_VAR_account_name_ci=op://@VAULT@/aws-ci/account_name
TF_VAR_root_email_ci=op://@VAULT@/aws-ci/root_email
# The maintainer's IAM Identity Center user (its user name is the email), whom
# the stack gives the two permission sets in every member account.
TF_VAR_sso_given_name=op://@VAULT@/aws-org/sso_given_name
TF_VAR_sso_family_name=op://@VAULT@/aws-org/sso_family_name
TF_VAR_sso_email=op://@VAULT@/aws-org/sso_email
