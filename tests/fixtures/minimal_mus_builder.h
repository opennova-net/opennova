// The minimal set's generated music: one silent SBF bank + one minimal MUS
// script, emitted under both boot music names. The base-game boot wires the
// hardcoded pairs MENUMUS.SBF/MENUMUS.BIN + GAMEMUS.SBF/GAMEMUS.BIN
// [orig: Expansion_LoadAssets @ 0x4a4730 else-branch]; retail ships the .sbf
// banks LOOSE in the game dir and the .bin scripts in localres.pff. Both are
// pure writer output (libs/sbf encoder + libs/mus compiler), generated at
// package time, never committed.
#ifndef OPENNOVA_TESTS_MINIMAL_MUS_BUILDER_H
#define OPENNOVA_TESTS_MINIMAL_MUS_BUILDER_H

#include <mus/mus.h>
#include <sbf/sbf.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace minimal_mus {

// Half a second of silence at the banks' nominal 22050 Hz — enough for the
// script's `play` to have a real entry to address.
inline int build_silent_sbf(std::vector<uint8_t> *out) {
	static const char *kNames[] = {"silence"};
	const std::vector<int16_t> pcm(11025, 0);
	const int16_t *samples[] = {pcm.data()};
	const size_t counts[] = {pcm.size()};
	uint8_t *buf = nullptr;
	size_t size = 0;
	const int rc = sbf_encode_file(kNames, 1, samples, counts, &buf, &size);
	if (rc != 0) return rc;
	out->assign(buf, buf + size);
	sbf_free(buf);
	return 0;
}

// The smallest useful script: enter, play the silent entry, done. The AudioVM
// parses the SCR0 container and runs Begin; a `done` script idles quietly.
inline int build_minimal_mus(const char *script_name, std::vector<uint8_t> *out) {
	char src[128];
	std::snprintf(src, sizeof(src),
	              "script %s\n"
	              "section Begin\n"
	              "{\n"
	              "  play sound_0\n"
	              "  done\n"
	              "}\n",
	              script_name);
	MusScript script = {};
	int err_line = 0, err_col = 0;
	const char *err_msg = nullptr;
	int rc = mus_compile(src, &script, &err_line, &err_col, &err_msg);
	if (rc != 0) return rc;
	const MusScript *scripts[1] = {&script};
	uint8_t *buf = nullptr;
	size_t size = 0;
	rc = mus_encode_file(scripts, 1, &buf, &size);
	mus_script_free(&script);
	if (rc != 0) return rc;
	out->assign(buf, buf + size);
	mus_free(buf);
	return 0;
}

} // namespace minimal_mus

#endif // OPENNOVA_TESTS_MINIMAL_MUS_BUILDER_H
