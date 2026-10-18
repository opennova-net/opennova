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
    item             text NOT NULL CHECK (item IN ('tier', 'own_machine')),
    tier             bigint NOT NULL DEFAULT 0 CHECK (tier BETWEEN 0 AND 3),
    checkout_session text NOT NULL DEFAULT '',
    created_at       timestamptz NOT NULL DEFAULT now(),
    expires_at       timestamptz NOT NULL
);
CREATE INDEX idx_billing_checkout_reservations_user ON billing_checkout_reservations(user_id);
CREATE INDEX idx_billing_checkout_reservations_session ON billing_checkout_reservations(checkout_session)
    WHERE checkout_session <> '';

