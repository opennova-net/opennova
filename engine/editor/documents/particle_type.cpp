#include "particle_type.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include <base/io/strutil.h>
#include <base/resource_index/resource_index.h>
#include <base/vfs/file_stamps.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/particle_keys.h>
#include <editor/documents/project_check.h>
#include <editor/documents/text_types.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

namespace {

// All listed: the game runs a mission over a particle file that defines an effect twice, and over names inside
// the files it does not resolve (an effect that spawns nothing, a channel drawn without its curve).
constexpr FindingCodeEntry<ParticleFinding> kFindingEntries[] = {
	{ ParticleFinding::DuplicateEffect, listed_code("particle.duplicate_effect") },
	{ ParticleFinding::EffectUnresolved, listed_code("particle.effect_unresolved", FindingFix::EditRecord) },
	{ ParticleFinding::CurveUnresolved, listed_code("particle.curve_unresolved") },
	// Gates: the game faults as it loads the effects at a mission's start.
	{ ParticleFinding::UnfilledTokenSlot,
	  game_fails_code("particle.unfilled_token_slot",
	                  "the game's particle parser reads a token slot of its line tokenizer that no line has filled yet "
	                  "and faults as the mission loads [orig: CParticleDefEntry_NextToken @ 0x5E0000; the tokenizer @ "
	                  "0x53CB60]") },
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

// The text in the file's line ends: the writer's CR LF, an LF alone where the file's lines end so (the line-ends rule
// says what the game reads of such a file, documents/line_ends.h).
std::string in_line_ends(std::string written, const std::string &file) {
	if (file.empty() || file.find("\r\n") != std::string::npos) return written;
	written.erase(std::remove(written.begin(), written.end(), '\r'), written.end());
	return written;
}

} // namespace

std::unique_ptr<DocumentBase> make_particle_document() {
	// The file is its text (a stored SCR form unwrapped by the document's load, as the game's text
	// reader takes either [orig: File_ParseASCIIFile @ 0x53D860]); the reader ends a line at a CR LF
	// alone (documents/line_ends.h says what it makes of an LF).
	return std::make_unique<TextDocument>();
}

bool read_particle_text(const TextDocument &document, particle::ParticleFile &out, particle::ParseError &error) {
	const std::string &text = document.text();
	return particle::load_particles_from_buffer(text.data(), text.size(), out, error);
}

std::vector<Diagnostic> validate_particle_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const TextDocument *text = text_of(document);
	if (!text) return findings;
	particle::ParticleFile file;
	particle::ParseError error;
	read_particle_text(*text, file, error);
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
	particle::ParticleFile read;
	particle::ParseError error;
	read_particle_text(*text, read, error);
	// The block the engine's own writer makes of an effect of that id and a pdefs line of no particle: the game's
	// reader makes an effect only at a pdefs line [orig: CParticleTableDef_ParseScriptLine @ 0x5e93b0..0x5e9430].
	particle::ParticleFile made;
	particle::EffectDef effect;
	effect.id = missing.target;
	effect.pdefs.push_back(std::string());
	made.effects.push_back(std::move(effect));
	std::ostringstream written;
	std::string write_error;
	if (!particle::save_particles(written, made, write_error)) return false;
	const std::string block = in_line_ends(written.str(), text->text());
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
	             "id and a pdefs line of no particle yet. The game then holds an effect of that name, which spawns "
	             "nothing until its particles are named, where it now spawns the stock effect.";
	return true;
}

// --- the project check ------------------------------------------------------------------------------

namespace {

// A particle file the catalog loads, as the reader read it, with the places of its keys.
struct LoadedParticles {
	std::string path;
	std::string text;
	particle::ParticleFile file;
	particle::ParticlePlaces places;
};

// "Add particle X to <file>" (particle.effect_unresolved's first fix): the [particledef] block the engine's own writer
// makes of the particle the game's reader holds of a block writing its id alone (its id line resets the definition
// to the game's defaults [orig: CParticleSystemDef_InitDefaults @ 0x5E14F0]; the writer CParticleDef_SaveToFile @
// 0x5E4D70, formats/particle save_particles), after the file's last particle, where the writer puts every particle,
// else at its end; false for a name the reader does not read whole as an id.
bool add_particle_fix(const LoadedParticles &read, const std::vector<size_t> &lines, const std::string &id,
		PlannedFix &out) {
	particle::ParticleFile alone;
	particle::ParseError error;
	const std::string block = "[particledef]\r\n{\r\n\tid = " + id + ";\r\n}\r\n";
	if (!particle::load_particles_from_buffer(block.data(), block.size(), alone, error) || alone.particles.size() != 1 ||
	    alone.particles.front().id != id)
		return false;
	particle::ParticleFile made;
	made.particles.push_back(alone.particles.front());
	std::ostringstream written;
	std::string write_error;
	if (!particle::save_particles(written, made, write_error)) return false;
	size_t offset = read.text.size();
	const particle::BlockPlace *last = nullptr;
	for (const particle::BlockPlace &place : read.places.blocks)
		if (place.kind == particle::BlockKind::Particle) last = &place;
	if (last) {
		const size_t lf = read.text.find('\n', last->close_offset);
		offset = lf == std::string::npos ? read.text.size() : lf + 1;
	}
	const std::string file = basename_of(read.path);
	out = PlannedFix();
	out.edits.push_back(TextDocument::replace(text_span_at(lines, offset, 0), in_line_ends(written.str(), read.text)));
	out.label = "Add particle " + id + " to " + file;
	out.detail = "Writes a particle block of id " + id + " into " + file + " " +
	             (last ? "after its last particle" : "at its end") + ", as the game's own particle writer writes one at "
	             "the game's defaults (a graphic of no texture yet): the game then holds a particle of that name, which "
	             "the effect's pdefs finds.";
	return true;
}

// "Take X out of pdefs" (particle.effect_unresolved's second fix): the member, the `nth` word of the pdefs line `place`
// writes (`words` its members of a name, in order), taken out with the separator before the next member, or after the
// one before it for the last; offered only where another member stays, so the effect still names a particle. False
// where the member is not at its place (a word the reader takes otherwise) or is the effect's one member.
bool take_member_fix(const LoadedParticles &read, const std::vector<size_t> &lines, const particle::KeyPlace &place,
		const std::vector<std::string> &words, size_t nth, const std::string &effect, PlannedFix &out) {
	if (words.size() < 2 || nth >= words.size()) return false;
	size_t at = 0, next = 0, before = 0;
	if (!particle_value_name(read.text, place.offset, place.length, nth, words[nth], at)) return false;
	size_t from = at, to = at + words[nth].size();
	if (nth + 1 < words.size()) {
		if (!particle_value_name(read.text, place.offset, place.length, nth + 1, words[nth + 1], next)) return false;
		to = next;
	} else {
		if (!particle_value_name(read.text, place.offset, place.length, nth - 1, words[nth - 1], before)) return false;
		from = before + words[nth - 1].size();
	}
	out = PlannedFix();
	out.edits.push_back(TextDocument::replace(text_span_at(lines, from, to - from), std::string()));
	out.label = "Take " + words[nth] + " out of pdefs";
	out.detail = "Takes " + words[nth] + " out of the pdefs line of " + effect + ", which goes on naming its other "
	             "particles: the game resolves an effect's particles all or none, so with the others defined it spawns "
	             "them.";
	return true;
}

class ParticleProjectCheck final : public ProjectCheck {
public:
	bool update(const ProjectCheckInput &input) override {
		// The catalog's files in the effect system's order, each name once (a name the scan lists twice is its
		// first file's), the gore set the files' own fgn2.bin picks [orig: Game_LoadConfig @ 0x5514E8..0x5514FA].
		const AssetScan &scan = input.validation.scan;
		std::vector<std::string> names;
		std::set<std::string> seen;
		for (const AssetEntry &entry : scan.entries)
			if (entry.kind == AssetKind::Particles && seen.insert(strutil::to_lower(entry.logical_name)).second)
				names.push_back(entry.logical_name);
		std::vector<std::string> order;
		if (input.validation.project.expansion.standalone())
			order = effect_file_order(names, gore_particle_extension(scan.find(kGoreContentMarker) != nullptr));
		if (made_ && order == order_ && !stamps_.moved(input.files)) return false;
		const std::vector<Diagnostic> before = std::move(findings_);
		findings_.clear();
		stamps_ = FileStamps();
		order_ = order;
		made_ = true;
		// Every file loaded first, as the effect system loads them (one reader state through them all), then every
		// name resolved among them all.
		std::vector<LoadedParticles> loaded;
		std::set<std::string> particles, tables;
		particle::ParticleReadState state;
		for (const std::string &name : order) {
			std::vector<uint8_t> bytes;
			stamps_.note(name, input.files.stamp(name));
			if (!input.files.read(name, bytes)) continue;
			LoadedParticles read;
			const AssetEntry *entry = scan.find(name);
			read.path = entry ? entry->relative_path : name;
			read.text.assign(bytes.begin(), bytes.end());
			particle::ParseError error;
			particle::load_particles_with_places(read.text.data(), read.text.size(), read.file, read.places, state,
					error);
			for (const particle::ParticleDef &definition : read.file.particles) particles.insert(strutil::to_lower(definition.id));
			for (const particle::TableDef &table : read.file.tables) tables.insert(strutil::to_lower(table.id));
			loaded.push_back(std::move(read));
		}
		for (const LoadedParticles &read : loaded) check_file_(read, particles, tables);
		return findings_ != before;
	}
	const std::vector<Diagnostic> &findings() const override { return findings_; }
	void clear() override {
		findings_.clear();
		stamps_ = FileStamps();
		order_.clear();
		made_ = false;
	}

private:
	// A finding at `name`, the `nth` name of the value `place` holds (particle_value_name; the value's start where the
	// word is another): where Problems opens the file; at the fallback where the block writes no such key.
	Diagnostic &add_(ParticleFinding code, std::string message, const LoadedParticles &read, const particle::KeyPlace *place,
			size_t nth, const std::string &name, int fallback_line, int fallback_column) {
		Diagnostic d = make_finding(finding_code(code), DiagnosticSeverity::Warning, std::move(message), read.path);
		if (place) {
			size_t at = place->offset;
			particle_value_name(read.text, place->offset, place->length, nth, name, at);
			const size_t newline = at == 0 ? std::string::npos : read.text.rfind('\n', at - 1);
			d.line = size_t(std::max(place->line, 0));
			d.column = at - (newline == std::string::npos ? 0 : newline + 1) + 1;
		} else {
			d.line = size_t(std::max(fallback_line, 0));
			d.column = size_t(std::max(fallback_column, 0));
		}
		findings_.push_back(std::move(d));
		return findings_.back();
	}

	void check_file_(const LoadedParticles &read, const std::set<std::string> &particles,
			const std::set<std::string> &tables) {
		using particle::BlockKind;
		const std::vector<size_t> lines = text_line_starts(read.text);
		// An effect's particles, each by its id among every file's [particledef]s, without case: the first it
		// misses empties the effect [orig: CEffectBank_ResolveAllEntries @ 0x5E4920 -> @ 0x5E495D, @ 0x5E49BE;
		// CEffectWorld_FindParticleDefByName @ 0x5E41D0, _stricmp @ 0x5E420C]. Its fixes: the particle written into
		// this file, or the member taken out of the line where others stay.
		for (size_t e = 0; e < read.file.effects.size(); ++e) {
			const particle::EffectDef &effect = read.file.effects[e];
			const particle::KeyPlace *place = particle_key_place(read.places, BlockKind::Effect, e, "pdefs");
			const std::string shown = effect.id.empty() ? std::string("an effect of no id") : "the effect " + effect.id;
			// The members of a name, the line's words in order (a member of no name is a slot past the line's words).
			std::vector<std::string> words;
			for (const std::string &member : effect.pdefs)
				if (!member.empty()) words.push_back(member);
			std::string first;
			for (size_t nth = 0; nth < words.size(); ++nth) {
				const std::string &member = words[nth];
				if (particles.count(strutil::to_lower(member))) continue;
				if (first.empty()) first = member;
				Diagnostic &d = add_(ParticleFinding::EffectUnresolved,
						"The particle " + member + ", which " + shown + " names, is defined by no particle file the "
						"game loads: the game resolves an effect's particles all or none, so " + shown +
								" spawns nothing (the game's log: UNRESOLVED: EFFDEF " + effect.id + " missing PARDEF " +
								first + "). Define the particle, or take it out of pdefs.",
						read, place, nth, member, effect.id_line, effect.id_column);
				PlannedFix fix;
				if (add_particle_fix(read, lines, member, fix)) d.planned.push_back(std::move(fix));
				if (place && take_member_fix(read, lines, *place, words, nth, shown, fix)) d.planned.push_back(std::move(fix));
			}
		}
		// A particle's curves, each by its table's id among every file's [tabledef]s, without case: a miss is a
		// null curve [orig: CEffectDef_ResolveTblDefReference @ 0x5E9630; CParticleManager_FindTableDefByName @
		// 0x5E9540, _stricmp @ 0x76FDF6].
		for (size_t p = 0; p < read.file.particles.size(); ++p) {
			const particle::ParticleDef &definition = read.file.particles[p];
			const auto curve = [&](const particle::CurveRef &ref, const std::string &key, const char *channel) {
				if (!ref.present || ref.name.empty() || tables.count(strutil::to_lower(ref.name))) return;
				const particle::KeyPlace *place = particle_key_place(read.places, BlockKind::Particle, p, key);
				// A graphic's curve no line of its own wrote: the copy its graphicN line took of the particle's.
				if (!place && key[0] == 'g') return;
				add_(ParticleFinding::CurveUnresolved,
						"The table " + ref.name + ", which " + key + " of the particle " + definition.id + " names, is "
						"defined by no particle file the game loads: the game logs it not found and draws the particle's " +
								channel + " without its curve. Define the table, or name another.",
						read, place, 0, ref.name, 0, 0);
			};
			curve(definition.scale_func, "scale_func", "scale");
			curve(definition.alpha_func, "alpha_func", "alpha");
			curve(definition.red_func, "red_func", "red");
			curve(definition.green_func, "green_func", "green");
			curve(definition.blue_func, "blue_func", "blue");
			curve(definition.emit_rate_func, "emit_rate_func", "emission rate");
			for (size_t g = 0; g < definition.graphics.size(); ++g) {
				const particle::GraphicLayer &layer = definition.graphics[g];
				if (!layer.present) continue;
				const std::string prefix = "g" + std::to_string(g + 1) + "_";
				curve(layer.scale_func, prefix + "scale_func", "graphic's scale");
				curve(layer.alpha_func, prefix + "alpha_func", "graphic's alpha");
				curve(layer.red_func, prefix + "red_func", "graphic's red");
				curve(layer.green_func, prefix + "green_func", "graphic's green");
				curve(layer.blue_func, prefix + "blue_func", "graphic's blue");
			}
		}
	}

	std::vector<Diagnostic> findings_;
	FileStamps stamps_;
	std::vector<std::string> order_;
	bool made_ = false;
};

} // namespace

std::unique_ptr<ProjectCheck> make_particle_project_check() { return std::make_unique<ParticleProjectCheck>(); }

const FindingCodeRow &finding_code(ParticleFinding code) {
	return kFindingRows[static_cast<size_t>(code)];
}

FindingTable particle_finding_codes() {
	return { kFindingRows.data(), kFindingRows.size() };
}

} // namespace opennova::editor
