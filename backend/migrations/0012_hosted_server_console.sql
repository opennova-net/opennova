-- The hosted servers' remote-admin console (ADR 0053 d5, proposed): the
-- service keeps one logged-in session on each running hosted server's retail
-- remote-admin port (apps/novaworld_server/hosted/admin_link.h), and its
-- console is this ring. One row per line:
--
--   command  a command the link sent for an owner, an admin or the service
--            itself (actor names which), as the site asked for it
--   reply    one reply packet the server answered with (a PLAYER LIST table
--            is one row)
--   status   the link's own lines: logged in, the session lost or refused,
--            the server's game state when it changes
--
-- Read-only queries the site polls (the player list, the rotation, the chat
-- window) are not written here. The text is UTF-8 (the server's cp1252
-- decoded); the admin password never reaches it. `at` is UTC. A capped ring:
-- every append keeps a server's newest 500 rows (hosted::append_console).
--
-- Forward-only, like 0005..0011.

CREATE TABLE hosted_server_console (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    server_id  INTEGER NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    kind       TEXT NOT NULL CHECK (kind IN ('command', 'reply', 'status')),
    actor      TEXT NOT NULL DEFAULT '' CHECK (actor IN ('', 'owner', 'admin', 'service')),
    text       TEXT NOT NULL,
    at         TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_hosted_server_console_server ON hosted_server_console(server_id, id);
