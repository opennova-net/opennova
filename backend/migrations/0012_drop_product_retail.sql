-- The products' retail flag goes. It marked NovaLogic's own games (Joint Operations, Escalation,
-- Delta Force Xtreme 2) for the stock UPDATE.EXE the Switcher sent here: SERVER.LST named
-- ftp.novaworld.com for them, an unpublished one answered current, a release counted its
-- removals from the stock install, and their lobbies were closed to players' own projects. The
-- Switcher is retired (ADR 0075, 0002_players_projects.sql), and every product updates through
-- OpenNova's own update.exe, which dials only the deployment's update host. A lobby the original
-- executables browse is now told by its gate tag (catalog_repository.h selectable_lobbies).
--
-- The owner check named the column; it keeps its other half: an account's own is never an app.

ALTER TABLE products DROP CONSTRAINT products_owner_check;
ALTER TABLE products DROP COLUMN retail;
ALTER TABLE products ADD CONSTRAINT products_owner_check CHECK (owner_id IS NULL OR kind <> 'app');
