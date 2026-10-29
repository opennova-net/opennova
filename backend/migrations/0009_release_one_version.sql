-- The ONE (the OpenNova Engine, CONTEXT.md "ONE") that built a release: the data program's own
-- build, one_build()'s version ("0.0.22"; a staging build's "0.0.22.123"), recorded when its
-- release.build job builds the package from a revision (products/release_build.h). NULL for an
-- uploaded package (an app's build, which the service packs and never builds) and for a release
-- built before the column.
ALTER TABLE releases ADD COLUMN one_version text;
