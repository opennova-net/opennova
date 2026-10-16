# 1Password-referenced Stripe TEST-mode settings for the local stack
# (apps/novaworld_common/README.md "Billing (monthly support tiers)"). `op run
# --env-file=` (or `op inject`) resolves these references into the shell's
# environment for one command, and deploy/compose/docker-compose.yml passes them
# to the programs; nothing secret is committed or written to disk.
#
# The local stack never holds a deployment's secrets: the key is stripe-test's
# restricted_key (the server's permissions alone, the key CI uses too), never
# the mode's secret_key, which is staging's, and there is no webhook secret
# here (staging's endpoint's would let a local stack sign staging's webhooks):
# `stripe listen --forward-to localhost:8080/api/billing/webhook` prints the
# local one, which goes in STRIPE_WEBHOOK_SECRET by hand. Staging's database
# ignores the customers the local stack makes (each database stamps its own
# instance id on them). The live item is prod's alone.
STRIPE_SECRET_KEY=op://OpenNova-Deploy/stripe-test/restricted_key
STRIPE_PRICE_TIER1=op://OpenNova-Deploy/stripe-test/price_tier1
STRIPE_PRICE_TIER2=op://OpenNova-Deploy/stripe-test/price_tier2
STRIPE_PRICE_TIER3=op://OpenNova-Deploy/stripe-test/price_tier3
STRIPE_PRICE_OWN_MACHINE=op://OpenNova-Deploy/stripe-test/price_own_machine
