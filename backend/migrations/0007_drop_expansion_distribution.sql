-- Retire the expansion distribution pipeline and the launcher's catalogue
-- column (ADR 0048). The OpenNova Launcher installed expansions from these
-- tables and launched the game by games.executable_name; both products are
-- gone, and nothing else reads them. The game's own expansion concept
-- (games.exp_bits, player_game_access.exp_bits) is unaffected.
--
-- Forward-only, like 0005/0006: applies exactly once on fresh and existing DBs.

DROP TABLE IF EXISTS expansion_files;
DROP TABLE IF EXISTS expansion_releases;
DROP TABLE IF EXISTS expansions;

ALTER TABLE games DROP COLUMN executable_name;
