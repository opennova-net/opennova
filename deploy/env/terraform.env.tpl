# 1Password references for a run of a deployment's stack (infra/aws): the
# Cloudflare credentials and the TF_VAR_* secrets. `op run` resolves them
# against the vault at deploy time; the values reach terraform's environment
# alone, never the disk outside the toolbox's tmpfs. Edit the vault, not this
# file. The AWS credentials are the deployment's deploy user's, which the
# toolbox puts in the environment before it runs terraform
# (deploy/env/aws.env.tpl), and the account, region and prod's gate address
# come from the same vault item, aws-<deployment>. The
# toolbox fills in @VAULT@ (OP_VAULT, default OpenNova-Deploy), @APP_ITEM@
# (app-<deployment>) and @RDS_ITEM@ (rds-<deployment>; `secrets init` makes
# both). The database's master password is write-only in Terraform: it never
# enters the state.
TF_VAR_cloudflare_api_token=op://@VAULT@/cloudflare/api_token
TF_VAR_cloudflare_zone_id=op://@VAULT@/cloudflare/zone_id
TF_VAR_cloudflare_domain=op://@VAULT@/cloudflare/domain
TF_VAR_db_master_password=op://@VAULT@/@RDS_ITEM@/master_password
# The cache's AUTH token (infra/aws/cache.tf), the same field the service gets as
# ONNET_REDIS_PASSWORD (runtime.env.tpl).
TF_VAR_cache_auth_token=op://@VAULT@/@APP_ITEM@/redis_password
TF_VAR_alert_email=op://@VAULT@/alerts/email
# The SES sandbox's recipient (infra/aws/mail.tf): each account verifies it.
TF_VAR_email_forward_to=op://@VAULT@/email/forward_to
