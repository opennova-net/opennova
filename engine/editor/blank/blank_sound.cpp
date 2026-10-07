// The sound lane's blanks (ADR 0046 d7, the sound lane; DI-33): a bank of no wave and no set, a wave of one
// sample of silence, a dialog bank of no group, and the music pair (a bank of no stream and the script that
// idles over it), each written by the engine's own writer (formats/lwf, formats/dbf, formats/sbf, formats/mus).
#include "blank_makers.h"

#include <cstring>

#include <formats/dbf/dbf.h>
#include <formats/lwf/lwf.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/mus/mus.h>
#include <formats/sbf/sbf.h>

namespace opennova::editor {

namespace {

Diagnostic refusal(CoreFinding code, const BlankRequest &request, const std::string &what, const std::string &why) {
	return make_finding(code, DiagnosticSeverity::Error, what + " could not be written: " + why + ".", request.logical_name);
}

// A music script idling: one section, Begin, whose code is `done`, which sends the script back to its entry
// section and halts [orig: AudioVM_Op_Done @ 0x672cd0], so it never plays a stream. Compiled by the engine's MUS
// compiler and encoded by its writer. One section at least: with no chunk the game's pump reads its first
// opcode from address 0 once the bank is open [orig: ScriptInstance_Init @ 0x672ef0, the empty walk @ 0x672f0b;
// the interpreter's read @ 0x67274d]. `handler`: the chunk's message handler pointed at that `done`, which the
// single-player round's end runs whenever a script is loaded and without which it reads address 0x18 [orig:
// MusicCtx_SelectEndTrack @ 0x672fd0 from Server_ProcessRoundEnd @ 0x51696b / 0x51698f; sub_672E50 @
// 0x672eba..0x672ec1]; the compiler writes none, so it is set here as the mission's script needs it.
bool music_script_bytes(const BlankRequest &request, bool handler, std::vector<uint8_t> &out, Diagnostic &error) {
	mus::MusScript script{};
	int line = 0, column = 0;
	const char *message = nullptr;
	if (mus::mus_compile("script music\nsection Begin\n{\n}\n", &script, &line, &column, &message) != 0) {
		error = refusal(CoreFinding::BlankMusic, request, "The music script", message ? message : "it does not compile");
		return false;
	}
	if (handler) {
		script.has_message_handler = 1;
		script.message_handler_offset = 0;
	}
	const mus::MusScript *scripts[] = {&script};
	uint8_t *buffer = nullptr;
	size_t size = 0;
	const bool written = mus::mus_encode_file(scripts, 1, &buffer, &size) == 0 && buffer;
	if (written) out.assign(buffer, buffer + size);
	mus::mus_free(buffer);
	mus::mus_script_free(&script);
	if (!written) error = refusal(CoreFinding::BlankMusic, request, "The music script", "the encoder refused it");
	return written;
}

} // namespace

// A bank the game opens and finds nothing in [orig: SoundBank_OpenFile @ 0x75caa0 checks no header field;
// SoundBank_LoadTriggerSets @ 0x75c370 reads a set count of 0].
bool make_blank_sound_bank(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	lwf::File bank;
	bank.header.magic = lwf::kMagic;
	std::string message;
	if (!lwf::encode_lwf(bank, out, message)) {
		error = make_finding(CoreFinding::BlankSound, DiagnosticSeverity::Error,
		                     "The sound bank could not be written: " + message + ".", request.logical_name);
		return false;
	}
	return true;
}

// The smallest wave the game's loader plays (docs/audio/lwf-dbf-sound-re.md, "The wave loader's rules" [orig:
// Audio_LoadWavFileFromArchive @ 0x766480]): RIFF..WAVE, a PCM fmt chunk, mono, 16 bits, and a data chunk of one
// sample of silence; none would not do (an empty data chunk is stepped over and the walk runs past the file's
// end, @ 0x76659b..0x7665a5). At 22050 a second, half the mixer's 44100 (the pitch ratio @ 0x766735).
bool make_blank_wave(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	const uint8_t silence[2] = {0, 0};
	std::string why;
	if (!lwf::wav_write_pcm_mono(silence, sizeof(silence), 22050, 16, out, why)) {
		error = refusal(CoreFinding::BlankSound, request, "The wave", why);
		return false;
	}
	return true;
}

// A dialog bank of no group (the writer's header alone: DLG0, no id-def, no group), which the game loads and
// finds no dialog in [orig: DialogManager_LoadFromFile @ 0x44e650: the magic alone checked @ 0x44e6f7, the group
// loop skipped @ 0x44e733]; a mission finds it by its own name [orig: DialogSystem_Init @ 0x5275e0, @ 0x52763e].
bool make_blank_dialog_bank(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	std::string why;
	if (!dbf::encode_dbf(dbf::File{}, out, why)) {
		error = refusal(CoreFinding::BlankSound, request, "The dialog bank", why);
		return false;
	}
	return true;
}

// A music bank of no stream: the writer's 24-byte header, which the game opens by path and takes [orig:
// AudioVM_OpenContextFile @ 0x672160: the SBF0 magic alone checked @ 0x67222b, an entry count of 0 done @
// 0x672253]. It is opened before its script (@ 0x6721da, then @ 0x6721f7), and a bank of no stream must never be played
// from, so it is made with its script (blank_companion), which idles.
bool make_blank_music_bank(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	uint8_t *buffer = nullptr;
	size_t size = 0;
	if (sbf::sbf_encode_file(nullptr, 0, nullptr, nullptr, &buffer, &size) != 0 || !buffer) {
		sbf::sbf_free(buffer);
		error = refusal(CoreFinding::BlankMusic, request, "The music bank", "the encoder refused it");
		return false;
	}
	out.assign(buffer, buffer + size);
	sbf::sbf_free(buffer);
	return true;
}

// The shell's music script (MENUMUS.BIN): the idling script (music_script_bytes), no handler: a mission's start
// frees or replaces the shell's script before a round can end.
bool make_blank_menu_music_script(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return music_script_bytes(request, false, out, error);
}

// A mission's music script (GAMEMUS.BIN): the idling script with the handler the round's end runs.
bool make_blank_game_music_script(const BlankRequest &request, std::vector<uint8_t> &out, Diagnostic &error) {
	return music_script_bytes(request, true, out, error);
}

} // namespace opennova::editor
