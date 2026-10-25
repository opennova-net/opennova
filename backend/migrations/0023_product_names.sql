-- The names players read for OpenNova's own game and its expansion: the base game is
-- "OP: Acid Gambit" and its onjo1 expansion "OP: Nifty Package". The games seed
-- (backend/seed/0001_games.sql) inserts its rows after the migrations and never updates
-- one, so it names the lobby for a new database; this renames the lobby of a database
-- seeded before, and the two data sets 0011 and 0013 inserted.
UPDATE games SET display_name = 'OP: Acid Gambit'
WHERE slug = 'opennova_0_consumer' AND display_name = 'OpenNova';
UPDATE data_sets SET display_name = 'OP: Acid Gambit'
WHERE kind = 'base' AND game = 'opennova_0_consumer' AND display_name = 'OpenNova';
UPDATE data_sets d SET display_name = 'OP: Nifty Package'
FROM data_sets b
WHERE d.kind = 'expansion' AND d.expansion_code = 'onjo1' AND d.parent_id = b.id
  AND b.kind = 'base' AND b.game = 'opennova_0_consumer'
  AND d.display_name = 'OpenNova Joint Operations';
