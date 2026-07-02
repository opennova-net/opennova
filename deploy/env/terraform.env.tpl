# 1Password-referenced terraform credentials. `op inject`/`op run` resolves
# these 1Password references against the vault at deploy time; the resolved
# values never touch disk outside the tmpfs. Edit the vault, not this file.
AWS_ACCESS_KEY_ID=op://OpenNova-Deploy/aws/access_key_id
AWS_SECRET_ACCESS_KEY=op://OpenNova-Deploy/aws/secret_access_key
AWS_REGION=op://OpenNova-Deploy/aws/region
AWS_DEFAULT_REGION=op://OpenNova-Deploy/aws/region
TF_VAR_cloudflare_api_token=op://OpenNova-Deploy/cloudflare/api_token
TF_VAR_cloudflare_zone_id=op://OpenNova-Deploy/cloudflare/zone_id
TF_VAR_cloudflare_domain=op://OpenNova-Deploy/cloudflare/domain
TF_VAR_ssh_public_key=op://OpenNova-Deploy/ssh/public key
