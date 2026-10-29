-- OP: Acid Gambit is its own title (games/acidgambit): its product names it. The protocol it speaks
-- is Joint Operations' (the same object, games/acidgambit/acidgambit_protocol.cpp), so its lobby
-- stays JO's and a game of either title sits in it (lobbies are keyed by protocol:
-- 0008_lobbies_by_protocol.sql, catalog_repository.h lobby_available).
-- Its expansions take its title from it.

UPDATE products SET title = 'acidgambit' WHERE key = 'ACIDGAMBIT' AND kind = 'game';
