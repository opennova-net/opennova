-- The content store (ADR 0069; apps/novaworld_common/data_sets/content_repository.h): each
-- product's raw content, an OpenNova Editor project's own files, as a linear history of
-- revisions. Every file is stored once by its SHA-256 in the object store
-- (content/<key>/blobs/<sha256>, core/object_store.h), and each revision is a manifest of them
-- (content/<key>/manifests/<sha256>.json, base/content/content_manifest.h). Admins push; a
-- public project is anyone's to pull, a private one (a retail game's) an admin's alone.
-- Service policy, not a port.

-- A content project. key names it in the store and the routes ('JO', 'JOX01');
-- data_set_id the data set a release built from it updates; base_project_id an expansion's
-- base game's project, which a release build exports beside it; retail a stock game's import,
-- whose revision 1 is the stock install imported afresh and whose releases patch the stock
-- install (each the difference from revision 1's export, ADR 0067 d6). head is the newest
-- revision's number, 0 before the first push: a push names it as its parent and moves it on
-- in the same statement (content_repository.h commit_revision).
CREATE TABLE content_projects (
    id              bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    key             text NOT NULL UNIQUE CHECK (key ~ '^[A-Z0-9_]{1,16}$'),
    title           text NOT NULL CHECK (title <> ''),
    visibility      text NOT NULL CHECK (visibility IN ('public', 'private')),
    retail          boolean NOT NULL DEFAULT false,
    data_set_id     bigint UNIQUE REFERENCES data_sets(id) ON DELETE SET NULL,
    base_project_id bigint REFERENCES content_projects(id) ON DELETE RESTRICT,
    head            bigint NOT NULL DEFAULT 0 CHECK (head >= 0),
    created_by      text NOT NULL DEFAULT '',
    created_at      timestamptz NOT NULL DEFAULT now(),
    updated_at      timestamptz NOT NULL DEFAULT now(),
    CHECK (base_project_id IS DISTINCT FROM id)
);

-- One revision of a project: its number (1, 2, ...; its parent is the one before), the admin
-- who pushed it (author_id, kept as created_by once the account is gone), its message, its
-- manifest's SHA-256 and what the manifest lists.
CREATE TABLE content_revisions (
    project_id      bigint NOT NULL REFERENCES content_projects(id) ON DELETE CASCADE,
    number          bigint NOT NULL CHECK (number > 0),
    author_id       bigint REFERENCES players(id) ON DELETE SET NULL,
    created_by      text NOT NULL,
    message         text NOT NULL CHECK (message <> ''),
    manifest_sha256 text NOT NULL CHECK (manifest_sha256 ~ '^[0-9a-f]{64}$'),
    file_count      bigint NOT NULL CHECK (file_count >= 0),
    bytes           bigint NOT NULL CHECK (bytes >= 0),
    created_at      timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, number)
);

-- The files a project's store holds, each once by its SHA-256 (what a push need not upload
-- again, and the project's storage).
CREATE TABLE content_blobs (
    project_id bigint NOT NULL REFERENCES content_projects(id) ON DELETE CASCADE,
    sha256     text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    bytes      bigint NOT NULL CHECK (bytes >= 0),
    created_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (project_id, sha256)
);

-- The products' projects, each on its data set and titled as it is. Our own game and its
-- expansion are public, each keyed by its updater product's login; the retail games are
-- private, found by the command files their stock installs ship (JOPTR.CMD, JOEPTR.CMD,
-- DFX2.CMD). An expansion's project builds on its base game's.
INSERT INTO content_projects (key, title, visibility, retail, data_set_id)
SELECT updater_login, display_name, 'public', false, id FROM data_sets
WHERE kind = 'base' AND updater_login <> '' AND NOT retail_updater;
INSERT INTO content_projects (key, title, visibility, retail, data_set_id)
SELECT 'JO', display_name, 'private', true, id FROM data_sets
WHERE kind = 'base' AND command_file = 'JOPTR.CMD';
INSERT INTO content_projects (key, title, visibility, retail, data_set_id)
SELECT 'DFX2', display_name, 'private', true, id FROM data_sets
WHERE kind = 'base' AND command_file = 'DFX2.CMD';
INSERT INTO content_projects (key, title, visibility, retail, data_set_id, base_project_id)
SELECT d.updater_login, d.display_name, 'public', false, d.id, p.id
FROM data_sets d JOIN content_projects p ON p.data_set_id = d.parent_id
WHERE d.kind = 'expansion' AND d.updater_login <> '' AND NOT d.retail_updater;
INSERT INTO content_projects (key, title, visibility, retail, data_set_id, base_project_id)
SELECT 'JOX01', d.display_name, 'private', true, d.id, p.id
FROM data_sets d JOIN content_projects p ON p.data_set_id = d.parent_id
WHERE d.kind = 'expansion' AND d.command_file = 'JOEPTR.CMD';

-- A release built from a content project's revision (ADR 0069 d7; data_sets/release_build.h): the
-- data program's release.build job exports the revision (an expansion's beside its base's
-- revision), packs it whole (our own game's) or as what changed since the stock revision's export
-- (a retail game's), and hands the package to release.validate as an uploaded one is. building ->
-- validating, or invalid with `error`; build_log what the tools said. content_revision and
-- base_revision are the revisions built, version the package's.
ALTER TABLE releases
    DROP CONSTRAINT releases_status_check,
    ADD CONSTRAINT releases_status_check CHECK (status IN
        ('uploading', 'building', 'validating', 'invalid', 'draft', 'published', 'retired')),
    ADD COLUMN content_project_id bigint REFERENCES content_projects(id) ON DELETE SET NULL,
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
WHERE kind = 'base' AND updater_login <> '' AND NOT retail_updater;
UPDATE data_sets SET product_name = display_name
WHERE kind = 'expansion' AND updater_login <> '' AND NOT retail_updater;
