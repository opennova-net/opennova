-- Sharing native assets in the public library is independent of access to the project's
-- sources. Existing and new projects opt in explicitly, including projects already public.
-- Apps have no project or asset library.
ALTER TABLE products
    ADD COLUMN assets_public boolean NOT NULL DEFAULT false,
    ADD CONSTRAINT products_assets_public_check CHECK (kind <> 'app' OR NOT assets_public);
