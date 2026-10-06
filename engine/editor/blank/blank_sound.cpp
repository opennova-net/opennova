// The sound banks' blank (ADR 0046 d7, the sound lane): a bank of no wave and no set, written by the
// engine's writer (formats/lwf), which the game opens and finds nothing in [orig: SoundBank_OpenFile @
// 0x75caa0 checks no header field; SoundBank_LoadTriggerSets @ 0x75c370 reads a set count of 0].
#include "blank_makers.h"

#include <formats/lwf/lwf.h>

namespace opennova::editor {

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

} // namespace opennova::editor
