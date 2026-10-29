-- Lobbies keyed by protocol (CONTEXT.md "Protocol"): a games row is a lobby, and what its clients
-- share is the in-match protocol they speak, not the title a game runs as (OP: Acid Gambit's title
-- speaks JO's protocol). games.title already held each row's protocol code ('jo', 'dfx2': the
-- codes engine/net/protocol's rows carry), so the column takes the name and keeps its values; a
-- row a title code names takes the code of the protocol that title speaks (OP: Acid Gambit's
-- speaks JO's). The version pair (VER1 / VER2) is the protocol's (Protocol::ver1 / ver2), read
-- from the protocol's row, no longer copied into every lobby.
ALTER TABLE games RENAME COLUMN title TO protocol;
UPDATE games SET protocol = 'jo' WHERE protocol = 'acidgambit';
ALTER TABLE games DROP COLUMN ver1, DROP COLUMN ver2;
