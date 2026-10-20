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
	{ ParticleFinding::EffectUnresolved, listed_code("particle.effect_unresolved") },
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
	std::string block = written.str();
	// In the file's line ends: the writer's CR LF, an LF alone where the file's lines end so (the line-ends rule
	// says what the game reads of such a file, documents/line_ends.h).
	if (!text->text().empty() && text->text().find("\r\n") == std::string::npos) {
		std::string lf;
		for (const char c : block)
			if (c != '\r') lf += c;
		block = std::move(lf);
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

// The last place the block (`kind`, `index`) writes `key` (without case: the reader folds known keys, and of a key
// written twice the last is read).
const particle::KeyPlace *key_place(const particle::ParticlePlaces &places, particle::BlockKind kind, size_t index,
		const std::string &key) {
	const particle::KeyPlace *found = nullptr;
	for (const particle::KeyPlace &place : places.keys)
		if (place.block == kind && place.index == index && strutil::iequals(place.key, key)) found = &place;
	return found;
}

// The line and column (1-based) of `name` inside the value `place` holds (the value's start where it does not
// stand alone among the commas): where Problems opens the file.
void name_place(const std::string &text, const particle::KeyPlace &place, const std::string &name, size_t &line,
		size_t &column) {
	const auto separates = [](char c) { return c == ',' || c == ' ' || c == '\t'; };
	const size_t end = std::min(text.size(), place.offset + place.length);
	size_t at = place.offset;
	for (size_t from = place.offset; from < end && !name.empty();) {
		const size_t found = text.find(name, from);
		if (found == std::string::npos || found + name.size() > end) break;
		const char before = found > place.offset ? text[found - 1] : ',';
		const char after = found + name.size() < end ? text[found + name.size()] : ',';
		if (separates(before) && separates(after)) {
			at = found;
			break;
		}
		from = found + 1;
	}
	const size_t newline = at == 0 ? std::string::npos : text.rfind('\n', at - 1);
	line = size_t(std::max(place.line, 0));
	column = at - (newline == std::string::npos ? 0 : newline + 1) + 1;
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
	void add_(ParticleFinding code, std::string message, const LoadedParticles &read, const particle::KeyPlace *place,
			const std::string &name, int fallback_line, int fallback_column) {
		Diagnostic d = make_finding(finding_code(code), DiagnosticSeverity::Warning, std::move(message), read.path);
		if (place) {
			name_place(read.text, *place, name, d.line, d.column);
		} else {
			d.line = size_t(std::max(fallback_line, 0));
			d.column = size_t(std::max(fallback_column, 0));
		}
		findings_.push_back(std::move(d));
	}

	void check_file_(const LoadedParticles &read, const std::set<std::string> &particles,
			const std::set<std::string> &tables) {
		using particle::BlockKind;
		// An effect's particles, each by its id among every file's [particledef]s, without case: the first it
		// misses empties the effect [orig: CEffectBank_ResolveAllEntries @ 0x5E4920 -> @ 0x5E495D, @ 0x5E49BE;
		// CEffectWorld_FindParticleDefByName @ 0x5E41D0, _stricmp @ 0x5E420C].
		for (size_t e = 0; e < read.file.effects.size(); ++e) {
			const particle::EffectDef &effect = read.file.effects[e];
			const particle::KeyPlace *place = key_place(read.places, BlockKind::Effect, e, "pdefs");
			const std::string shown = effect.id.empty() ? std::string("an effect of no id") : "the effect " + effect.id;
			std::string first;
			for (const std::string &member : effect.pdefs) {
				// A member of no name (the editor's Add writes one) is no name to report.
				if (member.empty() || particles.count(strutil::to_lower(member))) continue;
				if (first.empty()) first = member;
				add_(ParticleFinding::EffectUnresolved,
						"The particle " + member + ", which " + shown + " names, is defined by no particle file the "
						"game loads: the game resolves an effect's particles all or none, so " + shown +
								" spawns nothing (the game's log: UNRESOLVED: EFFDEF " + effect.id + " missing PARDEF " +
								first + "). Define the particle, or take it out of pdefs.",
						read, place, member, effect.id_line, effect.id_column);
			}
		}
		// A particle's curves, each by its table's id among every file's [tabledef]s, without case: a miss is a
		// null curve [orig: CEffectDef_ResolveTblDefReference @ 0x5E9630; CParticleManager_FindTableDefByName @
		// 0x5E9540, _stricmp @ 0x76FDF6].
		for (size_t p = 0; p < read.file.particles.size(); ++p) {
			const particle::ParticleDef &definition = read.file.particles[p];
			const auto curve = [&](const particle::CurveRef &ref, const std::string &key, const char *channel) {
				if (!ref.present || ref.name.empty() || tables.count(strutil::to_lower(ref.name))) return;
				const particle::KeyPlace *place = key_place(read.places, BlockKind::Particle, p, key);
				// A graphic's curve no line of its own wrote: the copy its graphicN line took of the particle's.
				if (!place && key[0] == 'g') return;
				add_(ParticleFinding::CurveUnresolved,
						"The table " + ref.name + ", which " + key + " of the particle " + definition.id + " names, is "
						"defined by no particle file the game loads: the game logs it not found and draws the particle's " +
								channel + " without its curve. Define the table, or name another.",
						read, place, ref.name, 0, 0);
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
