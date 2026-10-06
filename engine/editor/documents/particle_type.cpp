#include "particle_type.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#include <base/io/strutil.h>
#include <editor/documents/text_types.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

// Both listed: the game runs a mission over a particle file it does not read (its effects missing, a
// name of one interned as the stock effect) and over one that defines an effect twice.
constexpr FindingCodeEntry<ParticleFinding> kFindingEntries[] = {
	{ ParticleFinding::Unreadable, listed_code("particle.unreadable") },
	{ ParticleFinding::DuplicateEffect, listed_code("particle.duplicate_effect") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(ParticleFinding::kCount),
		"every ParticleFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the particle type's rows follow ParticleFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Particles);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

// A finding of the type's at a place the reader names (the text type's text_finding_at, DI-06).
Diagnostic particle_finding(ParticleFinding code, std::string message, const TextDocument &document, int line,
		int column) {
	return text_finding_at(finding_code(code), DiagnosticSeverity::Warning, std::move(message), document,
			size_t(std::max(line, 0)), size_t(std::max(column, 0)));
}

} // namespace

std::unique_ptr<DocumentBase> make_particle_document() {
	// The file is its text (a stored SCR form unwrapped by the document's load, as the game's text
	// reader takes either [orig: File_ParseASCIIFile @ 0x53D860]); the reader ends a line at its LF.
	return std::make_unique<TextDocument>();
}

bool read_particle_text(const TextDocument &document, particle::ParticleFile &out, particle::ParseError &error) {
	const std::string &text = document.text();
	return particle::load_particles_from_buffer(text.data(), text.size(), out, error);
}

size_t particle_effect_at(const particle::ParticleFile &file, size_t line) {
	for (size_t i = 0; i < file.effects.size(); ++i) {
		const particle::EffectDef &effect = file.effects[i];
		if (effect.first_line > 0 && line >= size_t(effect.first_line) &&
				(effect.last_line <= 0 || line <= size_t(effect.last_line)))
			return i;
	}
	return std::string::npos;
}

std::vector<Diagnostic> validate_particle_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	particle::ParticleFile file;
	particle::ParseError error;
	if (!read_particle_text(*text, file, error)) {
		// The reader returns at the first line it cannot take, and the effect system keeps nothing of the
		// file (the port's line walk, D-PTL-32 [orig: CEffectWorld_ParseSectionCallback @ 0x5ECB40]).
		findings.push_back(particle_finding(ParticleFinding::Unreadable,
				"The game's particle reader stops here: " + reader_sentence(error.message) +
						" None of the file's effects, particles or tables load, a name only this file defines "
						"spawns the stock effect, and the editor cannot check what the file names.",
				*text, error.line, error.column));
		return findings;
	}
	// An effect defined again in the file: the effect system's by-name walk returns the first it holds,
	// without case [orig: CEffectWorld_FindEffectDefByName @ 0x5E34F0, _stricmp @ 0x5E352C].
	std::map<std::string, const particle::EffectDef *> first;
	for (const particle::EffectDef &effect : file.effects) {
		if (effect.id.empty()) continue;
		const auto [at, fresh] = first.emplace(strutil::to_lower(effect.id), &effect);
		if (fresh) continue;
		findings.push_back(particle_finding(ParticleFinding::DuplicateEffect,
				"The effect " + effect.id + " is defined again here: the game spawns the first, on line " +
						std::to_string(at->second->id_line) + ", and never reads this one.",
				*text, effect.id_line, effect.id_column));
	}
	return findings;
}

bool define_particle_effect(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const TextDocument *text = text_of(document);
	if (!text || missing.kind != ReferenceKind::Particle || missing.target.empty()) return false;
	// A file the reader stops in loads nothing: an effect added to it would not load either.
	particle::ParticleFile read;
	particle::ParseError error;
	if (!read_particle_text(*text, read, error)) return false;
	// The block the engine's own writer makes of an effect of that id and nothing else.
	particle::ParticleFile made;
	particle::EffectDef effect;
	effect.id = missing.target;
	made.effects.push_back(std::move(effect));
	std::ostringstream written;
	std::string write_error;
	if (!particle::save_particles(written, made, write_error)) return false;
	std::string block = written.str();
	// In the file's line ends: CR LF where its lines end so.
	if (text->text().find("\r\n") != std::string::npos) {
		std::string crlf;
		for (const char c : block) {
			if (c == '\n') crlf += '\r';
			crlf += c;
		}
		block = std::move(crlf);
	}
	// After the file's last effect, where the writer puts every effect (before its particles and tables), else
	// at its end.
	size_t offset = text->text().size();
	size_t after = 0;
	for (const particle::EffectDef &each : read.effects) after = std::max(after, size_t(std::max(each.last_line, 0)));
	if (after && !text->offset_of(after + 1, 1, offset)) offset = text->text().size();
	out = PlannedFix();
	out.edits.push_back(TextDocument::replace(text->span_at(offset, 0), block));
	const std::string file = basename_of(document.path());
	out.label = "Add effect " + missing.target + " to " + file;
	out.detail = "Writes an effect block of id " + missing.target + " into " + file + " " +
	             (after ? "after its last effect" : "at its end") + ", as the game's own effect writer writes one: its "
	             "id alone, no particles yet. The game then spawns it by that name (nothing until its particles are "
	             "named) where it now spawns the stock effect.";
	return true;
}

const FindingCodeRow &finding_code(ParticleFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable particle_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
