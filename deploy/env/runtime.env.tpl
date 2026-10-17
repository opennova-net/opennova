# A deployment's runtime secrets. `./deploy/run.sh --deployment NAME secrets
# sync` reads each reference below from the vault (its run's cache: each item
# read once) and writes it to SSM Parameter Store as a SecureString,
# /opennova/<deployment>/secret/<NAME>; ECS hands each to the containers whose
# task definitions name it (infra/aws/programs.tf), as they start. Edit the
# vault, not this file.
#
# The toolbox fills in @VAULT@ (OP_VAULT, default OpenNova-Deploy), @APP_ITEM@
# (app-<deployment>) and @STRIPE_ITEM@ (stripe-live for prod, stripe-test for
# staging): nothing of one deployment's reaches the other's.
#
# A line ending in "# optional" is synced only while its field exists, and
# removed from SSM by `secrets sync --prune` once it does not. The "# billing"
# lines are synced together, and only while the Stripe item is complete and well
# formed (`secrets check`); otherwise billing stays off (the programs log
# "billing off") and none of them is synced. A task definition names the
# others (infra/aws/programs.tf), which a deploy requires; the optional and
# billing ones are in its `opennova.optional-secrets` label, and a deploy names
# those SSM holds.
ADMIN_API_TOKEN=op://@VAULT@/@APP_ITEM@/admin_api_token
ADMIN_BASIC_AUTH_USER=op://@VAULT@/@APP_ITEM@/admin_basic_auth_user
ADMIN_BASIC_AUTH_PASSWORD=op://@VAULT@/@APP_ITEM@/admin_basic_auth_password
# The service's database role's password (`db bootstrap` sets it on the role).
DATABASE_PASSWORD=op://@VAULT@/@APP_ITEM@/database_password
# The cache's AUTH token (infra/aws/cache.tf takes the same field).
ONNET_REDIS_PASSWORD=op://@VAULT@/@APP_ITEM@/redis_password
ONNET_BOOTSTRAP_ADMIN=op://@VAULT@/@APP_ITEM@/bootstrap_admin # optional
STRIPE_SECRET_KEY=op://@VAULT@/@STRIPE_ITEM@/secret_key # billing
STRIPE_WEBHOOK_SECRET=op://@VAULT@/@STRIPE_ITEM@/webhook_secret # billing
STRIPE_PRICE_TIER1=op://@VAULT@/@STRIPE_ITEM@/price_tier1 # billing
STRIPE_PRICE_TIER2=op://@VAULT@/@STRIPE_ITEM@/price_tier2 # billing
STRIPE_PRICE_TIER3=op://@VAULT@/@STRIPE_ITEM@/price_tier3 # billing
STRIPE_PRICE_OWN_MACHINE=op://@VAULT@/@STRIPE_ITEM@/price_own_machine # billing
# Edge authentication: the deployment's own secret, which the zone's Transform
# Rule stamps on that deployment's web hostnames alone (infra/zone). The
# previous one only during a rotation (DEPLOY.md).
EDGE_AUTH_SECRET=op://@VAULT@/@APP_ITEM@/edge_auth_secret
EDGE_AUTH_SECRET_PREVIOUS=op://@VAULT@/@APP_ITEM@/edge_auth_secret_previous # optional
