#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::editor {

// The game's particle files parse through its one ASCII line tokenizer, whose token table is process-wide state, and
// three of the particle parsers read that table past the line they parse: a hazard of the original game (a stock
// Jointops.exe 1.7.5.7 faulted loading a mission over our files, 2026-10-14), which the editor models so a project
// finds it before the game does. The engine's own reader (formats/particle) never reads past a line.
//
// The tokenizer [orig: @ 0x53CB60] copies a line (at most 1000 bytes, @ 0x53CBBB) into a global buffer (@ 0x24E5DF0),
// cuts it at spaces, tabs and commas, keeps a quoted run whole, stops at "//" and at the first ';' (which no token
// holds but the one it is attached to), and records at most 30 token pointers in a global table (the count @ 0x24E61E0,
// the slots from @ 0x24E61E4); each line resets the count and slots 0 to 2 to "" alone, so a slot past a line's tokens
// keeps the pointer an earlier, longer line left there (into the buffer, which each line overwrites), or NULL until
// some line first reached it. The file walk [orig: File_ParseASCIIFile @ 0x53D8F8] cuts lines at CR LF (a last line
// with none loses its last byte, @ 0x53D8E9) and hands every line with a token not starting '/' to the section
// callback. The parsers' next-token helper [orig: CParticleDefEntry_NextToken @ 0x5DFFB0] steps to the next slot with
// no bound: it reads the slot (faulting on NULL, @ 0x5E0000), skips a lone ';' ',' or '=', strips those three from the
// token in place (CParticleDefEntry_StripSeparators @ 0x5DFF00; the set @ 0x5E897C) and, where it stripped a ';', ends the statement: the
// helper answers NULL from then on. A list therefore stops at a ';' attached to its last token, and otherwise reads on
// into the slots past the line:
// - `flags` and `move` take tokens while each is a name of their table [orig: CParticleDef_ParseProperties @
//   0x5EA5B3..0x5EA66A, the flag names @ 0x846A20; @ 0x5EA4B3..0x5EA56F, the move names @ 0x848808]: the game's own
//   writer ends the list " ;" (FlagTable_BuildString's trailing space, @ 0x5DF9C0), so it always reads the slot past
//   the last name;
// - `pdefs` takes tokens until the helper answers NULL [orig: CParticleTableDef_ParseScriptLine @ 0x5E9444..0x5E948B]:
//   a list whose ';' stands apart reads on until a slot holding a ';' or a NULL one;
// - a table's row (a key holding 't') takes up to 8 values [orig: CEffectTableDef_ParseCallback @ 0x5E4130..0x5E416F]:
//   a short row whose ';' stands apart reads past its values.
// A slot read past the 30 reads past the table (the caller's copy of it on the stack).

// A particle file in the order the game parses them, its bytes as the game's loader is served them.
struct ParticleLoadFile {
	std::string path;
	std::vector<uint8_t> bytes;
};

// A read of a token slot that holds NULL (no line parsed before it reached that slot: the game faults), or past the
// table: the file (its index among those given), the line (1-based, as the game's walk counts them), the key whose
// list read it ("flags", "move", "pdefs", or a table row's key), the slot read, and the most tokens a line before it
// held (the slots filled, 0 to that less one).
struct ParticleSlotFault {
	size_t file = 0;
	int line = 0;
	std::string key;
	int slot = 0;
	int filled = 0;
	bool outside = false; // past the table's 30 slots
};

// Every such read across `files`, parsed in order from a table no line has filled yet (the game's own files parsed
// before the particles leave theirs too, which the editor does not know of: what it finds is a fault no file the
// project holds guards against).
std::vector<ParticleSlotFault> particle_token_slot_faults(const std::vector<ParticleLoadFile> &files);

} // namespace opennova::editor
