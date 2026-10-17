-- Lobby instances share one endpoint (apps/novaworld_common/README.md "Lobby instances";
-- engine/net/novaworld/lobby_repository.h). Every lobby announces the deployment's one lobby
-- address, the load balancer's (on ECS: the gate's Elastic IP and UDP 64206), which spreads the
-- clients across the lobby tasks, so a stock host whose lobby went away reconnects to that same
-- address and lands on a live one. An instance is its id alone, and its endpoint is data: every
-- start of a lobby registers a row of its own (its previous run's, if it stopped cleanly, went at
-- that stop; else the stale reap takes it with its rows), and no start refuses an endpoint
-- another running lobby announces. Service policy, not a port.
ALTER TABLE lobby_instances DROP CONSTRAINT lobby_instances_endpoint_key;
