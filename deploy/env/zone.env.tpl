# 1Password references for a run of the Cloudflare zone's stack (infra/zone:
# the zone's settings, each deployment's edge authentication rule, the Stripe
# webhook's rule, Email Routing). One stack for the whole zone, so it reads
# every deployment's edge secret: prod's from app-prod, staging's from
# app-staging (`secrets init` makes each). Email Routing forwards support@
# and the member accounts' root addresses (aws-prod@, aws-staging@) to
# email/forward_to. The toolbox resolves them from its run's
# cache of the vault (each item read once per run; on-deploy, "The vault");
# the values reach terraform's environment alone. The
# toolbox fills in @VAULT@ (OP_VAULT, default OpenNova-Deploy).
TF_VAR_cloudflare_api_token=op://@VAULT@/cloudflare/api_token
TF_VAR_cloudflare_zone_id=op://@VAULT@/cloudflare/zone_id
TF_VAR_cloudflare_domain=op://@VAULT@/cloudflare/domain
TF_VAR_edge_auth_secret_prod=op://@VAULT@/app-prod/edge_auth_secret
TF_VAR_edge_auth_secret_staging=op://@VAULT@/app-staging/edge_auth_secret
# Where support@<domain> forwards; REPLACE_ME keeps email routing off.
TF_VAR_email_forward_to=op://@VAULT@/email/forward_to
