-- Billing: the monthly support tiers (Stripe subscriptions that support the
-- project and set an account's hosting entitlement), as the service last read
-- them from Stripe (apps/novaworld_server/billing/). Stripe is the record; these
-- tables are its cache, which the entitlement reads whether or not billing is
-- configured, so a deploy that breaks billing never stops a payer's servers.
-- The dialect is 0001_baseline.sql's.

-- The service's own facts, one row per key. instance_id names this database to
-- Stripe: every customer and subscription the service makes carries it in its
-- metadata (opennova_instance), so a reset, or a developer's local database on
-- a shared Stripe test account, never adopts another instance's subscription.
-- The service mints it at its first boot on the database
-- (billing::instance_id, 32 hex digits from the OS CSPRNG).
CREATE TABLE service_meta (
    key   text PRIMARY KEY,
    value text NOT NULL
);

-- An account's Stripe customer: one per account, made at its first checkout
-- (or relinked by an admin), in the key's mode (livemode).
-- open_checkout_session is the Checkout session last made for it ('' for
-- none), which the next checkout expires first. synced_at is the worker's last
-- full read of the customer.
CREATE TABLE billing_customers (
    user_id               bigint PRIMARY KEY REFERENCES players(id) ON DELETE CASCADE,
    stripe_customer_id    text NOT NULL UNIQUE,
    livemode              boolean NOT NULL,
    open_checkout_session text NOT NULL DEFAULT '',
    created_at            timestamptz NOT NULL DEFAULT now(),
    synced_at             timestamptz
);

-- Every subscription of a customer the service knows, as Stripe last answered
-- it. item is what its price sells: a support tier ('tier', with its tier
-- 1..3), the own-machine add-on ('own_machine', quantity = how many machines),
-- or neither ('unknown': a price the configuration no longer names, which
-- grants nothing). status is Stripe's (active, trialing, past_due, canceled,
-- unpaid, incomplete, incomplete_expired, paused); status_since is when the
-- service first read the current one. user_id goes NULL when the account goes,
-- and the subscription row stays (an admin sees it, Stripe still bills it).
CREATE TABLE billing_subscriptions (
    stripe_subscription_id text PRIMARY KEY,
    stripe_customer_id     text NOT NULL,
    user_id                bigint REFERENCES players(id) ON DELETE SET NULL,
    livemode               boolean NOT NULL,
    status                 text NOT NULL,
    item                   text NOT NULL CHECK (item IN ('tier', 'own_machine', 'unknown')),
    price_id               text NOT NULL,
    tier                   bigint NOT NULL DEFAULT 0 CHECK (tier BETWEEN 0 AND 3),
    quantity               bigint NOT NULL DEFAULT 1,
    current_period_end     timestamptz,
    cancel_at_period_end   boolean NOT NULL DEFAULT false,
    status_since           timestamptz NOT NULL DEFAULT now(),
    updated_at             timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_billing_subscriptions_user ON billing_subscriptions(user_id);
CREATE INDEX idx_billing_subscriptions_customer ON billing_subscriptions(stripe_customer_id);

-- Every webhook event Stripe delivered (its id once: a redelivery is a no-op),
-- with the customer it names. The worker syncs each customer with an event
-- not yet processed (processed_at NULL), reading the customer and its
-- subscriptions back from Stripe (an event's body is never trusted), then
-- marks its events with the outcome. Processed events go after 30 days.
CREATE TABLE billing_events (
    event_id       text PRIMARY KEY,
    type           text NOT NULL,
    livemode       boolean NOT NULL,
    customer_id    text NOT NULL DEFAULT '',
    stripe_created timestamptz,
    received_at    timestamptz NOT NULL DEFAULT now(),
    processed_at   timestamptz,
    outcome        text NOT NULL DEFAULT ''
);
CREATE INDEX idx_billing_events_pending ON billing_events(received_at) WHERE processed_at IS NULL;
