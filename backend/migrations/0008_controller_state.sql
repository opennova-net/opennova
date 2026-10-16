-- The controller's state in rows (apps/novaworld_server/controller/): what the
-- hosted servers' controller kept in its memory and shared with the routes, so
-- the controller, the web and the workers share it through the database alone.
-- Service policy, not a port.

-- The controller's leadership (controller/leadership.h): one process leads,
-- holding a session advisory lock on a connection of its own; each new leader
-- moves the generation on, and a leader whose generation is no longer the
-- row's runs no pass (a stale one fenced off until it exits).
CREATE TABLE controller_leader (
    id         bigint PRIMARY KEY CHECK (id = 1),
    generation bigint NOT NULL DEFAULT 0,
    holder     text NOT NULL DEFAULT '',
    elected_at timestamptz
);
INSERT INTO controller_leader (id) VALUES (1);

-- The owners' limits' hold (hosted/limits_hold.h), one row: whether it holds and
-- why, the owners it waits on a reconcile for and the reconciles counted when it
-- took them, the last pass's counts for the admin's view, the clean reconciles
-- with Stripe so far (the billing sync's), and an admin's override for the next
-- pass.
CREATE TABLE hosted_limits_hold (
    id               bigint PRIMARY KEY CHECK (id = 1),
    held             boolean NOT NULL DEFAULT false,
    reason           text NOT NULL DEFAULT '',
    owners           bigint[] NOT NULL DEFAULT '{}',
    taken_reconciles bigint NOT NULL DEFAULT 0,
    last_owners      bigint NOT NULL DEFAULT 0,
    servers          bigint NOT NULL DEFAULT 0,
    reconciles       bigint NOT NULL DEFAULT 0,
    force_next       boolean NOT NULL DEFAULT false
);
INSERT INTO hosted_limits_hold (id) VALUES (1);

-- Each running hosted server's admin link as the controller last saw it
-- (hosted/admin_link.h AdminLinkStatus): its state, the server's scene at the
-- last poll, when that poll answered and since when the session is up, why it
-- last failed; seen_at moves while the controller keeps the link, so a row it
-- stopped refreshing (no controller) reads as no link. The web routes read
-- these rows, never a link.
CREATE TABLE hosted_admin_links (
    server_id    bigint PRIMARY KEY REFERENCES hosted_servers(id) ON DELETE CASCADE,
    state        text NOT NULL CHECK (state IN ('connecting', 'up', 'down', 'stopped')),
    game_state   text NOT NULL DEFAULT '',
    last_poll_at timestamptz,
    up_since_at  timestamptz,
    error        text NOT NULL DEFAULT '',
    seen_at      timestamptz NOT NULL DEFAULT now()
);

-- The console routes' commands (hosted/console_queue.h): a web route writes one,
-- the controller runs it on the server's admin link and writes what came back
-- (its outcome, whether it was sent, each reply packet in the server's code
-- page), and the route reads it and deletes it. A command not run by its
-- deadline is not run; a row left past it (its route gone) is deleted by the
-- controller. claimed_generation is the leadership's generation of the
-- controller that claimed it (controller_leader): one still running under an
-- earlier generation lost its controller, and the next leader answers it
-- link_down.
CREATE TABLE hosted_console_commands (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    server_id   bigint NOT NULL REFERENCES hosted_servers(id) ON DELETE CASCADE,
    line        text NOT NULL CHECK (line <> ''),
    actor       text NOT NULL,
    record      boolean NOT NULL DEFAULT false,
    state       text NOT NULL DEFAULT 'queued'
        CHECK (state IN ('queued', 'running', 'done', 'cancelled')),
    outcome     text NOT NULL DEFAULT '',
    sent        boolean NOT NULL DEFAULT false,
    claimed_generation bigint NOT NULL DEFAULT 0,
    deadline_at timestamptz NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now(),
    finished_at timestamptz
);
CREATE INDEX idx_hosted_console_commands_queued ON hosted_console_commands(id) WHERE state = 'queued';
CREATE INDEX idx_hosted_console_commands_deadline ON hosted_console_commands(deadline_at);

CREATE TABLE hosted_console_replies (
    command_id bigint NOT NULL REFERENCES hosted_console_commands(id) ON DELETE CASCADE,
    n          bigint NOT NULL,
    text       text NOT NULL,
    PRIMARY KEY (command_id, n)
);
