-- Dedicated machines as their own product (apps/novaworld_common/billing/entitlement.h):
-- a dedicated machine (comp_grants' 'own_machine') no longer moves one of the account's
-- servers off the shared machines, it adds one on a machine of its own, and a grant of
-- 'servers' is of servers on shared machines alone. Each admin's grant keeps the servers
-- it gave: its servers past its dedicated machines stay on shared machines (11 servers
-- with 2 own machines become 9 shared servers and 2 dedicated machines), and a grant of
-- servers its dedicated machines cover goes (every one of them ran on one). Service
-- policy, not a port.

-- The grants that would come to 0 first: comp_grants holds none (its quantity CHECK).
DELETE FROM comp_grants s USING comp_grants o WHERE s.user_id = o.user_id
    AND s.item = 'servers' AND o.item = 'own_machine' AND o.quantity >= s.quantity;
UPDATE comp_grants s SET quantity = s.quantity - o.quantity, updated_at = now()
    FROM comp_grants o WHERE s.user_id = o.user_id AND s.item = 'servers' AND o.item = 'own_machine';
