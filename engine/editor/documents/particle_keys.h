#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/model/edit.h>
#include <editor/model/text_document.h>
#include <formats/particle/particle.h>

namespace opennova::editor {

// The particle key panel (ADR 0046 S23 B): a typed aid over a particle file's text, which stays the document
// (DI-14). It lists each block the game's reader reads, its keys as the reader takes them: each key the block
// writes, its value at its span and what the reader takes it as with the range it clamps it to (particle::key_rows,
// cited at the reader), and the keys of the block's kind it lacks, for an Add. A set rewrites that one value's span
// in the text, or puts a key the block lacks on a line of its own before the block's closing brace (in the indent of
// the block's last key and the file's line end), and the text is read again: no stored layout is replayed and
// nothing is written from a model (ADR 0003 untouched).
struct ParticleKeyField {
	std::string key;                       // as written; the row's own spelling for a key the block lacks
	const particle::KeyRow *row = nullptr; // what the reader takes it as; null for a key it keeps as unknown
	bool present = false;                  // the block writes it
	std::string value;                     // as the reader takes it ("" for one the block lacks)
	TextSpan span;                         // where the block writes the value (present ones)
};
struct ParticleKeyBlock {
	particle::BlockKind kind = particle::BlockKind::Particle;
	std::size_t index = 0;                 // among the file's blocks of its kind
	std::string id;                        // its id as read ("" for none)
	std::size_t first_line = 0, last_line = 0;
	std::size_t close_offset = 0;          // its closing brace's line's first byte
	// The keys it writes, in the text's order, then the reader's keys of its kind it lacks (those of no digit
	// pattern: a graphic layer's or a slot's keys are added by their name).
	std::vector<ParticleKeyField> fields;
};

// The blocks of the text as the game's reader reads it, in the text's order; none where it stops in the text.
std::vector<ParticleKeyBlock> particle_key_blocks(const TextDocument &document);
// "[particledef] spark", a block's words in a list.
std::string particle_block_title(const ParticleKeyBlock &block);
// What a key's value is to the reader, in words, with its range where the reader clamps it ("a whole number, which
// the game takes as at least 1"), and the key only the writer writes said so.
std::string particle_key_words(const particle::KeyRow &row);

// Whether the reader takes `value` as the row says, as written: false with why (a number that is none, fewer than
// three bytes, a name the flag table lacks, a whole number past the clamp: "the game reads it as 1"), and for a
// value the text cannot hold (a ';', a line's end).
bool particle_key_value_ok(const particle::KeyRow &row, const std::string &value, std::string &why);
// The edit that sets `key` of `block` to `value` (its row's checks first): its span replaced where the block
// writes it, else its line put before the block's closing brace. False with why for a value the reader would not
// take so, or a key the block's kind does not read.
bool particle_key_edit(const TextDocument &document, const ParticleKeyBlock &block, const std::string &key,
		const std::string &value, Edit &out, std::string &why);

} // namespace opennova::editor
