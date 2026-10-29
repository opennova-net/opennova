-- OP: Acid Gambit is its own title (games/acidgambit): its product names it. The protocol it speaks
-- is Joint Operations' (the same object, games/acidgambit/acidgambit_protocol.cpp), so its lobby
-- stays JO's and a game of either title sits in it (catalog_repository.h speaks_lobby_protocol).
-- Its expansions take its title from it.

UPDATE products SET title = 'acidgambit' WHERE key = 'ACIDGAMBIT' AND kind = 'game';
