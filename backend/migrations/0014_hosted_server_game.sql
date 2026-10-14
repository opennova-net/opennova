-- A hosted server's data set (the hosted titles program): the game folder its
-- machine mounts, the title opennova-serve speaks over it (/game) and the lobby
-- it lists in. hosted_servers.game holds the slug of that lobby's games row:
-- 'jop_2_consumer' (Joint Operations), 'dfx2_consumer' (Delta Force Xtreme 2)
-- or 'opennova_0_consumer' (OpenNova's own data). The service's catalog
-- (apps/novaworld_server/hosted/data_sets.h) says what each one is, and a
-- start names one the deployment offers (hosted_settings.h check_settings).
--
-- No REFERENCES games(slug): the games rows are seeded (backend/seed), not
-- migrated, so a database the seed has not reached has none, and SQLite adds a
-- referencing column only with a NULL default. Every server before this
-- migration ran Joint Operations' data, so the existing rows take
-- 'jop_2_consumer'.
--
-- 0013 is the lobbies' (games.title and the OpenNova row). Forward-only, like
-- 0005..0012: applies exactly once on fresh and existing DBs.

ALTER TABLE hosted_servers ADD COLUMN game TEXT NOT NULL DEFAULT 'jop_2_consumer';
