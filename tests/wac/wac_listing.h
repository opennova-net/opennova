// The listing scripts/oracles/wac_parity.py writes of the original compiler's
// output, rebuilt from a port Program: one line per word (a pool operand as
// its value or its catalog symbol, a variable by bank, a Text operand as its
// string), then the errors in order, the event depth bytes and the DO count.
// The retail-vector and corpus tests compare programs through it.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <formats/wac/bytecode.h>
#include <formats/wac/command.h>
#include <formats/wac/program.h>

namespace wac_listing {

inline std::string hex(uint32_t word) {
	char text[16];
	std::snprintf(text, sizeof text, "%08X", static_cast<unsigned>(word));
	return text;
}

// The first name of each named-value row's dword, lowercased (Player, Item
// and auto share one). [orig: the rows @0x82EEF0]
inline const char *builtin_name(opennova::wac::Builtin id) {
	using opennova::wac::Builtin;
	switch (id) {
		case Builtin::Result: return "result";
		case Builtin::Ticks: return "ticks";
		case Builtin::GameOver: return "gameover";
		case Builtin::WinVar: return "winvar";
		case Builtin::LoseVar: return "losevar";
		case Builtin::SquadSSN: return "squadssn";
		case Builtin::SquadWho: return "squadwho";
		case Builtin::Night: return "night";
		case Builtin::Seatbelt: return "seatbelt";
		case Builtin::Wind: return "wind";
		case Builtin::Breathtime: return "breathtime";
		case Builtin::Fallmps: return "fallmps";
		case Builtin::AccuracySpread: return "accuracyspread";
		case Builtin::Autogain: return "autogain";
		case Builtin::Health: return "health";
		case Builtin::Mana: return "mana";
		case Builtin::Bluekills: return "bluekills";
		case Builtin::Greenkills: return "greenkills";
		case Builtin::Humans: return "humans";
		case Builtin::RandomResult: return "rnd";
		case Builtin::AutoItem: return "player";
		case Builtin::CurTOD: return "curtod";
		case Builtin::Scratch: return "scratch";
	}
	return "?";
}

inline std::string operand(const opennova::wac::Program &program, size_t at) {
	using namespace opennova::wac;
	for (const OperandSymbol &symbol : program.operand_symbols) {
		if (symbol.word != at) continue;
		std::string text = symbol.symbol;
		for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return text;
	}
	// A word that names none of the operand spans (a POP a ')' emits while
	// parameters are pending, say) is written raw, as the generator writes
	// an address outside the original's operand arrays.
	const uint32_t ref = program.code[at];
	const uint32_t index = operand_index(ref);
	switch (operand_kind(ref)) {
		case OperandKind::Pool:
		case OperandKind::EntitySsn:
			if (index >= 512) break;
			return "#" + std::to_string(index < program.operands.size() ? program.operands[index] : 0);
		case OperandKind::MissionVar:
			if (index >= 512) break;
			return "V" + std::to_string(index);
		case OperandKind::GlobalVar:
			if (index >= 256) break;
			return "G" + std::to_string(index);
		case OperandKind::MusicVar: return "M" + std::to_string(index);
		case OperandKind::EventFired:
			if (index >= 0x400) break;
			return "E" + std::to_string(index);
		case OperandKind::Text:
			if (index >= 0x1010) break;
			return "S'" + program.text_at(index) + "'";
		case OperandKind::Builtin:
			if (index > uint32_t(Builtin::Scratch)) break;
			if (static_cast<Builtin>(index) == Builtin::Scratch) return "SCRATCH";
			return std::string("N:") + builtin_name(static_cast<Builtin>(index));
	}
	return "A" + hex(ref);
}

inline std::string document(const opennova::wac::Program &program) {
	using namespace opennova::wac;
	std::string out;
	const auto line = [&out](const std::string &text) {
		if (!out.empty()) out += '\n';
		out += text;
	};
	size_t end = program.code.size();
	if (end != 0 && program.code[end - 1] == kProgramTerminator) --end; // the loader's word
	for (size_t i = 0; i < end;) {
		const uint32_t word = program.code[i];
		const uint32_t op = (word >> 24) & 0x3F;
		line(hex(word));
		if (op == 0 || (op >= 0x0F && op <= 0x1C)) {
			// A ')' can pop a frame while parameters are pending: that POP
			// word sits between the call and its operands.
			const uint32_t index = word & 0xFFFF;
			int argc = index < uint32_t(wac_command_count()) ? wac_commands()[index].argc : 0;
			for (++i; argc != 0 && i < end; ++i) {
				if ((program.code[i] >> 24) == 7) {
					line(hex(program.code[i]));
				} else {
					line(operand(program, i));
					--argc;
				}
			}
		} else if (op == 8) {
			if (i + 1 < end) line(operand(program, i + 1));
			i += 2;
		} else if (op == 4 || op == 6) {
			if (i + 1 < end) line(hex(program.code[i + 1]));
			i += 2;
		} else {
			++i;
		}
	}
	line("ERRORS");
	for (const Diagnostic &d : program.diagnostics) {
		const std::string file = d.source < program.source_names.size() ? program.source_names[d.source] : std::string();
		line(file + " (" + std::to_string(d.line) + ") " + d.message);
	}
	std::string depths = "EVENTS ";
	for (size_t i = 0; i < program.event_depths.size(); ++i) {
		if (i) depths += ',';
		depths += std::to_string(program.event_depths[i]);
	}
	line(depths);
	line("LOOPS " + std::to_string(program.loop_count));
	return out;
}

// The generator's repeat marker: \x02COUNT\x02TEXT\x03 stands for COUNT copies.
inline std::string expand(const char *source) {
	std::string out;
	for (const char *s = source; *s != '\0';) {
		if (*s != '\x02') {
			out += *s++;
			continue;
		}
		const int count = std::atoi(s + 1);
		const char *text = std::strchr(s + 1, '\x02') + 1;
		const char *stop = std::strchr(text, '\x03');
		for (int i = 0; i < count; ++i) out.append(text, size_t(stop - text));
		s = stop + 1;
	}
	return out;
}

} // namespace wac_listing
