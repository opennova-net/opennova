-- An expansion's version.txt is NovaWorld's: a release build writes it beside the
-- expansion's gt.ssc, one line ending in CRLF, from the product's row: version_file when set,
-- else "<display name>, V<the release's version>; English". A project never holds one.
--
-- Escalation keeps retail's line, so our build of it joins a retail Escalation host: the host
-- compares the CRC-32/MPEG-2 of its expansion\jox01\version.txt with the joiner's (DPC 48,
-- vfs_expansion_version_checksum). The line is the retail install's (docs/update/updater-re.md
-- §2: "Joint Operations: Escalation, V1.3.0.0; English"). Every other product's is NULL.
ALTER TABLE products ADD COLUMN version_file text NULL;
UPDATE products SET version_file = 'Joint Operations: Escalation, V1.3.0.0; English'
WHERE key = 'JOX01';
