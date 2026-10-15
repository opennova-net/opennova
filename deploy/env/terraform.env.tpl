# 1Password references for a Terraform run: the AWS and Cloudflare credentials
# and the TF_VAR_* secrets. `op run` resolves them against the vault at deploy
# time; the values reach terraform's environment alone, never the disk outside
# the toolbox's tmpfs. Edit the vault, not this file. The toolbox fills in
# @VAULT@ (OP_VAULT, default OpenNova-Deploy) and @RDS_ITEM@ (rds for prod,
# rds-<deployment> otherwise; `secrets init` makes it). The database's master
# password is write-only in Terraform: it never enters the state.
AWS_ACCESS_KEY_ID=op://@VAULT@/aws/access_key_id
AWS_SECRET_ACCESS_KEY=op://@VAULT@/aws/secret_access_key
AWS_REGION=op://@VAULT@/aws/region
AWS_DEFAULT_REGION=op://@VAULT@/aws/region
TF_VAR_cloudflare_api_token=op://@VAULT@/cloudflare/api_token
TF_VAR_cloudflare_zone_id=op://@VAULT@/cloudflare/zone_id
TF_VAR_cloudflare_domain=op://@VAULT@/cloudflare/domain
TF_VAR_db_master_password=op://@VAULT@/@RDS_ITEM@/master_password
TF_VAR_alert_email=op://@VAULT@/alerts/email
