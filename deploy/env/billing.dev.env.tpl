# 1Password-referenced Stripe TEST-mode settings for the dev compose stack
# (apps/novaworld_server/README.md "Billing (monthly support tiers)"). `op run
# --env-file=` (or `op inject`) resolves these references into the shell's
# environment for one command, and deploy/compose/docker-compose.yml passes them
# to the server; nothing secret is committed or written to disk. Test mode only:
# the live item is prod's, synced to SSM by the deploy toolbox.
STRIPE_SECRET_KEY=op://OpenNova-Deploy/stripe-test/secret_key
STRIPE_WEBHOOK_SECRET=op://OpenNova-Deploy/stripe-test/webhook_secret
STRIPE_PRICE_TIER1=op://OpenNova-Deploy/stripe-test/price_tier1
STRIPE_PRICE_TIER2=op://OpenNova-Deploy/stripe-test/price_tier2
STRIPE_PRICE_TIER3=op://OpenNova-Deploy/stripe-test/price_tier3
STRIPE_PRICE_OWN_MACHINE=op://OpenNova-Deploy/stripe-test/price_own_machine
