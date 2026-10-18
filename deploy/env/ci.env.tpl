# 1Password references for a run of the CI runners' stack (infra/ci): the ci
# account and its region (item aws-ci, made by hand like the deployments'
# accounts), RunsOn's license key (item runs-on, field key) and where its
# alerts and the account's budget write. The toolbox resolves them from its
# run's cache of the vault (each item read once per run; on-deploy, "The
# vault"); the values reach terraform's environment alone. The AWS
# credentials are the management account's opennova-org user's (item
# aws-org), which the toolbox puts in the environment before it runs
# terraform. The toolbox fills in @VAULT@ (OP_VAULT, default OpenNova-Deploy).
TF_VAR_region=op://@VAULT@/aws-ci/region
TF_VAR_aws_account_id_ci=op://@VAULT@/aws-ci/account_id
TF_VAR_license_key=op://@VAULT@/runs-on/key
TF_VAR_alert_email=op://@VAULT@/alerts/email
