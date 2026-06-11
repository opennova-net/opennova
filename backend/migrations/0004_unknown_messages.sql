-- Unknown-message tracking. Every inbound signal the server doesn't have a
-- handler for — an unhandled NW-UDP opcode, a non-NOVAWORLDUDP PN, an
-- unrecognized container/protocol message, an unknown gate tag, an HTTP 404 —
-- gets deduped by (channel, signature) into this table by the in-memory
-- UnknownTracker, flushed on the main tick (never per-packet).
--
-- This is the diagnostic backstop for "retail asked for X and we didn't know
-- what it was": the count + first/last seen + a capped first sample tell us
-- what to reverse next. /api/unknowns reads the live in-memory snapshot;
-- this table is the durable cross-run record.

CREATE TABLE IF NOT EXISTS unknown_messages (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    channel       TEXT NOT NULL,            -- gate | nwu | container | ptype | pn | http
    signature     TEXT NOT NULL,            -- e.g. "0x47", "ClientFooRequest", "GET /foo"
    count         INTEGER NOT NULL DEFAULT 0,
    first_seen_ms INTEGER NOT NULL,
    last_seen_ms  INTEGER NOT NULL,
    sample        BLOB,                     -- first sighting only, capped at 512 bytes
    sample_meta   TEXT,                     -- e.g. peer "1.2.3.4:64206"
    UNIQUE(channel, signature)
);
