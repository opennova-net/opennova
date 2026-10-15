# A deployment's runtime secrets. `./deploy/run.sh [--deployment NAME] secrets
# sync` reads each reference below from the vault and writes it to SSM
# Parameter Store as a SecureString, /opennova/<deployment>/secret/<NAME>; the
# service host's converge turns them into a memory-only env file for docker
# compose (deploy/host/converge.sh). Edit the vault, not this file.
#
# The toolbox fills in @VAULT@ (OP_VAULT, default OpenNova-Deploy), @APP_ITEM@
# (app-prod for prod, app-<deployment> otherwise) and @STRIPE_ITEM@
# (stripe-live for prod, stripe-test otherwise).
#
# A line ending in "# optional" is synced only while its field exists, and
# removed from SSM by `secrets sync --prune` once it does not. The "# billing"
# lines are synced together, and only while the Stripe item is complete and well
# formed (`secrets check`); otherwise billing stays off (the server logs
# "billing off") and none of them is synced.
ADMIN_API_TOKEN=op://@VAULT@/@APP_ITEM@/admin_api_token
ADMIN_BASIC_AUTH_USER=op://@VAULT@/@APP_ITEM@/admin_basic_auth_user
ADMIN_BASIC_AUTH_PASSWORD=op://@VAULT@/@APP_ITEM@/admin_basic_auth_password
ONNET_BOOTSTRAP_ADMIN=op://@VAULT@/@APP_ITEM@/bootstrap_admin # optional
STRIPE_SECRET_KEY=op://@VAULT@/@STRIPE_ITEM@/secret_key # billing
STRIPE_WEBHOOK_SECRET=op://@VAULT@/@STRIPE_ITEM@/webhook_secret # billing
STRIPE_PRICE_TIER1=op://@VAULT@/@STRIPE_ITEM@/price_tier1 # billing
STRIPE_PRICE_TIER2=op://@VAULT@/@STRIPE_ITEM@/price_tier2 # billing
STRIPE_PRICE_TIER3=op://@VAULT@/@STRIPE_ITEM@/price_tier3 # billing
STRIPE_PRICE_OWN_MACHINE=op://@VAULT@/@STRIPE_ITEM@/price_own_machine # billing
