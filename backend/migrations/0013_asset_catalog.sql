-- Derived native files from one immutable release build, separate from editor sources.
CREATE TABLE asset_snapshots (
    release_id bigint PRIMARY KEY REFERENCES releases(id) ON DELETE CASCADE,
    product_id bigint NOT NULL REFERENCES products(id) ON DELETE CASCADE,
    manifest_sha256 text NOT NULL CHECK (manifest_sha256 ~ '^[0-9a-f]{64}$')
);

CREATE TABLE asset_entries (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    release_id bigint NOT NULL REFERENCES asset_snapshots(release_id) ON DELETE CASCADE,
    name text NOT NULL,
    kind text NOT NULL CHECK (kind IN ('model', 'animation', 'animation_map', 'terrain', 'texture')),
    sha256 text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    size bigint NOT NULL CHECK (size >= 0),
    file_count bigint NOT NULL CHECK (file_count > 0),
    total_bytes bigint NOT NULL CHECK (total_bytes >= size),
    bundle_key text NOT NULL DEFAULT '',
    bundle_error text NOT NULL DEFAULT '',
    UNIQUE (release_id, name)
);
CREATE INDEX asset_entries_kind_id ON asset_entries(kind, id DESC);

CREATE TABLE asset_files (
    asset_id bigint NOT NULL REFERENCES asset_entries(id) ON DELETE CASCADE,
    name text NOT NULL,
    sha256 text NOT NULL CHECK (sha256 ~ '^[0-9a-f]{64}$'),
    size bigint NOT NULL CHECK (size >= 0),
    -- Losing a contributor must hide the entire asset, never make its closure more public.
    product_id bigint REFERENCES products(id) ON DELETE SET NULL,
    PRIMARY KEY (asset_id, name)
);
CREATE INDEX asset_files_product ON asset_files(product_id);

-- Polling must also recognize jobs failed by the reaper, without scanning unrelated jobs.
CREATE INDEX idx_jobs_asset_bundle_history ON jobs(dedupe_key, id DESC)
    WHERE kind = 'asset.bundle';
