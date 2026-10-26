-- The content store (ADR 0069; apps/novaworld_common/data_sets/content_repository.h): each
-- game's project, an OpenNova Editor project's own files, as a linear history of revisions.
-- Every file is stored once by its SHA-256 in the object store (content/<key>/blobs/<sha256>,
-- core/object_store.h), and each revision is a manifest of them
-- (content/<key>/manifests/<sha256>.json, base/content/content_manifest.h). Admins push; a
-- public project is anyone's to pull, a private one (a retail game's) an admin's alone.
-- Service policy, not a port.

-- A game's project, on its data_sets row (0011): key names it in the store and the routes
-- ('JO', 'JOX01'; NULL for a game with no project, OpenNova's runtime, whose releases arrive
-- from CI), visibility who may read it, head the newest revision's number (0 before the first
-- push: a push names it as its parent and moves it on in the same statement,
-- content_repository.h commit_revision). An expansion's base game is its parent, whose project
-- a release build exports beside it; a retail game's (0013's retail) revision 1 is the stock
-- install imported afresh, what its releases count their removals from (ADR 0067 d6).
ALTER TABLE data_sets
    ADD COLUMN key        text UNIQUE CHECK (key ~ '^[A-Z0-9_]{1,16}$'),
    ADD COLUMN visibility text CHECK (visibility IN ('public', 'private')),
    ADD COLUMN head       bigint NOT NULL DEFAULT 0 CHECK (head >= 0),
    ADD CONSTRAINT data_sets_project_check CHECK ((key IS NULL) = (visibility IS NULL)),
    ADD CONSTRAINT data_sets_update_project_check CHECK (kind <> 'update' OR key IS NULL);

-- One revision of a game's project: its number (1, 2, ...; its parent is the one before), the
-- admin who pushed it (author_id, kept as created_by once the account is gone), its message,
-- its manifest's SHA-256 and what the manifest lists.
CREATE TABLE content_revisions (
    data_set_id     bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    number          bigint NOT NULL CHECK (number > 0),
    author_id       bigint REFERENCES players(id) ON DELETE SET NULL,
    created_by      text NOT NULL,
    message         text NOT NULL CHECK (message <> ''),
    manifest_sha256 text NOT NULL CHECK (manifest_sha256 ~ '^[0-9a-f]{64}$'),
    file_count      bigint NOT NULL CHECK (file_count >= 0),
    bytes           bigint NOT NULL CHECK (bytes >= 0),
    created_at      timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (data_set_id, number)
);

-- The files a project's store holds, each once by its SHA-256 (what a push need not upload
-- again, and the project's storage).
CREATE TABLE content_blobs (
    data_set_id bigint NOT NULL REFERENCES data_sets(id) ON DELETE CASCADE,
    sha256      text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    bytes       bigint NOT NULL CHECK (bytes >= 0),
    created_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (data_set_id, sha256)
);

-- Every seeded game's project. Our own game and its expansion are public, each keyed by its
-- updater product's login (ACIDGAMBIT, NIFTY); the retail games are private, keyed JO, JOX01
-- and DFX2 and found by the command files their stock installs ship (JOPTR.CMD, JOEPTR.CMD,
-- DFX2.CMD). A login that is no project key leaves its project for an admin to give it.
UPDATE data_sets SET key = updater_login, visibility = 'public'
WHERE kind IN ('base', 'expansion') AND updater_login ~ '^[A-Z0-9_]{1,16}$' AND NOT retail;
UPDATE data_sets SET key = 'JO', visibility = 'private' WHERE kind = 'base' AND command_file = 'JOPTR.CMD';
UPDATE data_sets SET key = 'JOX01', visibility = 'private'
WHERE kind = 'expansion' AND command_file = 'JOEPTR.CMD';
UPDATE data_sets SET key = 'DFX2', visibility = 'private' WHERE kind = 'base' AND command_file = 'DFX2.CMD';

-- A release built from a revision of its game's project (ADR 0069 d7; data_sets/release_build.h):
-- the data program's release.build job exports the revision (an expansion's beside its base
-- game's revision), packs it whole (a retail game's carrying its removals since the stock
-- revision's export), and hands the package to release.validate as an uploaded one is. building
-- -> validating, or invalid with `error`; build_log what the tools said. content_revision and
-- base_revision are the revisions built, version the package's; 0013's data_set_id the game.
ALTER TABLE releases
    DROP CONSTRAINT releases_status_check,
    ADD CONSTRAINT releases_status_check CHECK (status IN
        ('uploading', 'building', 'validating', 'invalid', 'draft', 'published', 'retired')),
    ADD COLUMN content_revision   bigint,
    ADD COLUMN base_revision      bigint,
    ADD COLUMN version            text NOT NULL DEFAULT '',
    ADD COLUMN build_log          text NOT NULL DEFAULT '';

-- A data set's updater product as its packages name it (the release build's): the command
-- file's PRODUCT and the executable its DONE
-- runs ('' for an expansion): the retail games' by their logins.
ALTER TABLE data_sets
    ADD COLUMN product_name text NOT NULL DEFAULT '',
    ADD COLUMN product_exe  text NOT NULL DEFAULT '';
UPDATE data_sets SET product_name = 'Joint Operations', product_exe = 'Jointops.exe'
WHERE updater_login = 'JOTRus';
UPDATE data_sets SET product_name = 'Joint Operations: Escalation' WHERE updater_login = 'JOEus';
UPDATE data_sets SET product_name = 'Delta Force Xtreme 2', product_exe = 'dfx2.exe'
WHERE updater_login = 'DFX2';
-- Our own game and its expansion are named as their data sets are (whatever their logins), the
-- game run by the runtime's executable.
UPDATE data_sets SET product_name = display_name, product_exe = 'opennova.exe'
WHERE kind = 'base' AND updater_login <> '' AND NOT retail;
UPDATE data_sets SET product_name = display_name
WHERE kind = 'expansion' AND updater_login <> '' AND NOT retail;
