// The text families' blanks (ADR 0046 DI-33): a particle file, a credits file, an AI profile, the HUD layout and
// the avatars table, each the smallest file its game reader takes, through the engine's own writer where it has
// one (formats/particle, formats/def, formats/avatars), a comment line naming it first: every one of these the
// game reads through the shared ASCII walk, which skips a line whose first word starts with '/' [orig:
// File_ParseASCIIFile @ 0x53D810, the skip @ 0x53D908..0x53D91E], or the ConfigFile reader (the credits).
#include "blank_makers.h"

#include <cstdio>
#include <sstream>

#include <editor/project/project_files.h>
#include <formats/avatars/avatars.h>
#include <formats/def/def_hudpos_write.h>
#include <formats/grm/grm.h>
#include <formats/particle/parser.h>
#include <runtime/menu/menu_credits.h>

namespace opennova::editor {

namespace {

std::string title_of(const BlankRequest &request) {
	return request.project_title.empty() ? std::string("the project") : request.project_title;
}

} // namespace

// A particle file with no effect: the effect writer's empty file (save_particles), a comment line first. The game
// reads every particle file the archives hold, whatever its name, and takes an empty one as nothing [orig:
// CEffectSystem_Init @ 0x5F6070, the results unread @ 0x5f62f0 / 0x5f6545].
bool make_blank_particles(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::ostringstream text;
	std::string why;
	if (!particle::save_particles(text, particle::ParticleFile{}, why)) {
		out.clear();
		error = make_finding(CoreFinding::BlankParticles, DiagnosticSeverity::Error,
		                     "The particle file could not be written: " + why + ".", request.logical_name);
		return false;
	}
	blank_text_to_bytes("// Particle effects of " + title_of(request) + ": [effectdef], [particledef] and [tabledef] blocks.\n" +
	                            text.str(),
	                    out);
	return true;
}

// A credits roll of one line: the ConfigFile text the marquee reads [orig: CMarqueeWnd_LoadCreditsFromIni @
// 0x65c5a0 -> ConfigFile_LoadGlobal @ 0x760ad0]: its [ENV] keys at the values a load resets them to
// (menu::kMarqueeScrollRate, kMarqueeCenterX, kMarqueeVerticalSpace; an [ENV] that lacks one reads it as 0), then
// one [TEXT] line, the project's title in the shell's bold 14-point font, which the menus require (Arial14b.fnt),
// marked as a load's marks draw it (menu::marquee_marked_line). Not empty: the ConfigFile reader takes no file of
// no byte [orig: ConfigFile_LoadFromFile @ 0x760a74]. The text form, which the credits document holds as it is: the
// CBIN writer would write an empty section's terminator the game does not read.
bool make_blank_credits(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	char env[128];
	std::snprintf(env, sizeof(env), "[ENV]\nSCROLL_RATE = %.1f\nCENTER_X = %d\nVERTICAL_SPACE = %d\n\n",
	              double(menu::kMarqueeScrollRate), menu::kMarqueeCenterX, menu::kMarqueeVerticalSpace);
	blank_text_to_bytes(std::string(env) + "[TEXT]\nTEXT = " + menu::marquee_marked_line(title_of(request)) +
	                            ", Arial14b\n",
	                    out);
	return true;
}

// An AI profile the game loads as a profile of no type (every key zero, as a missing one [orig: AIProfile_LoadOrFind
// @ 0x45fd80, the record cleared @ 0x45fe09]): a comment line saying the grammar, a `type` line first (HELO, GROUND or
// ORGANIC) and the keys it gates after it [orig: AIProfile_ParseProperty @ 0x45DE70]. The engine has a reader of
// the format (formats/aip) and no writer: the profile's keys are its own slice, so this is the text the game's walk
// skips, as a new script is.
bool make_blank_ai_profile(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &) {
	blank_text_to_bytes("// The AI profile " + request.logical_name +
	                            ": a type line first (type HELO, GROUND or ORGANIC), then the keys its type reads.\n",
	                    out);
	return true;
}

// The HUD layout as the game has it with no file (every position at the start value its globals hold): the
// layout writer's file of nothing authored (def_write_hudpos), a comment line first, which the HUD's parse reads
// as a file it is missing [orig: HUD_InitOverlaySystem @ 0x5a4620 (hudpos @ 0x5a4931) -> HUD_ParseHudposToken @
// 0x59f370]. Opened in the HUD layout's view to move its elements from there (DI-37).
bool make_blank_hud_layout(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const def::DefWriteResult written = def::def_write_hudpos(def::DefHudPosFile{});
	if (!written.ok()) {
		out.clear();
		error = make_finding(CoreFinding::BlankDef, DiagnosticSeverity::Error, "The HUD layout could not be written.",
		                     request.logical_name);
		return false;
	}
	blank_text_to_bytes("// The HUD layout of " + title_of(request) + ": each element's position as <KEY> <values>.\n" +
	                            written.text,
	                    out);
	return true;
}

// The avatars table of no part, nationality or combo: the avatars writer's empty file, which the game reads as it
// reads none [orig: CAvatarDefs_Init @ 0x57b180; CAvatarDefs_ParseConfigLine @ 0x57a3fc]; a combo the player wears
// is added to it (without one, first person draws no arms: docs/required-resources.md).
bool make_blank_avatars(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	avatars::AvatarsFile file{};
	char *data = nullptr;
	size_t size = 0;
	if (avatars::avatars_write(&file, &data, &size) != 0 || !data) {
		avatars::avatars_free_buffer(data);
		out.clear();
		error = make_finding(CoreFinding::BlankDef, DiagnosticSeverity::Error, "The avatars table could not be written.",
		                     request.logical_name);
		return false;
	}
	out.assign(data, data + size);
	avatars::avatars_free_buffer(data);
	return true;
}

// A face of no texture, mesh or gesture, its eyes where the game puts them with none: the GRM writer's file of an
// empty face (grm::write), which the game's reader loads as a face [orig: FaceAnimConfig_LoadFile @ 0x588BE0 takes a
// file of no line; sub_5891E0 @ 0x5891E0 seeds the eyes, FaceAnimConfig_InitEyeDefaults @ 0x588D20] and its
// compositor draws with nothing to deform [orig: Render_ScarDebugOverlay @ 0x589220, the triangles under their count
// @ 0x5892D4; sub_588FE0 @ 0x588FE0, no gesture]. The writer writes every count line, which a face loaded into a
// slot another mission's face held needs: the mission's end frees the arrays and keeps their counts [orig:
// sub_57FBB0 @ 0x57FBB0 -> sub_587C30 @ 0x587C30].
bool make_blank_face_animation(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string why;
	if (!grm::write(grm::File{}, out, why)) {
		out.clear();
		error = make_finding(CoreFinding::BlankDef, DiagnosticSeverity::Error, "The face animation could not be written: " + why + ".",
		                     request.logical_name);
		return false;
	}
	return true;
}

} // namespace opennova::editor
