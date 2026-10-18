-- Hosting capacity and static addresses (the fleet program, ADR 0066): an admin's
-- comp grants (apps/novaworld_common/billing/billing_repository.h), the hosting
-- entitlement's other source beside the support tiers' subscriptions
-- (0002_billing.sql). Service policy, not a port.

-- An admin's grant of hosted servers ('servers') or own machines ('own_machine') to an
-- account, outside any Stripe subscription: the entitlement's servers are the larger of
-- the servers granted and the support tier's, and its own machines the add-on's plus
-- the ones granted (while it has servers). One row per account and item; no row is 0.
-- It replaces players.comp_servers, whose grants it takes over.
CREATE TABLE comp_grants (
    user_id    bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    item       text NOT NULL CHECK (item IN ('servers', 'own_machine')),
    quantity   bigint NOT NULL CHECK (quantity BETWEEN 1 AND 16),
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, item)
);
INSERT INTO comp_grants (user_id, item, quantity)
    SELECT id, 'servers', comp_servers FROM players WHERE comp_servers > 0;
ALTER TABLE players DROP COLUMN comp_servers;

-- A checkout's hold on the item it sells (billing_repository.h "A checkout's hold"):
-- written with the judgement that the fleet has room for it (hosted/sold_capacity.h),
-- under the hosting-start lock, before Stripe makes the session (checkout_session, ''
-- until then), and counted as a granting subscription of the item until the sync writes
-- the subscription it made (its opennova_reservation metadata names the row), the
-- session expires, the account's next checkout settles it, or expires_at passes.
CREATE TABLE billing_checkout_reservations (
    id               bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    user_id          bigint NOT NULL REFERENCES players(id) ON DELETE CASCADE,
    item             text NOT NULL CHECK (item IN ('tier', 'own_machine', 'static_address')),
    tier             bigint NOT NULL DEFAULT 0 CHECK (tier BETWEEN 0 AND 3),
    checkout_session text NOT NULL DEFAULT '',
    created_at       timestamptz NOT NULL DEFAULT now(),
    expires_at       timestamptz NOT NULL
);
CREATE INDEX idx_billing_checkout_reservations_user ON billing_checkout_reservations(user_id);
CREATE INDEX idx_billing_checkout_reservations_session ON billing_checkout_reservations(checkout_session)
    WHERE checkout_session <> '';

-- The static-address add-on (apps/novaworld_common/hosted/address_repository.h): a
-- subscription's item may be one ('static_address', quantity 1: each address its own
-- subscription).
ALTER TABLE billing_subscriptions
    DROP CONSTRAINT billing_subscriptions_item_check,
    ADD CONSTRAINT billing_subscriptions_item_check
        CHECK (item IN ('tier', 'own_machine', 'static_address', 'unknown'));

-- A static address: an own machine's fixed public IPv4 with an A record named `name`
-- in the deployment's play zone, held by its owner for the own-machine servers it
-- starts. Its source is its own subscription (stripe_subscription_id, one address
-- each) or an admin's grant ('comp'). allocating -> ready -> lapsed (its grant gone, or
-- an admin's release: lapse_cause) -> releasing -> released, or failed (the cloud
-- refused it: reason). allocation_id and public_ip are the cloud's; dns_name the name
-- its record carries now ('' for none), dns_change_id the zone's last change and
-- dns_state whether a delete waits to be in sync; release_after when its record's TTL
-- has drained. A name is unique for good: a released address keeps it until an admin
-- renames the row.
CREATE TABLE hosted_addresses (
    id                     bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    owner_user_id          bigint REFERENCES players(id) ON DELETE SET NULL,
    name                   text NOT NULL,
    source                 text NOT NULL CHECK (source IN ('subscription', 'comp')),
    stripe_subscription_id text,
    state                  text NOT NULL DEFAULT 'allocating'
                           CHECK (state IN ('allocating', 'ready', 'lapsed', 'releasing', 'released', 'failed')),
    allocation_id          text NOT NULL DEFAULT '',
    public_ip              text NOT NULL DEFAULT '',
    dns_name               text NOT NULL DEFAULT '',
    dns_change_id          text NOT NULL DEFAULT '',
    dns_state              text NOT NULL DEFAULT 'none' CHECK (dns_state IN ('none', 'pending', 'insync')),
    release_after          timestamptz,
    lapse_cause            text NOT NULL DEFAULT '' CHECK (lapse_cause IN ('', 'grant', 'admin')),
    reason                 text NOT NULL DEFAULT '',
    created_at             timestamptz NOT NULL DEFAULT now(),
    updated_at             timestamptz NOT NULL DEFAULT now(),
    ready_at               timestamptz,
    lapsed_at              timestamptz,
    released_at            timestamptz,
    CONSTRAINT hosted_addresses_subscription_check CHECK ((source = 'subscription') = (stripe_subscription_id IS NOT NULL))
);
CREATE UNIQUE INDEX idx_hosted_addresses_name ON hosted_addresses(name);
CREATE UNIQUE INDEX idx_hosted_addresses_subscription ON hosted_addresses(stripe_subscription_id)
    WHERE stripe_subscription_id IS NOT NULL;
CREATE INDEX idx_hosted_addresses_owner ON hosted_addresses(owner_user_id);

-- The address a hosted server asked for (an own-machine server's; one live server per
-- address), and the address an own machine was launched to carry.
ALTER TABLE hosted_servers ADD COLUMN address_id bigint REFERENCES hosted_addresses(id) ON DELETE SET NULL;
CREATE UNIQUE INDEX idx_hosted_servers_one_per_address ON hosted_servers(address_id)
    WHERE address_id IS NOT NULL AND state NOT IN ('stopped', 'failed');
ALTER TABLE fleet_machines ADD COLUMN address_id bigint REFERENCES hosted_addresses(id) ON DELETE SET NULL;

-- The addresses the cloud holds for this deployment that no held row names, as the last
-- fleet.sweep found them (reported, never released by it).
CREATE TABLE hosted_address_orphans (
    allocation_id text PRIMARY KEY,
    public_ip     text NOT NULL,
    address_id    bigint NOT NULL DEFAULT 0,
    seen_at       timestamptz NOT NULL DEFAULT now()
);
