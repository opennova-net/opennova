# The Stripe TEST-mode key for the billing end-to-end test (tests/novaworld/
# billing_stripe_e2e_test.cpp), from the vault's test item: `op run --env-file=` resolves
# it into one command's environment, never onto disk or a command line:
#   op run --env-file=deploy/env/billing.e2e.env.tpl -- #     ctest --test-dir build -C Release -R '^billing_stripe_e2e$' --output-on-failure
# Test mode only (the test refuses a live key); the prices are the account's, found by
# the lookup keys `stripe setup` gives them.
ONNET_STRIPE_TEST_SECRET_KEY=op://OpenNova-Deploy/stripe-test/secret_key
