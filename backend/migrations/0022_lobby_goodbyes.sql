-- The goodbyes a lobby instance's clean stop leaves for the sessions it held
-- (apps/novaworld_common/README.md "Lobby instances";
-- engine/net/novaworld/lobby_repository.h). A NAPI server that stops sends
-- each of its connections the ServerGoodBye burst, and a stock client that gets
-- one reconnects at once. On ECS the load balancer has ended a lobby task's
-- flows by the time ECS signals its stop, so a burst from the stopping task no
-- longer reaches its sessions through the lobby address: each session's next
-- packet reaches another lobby instance, which holds no session for it, and a
-- stock client left unanswered drops its session only after 240 s of receive
-- silence. So the stopping lobby leaves a row per session it held: its address
-- (peer_ip, peer_port), the key its packets address (server_sk, the SK the
-- lobby minted, every SESSION packet's session id) and the key a goodbye to it
-- bears (client_ck, the client's CK). The lobby that a packet from that
-- address carrying that SK reaches sends the goodbye in the stopped one's
-- place. A row is good for that 240 s (past it the client has reconnected by
-- itself); each stop's write deletes the older ones, and a later stop's row at
-- the same address replaces the earlier. Service policy, not a port.
CREATE TABLE lobby_goodbyes (
    peer_ip   text NOT NULL,
    peer_port bigint NOT NULL CHECK (peer_port BETWEEN 0 AND 65535),
    server_sk bigint NOT NULL CHECK (server_sk BETWEEN 1 AND 4294967295),
    client_ck bigint NOT NULL CHECK (client_ck BETWEEN 0 AND 4294967295),
    left_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (peer_ip, peer_port)
);
