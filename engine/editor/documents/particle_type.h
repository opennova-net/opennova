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

class ProjectCheck;

// The particle type (ADR 0046 DI-14): a .ptl (and the gore set's .ptu or .ptg), a text document of
// the file as stored, which the game's effect system reads at a mission's start, the .ptl and gore-set
// files interleaved by the upper-cased name, as one archive's table walks them [orig: CEffectSystem_Init
// @ 0x5F6070 -> File_ParseASCIIFile with CEffectWorld_ParseSectionCallback @ 0x5ECB40]; the port of
// that reader is formats/particle's load_particles. Its findings are the reader's: where it stops
// reading the file, and an effect the file defines twice. What it names (each effect's id, a symbol
// at its place; each graphic's texture) the asset graph reads through the same reader
// (graph/extractors.cpp, extract_particles), so the type names no references of its own and a rename
// rewrites its names as a native text's (graph/native_text_sites.h). Its Document tab is the script
// device (S13 V10); the Preview window plays the effect the text defines (preview/effect_viewport.h).
std::unique_ptr<DocumentBase> make_particle_document();
std::vector<Diagnostic> validate_particle_file(const DocumentBase &document);

enum class ParticleFinding {
	// The reader stops in the file (an open block, a header with no body): none of the file loads.
	Unreadable,
	// An effect id the file defines again: the game's lookup takes the first.
	DuplicateEffect,
	// The project check's (make_particle_project_check): an effect's particle no file the game loads defines, and
	// a particle's curve table no file the game loads defines.
	EffectUnresolved,
	CurveUnresolved,
	// The use check's (graph/use_checks.cpp, documents/particle_token_slots.h): a list the game's parser reads past its
	// line into a token slot no line parsed before it filled, which faults the game as it loads the effects.
	UnfilledTokenSlot,
	kCount
};
const FindingCodeRow &finding_code(ParticleFinding code);
FindingTable particle_finding_codes();

// A particle document's text through the game's reader (its effects with their places, as
// formats/particle records them); false, with where it stopped, for one it does not read.
bool read_particle_text(const TextDocument &document, particle::ParticleFile &out, particle::ParseError &error);

// An effect a file names and no particle file defines, added to a particle file the reader reads whole (ADR 0046
// DI-15, DocumentType::define_symbol): the [effectdef] block the engine's own effect writer makes of an effect of
// that id and nothing else [orig: CParticleEffectDef_WriteToFile @ 0x5e0fe0], as the game's own writer saves
// a new one (formats/particle save_particles), after the file's last effect, in its line ends: one span written.
// The effect system loads every particle file [orig: CEffectSystem_Init @ 0x5F6070] and finds an effect by its
// id without case [orig: CEffectWorld_FindEffectDefByName @ 0x5E34F0]; a name it does not find it clones the
// stock effect for [orig: CEffectWorld_InternEffectHandle @ 0x5f7310].
bool define_particle_effect(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out);

// The particle type's project check (DocumentType::project_check, documents/project_check.h): what the names inside
// the particle files come to once the game has loaded them all. The effect system loads every particle file of the
// catalog (every .ptl and the gore set's .ptu, or .ptg where the files hold fgn2.bin, interleaved by the upper-cased
// name as one archive's table walks them: opennova::effect_file_order [orig: CEffectSystem_Init @ 0x5F6070]) and
// only then resolves the names inside them, each among every file's, without case. Problems rows after the build's
// gate (the game runs the mission either way), warnings at the name:
// - particle.effect_unresolved: an effect's pdefs member no file defines as a [particledef]. The game resolves an
//   effect's particles all or none: the first it misses is logged (UNRESOLVED: EFFDEF <effect> missing PARDEF
//   <particle>) and the effect keeps none of them, so it spawns nothing [orig: CEffectBank_ResolveAllEntries @
//   0x5E4920, its find CEffectWorld_FindParticleDefByName @ 0x5E41D0, _stricmp @ 0x5E420C];
// - particle.curve_unresolved: a particle's *_func (its own or a graphic's) naming a table no file defines as a
//   [tabledef]: the game logs it not found and draws that channel without its curve [orig:
//   CEffectDef_ResolveTblDefReference @ 0x5E9630, its find CParticleManager_FindTableDefByName @ 0x5E9540].
// A file the reader stops in loads nothing (particle.unreadable is its own finding): it defines nothing and its names
// are not checked. An expansion project's base game loads its own particle files with the project's, which the
// project's files do not hold, so the check finds nothing there.
std::unique_ptr<ProjectCheck> make_particle_project_check();

} // namespace opennova::editor
