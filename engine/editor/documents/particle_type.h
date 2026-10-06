#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document_base.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>
#include <formats/particle/parser.h>

namespace opennova::editor {

// The particle type (ADR 0046 DI-14): a .ptl (and the gore set's .ptu or .ptg), a text document of
// the file as stored, which the game's effect system reads at a mission's start, every file of the
// kind then the gore set, by name [orig: CEffectSystem_Init @ 0x5F6070 -> File_ParseASCIIFile with
// CEffectWorld_ParseSectionCallback @ 0x5ECB40]; the port of that reader is formats/particle's
// load_particles. Its findings are the reader's: where it stops reading the file, and an effect the
// file defines twice. What it names (each effect's id, a symbol at its place; each graphic's texture)
// the asset graph reads through the same reader (graph/extractors.cpp, extract_particles), so the
// type names no references of its own and a rename rewrites its names as a native text's
// (graph/native_text_sites.h). Its Document tab is the script device (S13 V10); the Preview window
// plays the effect the text defines (preview/effect_viewport.h).
std::unique_ptr<DocumentBase> make_particle_document();
std::vector<Diagnostic> validate_particle_file(const DocumentBase &document);

enum class ParticleFinding {
	// The reader stops in the file (an open block, a header with no body): none of the file loads.
	Unreadable,
	// An effect id the file defines again: the game's lookup takes the first.
	DuplicateEffect,
	kCount
};
const FindingCodeRow &finding_code(ParticleFinding code);
FindingTable particle_finding_codes();

// A particle document's text through the game's reader (its effects with their places, as
// formats/particle records them); false, with where it stopped, for one it does not read.
bool read_particle_text(const TextDocument &document, particle::ParticleFile &out, particle::ParseError &error);
// The effect of `file` whose block holds `line` (its header to its closing brace), its index; npos
// for a line in none.
size_t particle_effect_at(const particle::ParticleFile &file, size_t line);

// An effect a file names and no particle file defines, added to a particle file the reader reads whole (ADR 0046
// DI-15, DocumentType::define_symbol): the [effectdef] block the engine's own effect writer makes of an effect of
// that id and nothing else [orig: CParticleEffectDef_WriteToFile @ 0x5e0fe0], as the game's own writer saves
// a new one (formats/particle save_particles), after the file's last effect, in its line ends: one span written.
// The effect system loads every particle file [orig: CEffectSystem_Init @ 0x5F6070] and finds an effect by its
// id without case [orig: CEffectWorld_FindEffectDefByName @ 0x5E34F0]; a name it does not find it clones the
// stock effect for [orig: CEffectWorld_InternEffectHandle @ 0x5f7310].
bool define_particle_effect(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

} // namespace opennova::editor
