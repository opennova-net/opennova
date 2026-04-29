#include <bms/parser.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

// Build a synthetic minimal-valid .bms in-memory: header + 1 pool-1 entity at
// known coords + 0 of everything else. Tests the parser without depending on
// a 300 KB real-file fixture.
bool check_synthetic_minimum() {
	std::vector<uint8_t> buf(0x268 + 0xAC, 0); // header + 1 entity
	buf[0] = 'B'; buf[1] = 'M'; buf[2] = 'S';
	buf[3] = 19;                                // version
	// game_type @ 0x88 = 0x10003
	buf[0x88] = 0x03; buf[0x89] = 0x00; buf[0x8A] = 0x01; buf[0x8B] = 0x00;
	// pool 1 count = 1 @ 0xA4
	buf[0xA4] = 1; buf[0xA5] = 0; buf[0xA6] = 0; buf[0xA7] = 0;
	// loadout sizes both 0 (default zero-init)

	// Entity record at 0x268 — type_id=1234, X=100, Y=200, Z=300, team=2
	uint8_t *e = buf.data() + 0x268;
	e[0] = 1234 & 0xFF; e[1] = (1234 >> 8) & 0xFF; // type_id
	e[16] = 100;                                    // X = 100 (low byte; rest 0)
	e[20] = 200;                                    // Y = 200
	e[24] = 300 & 0xFF; e[25] = (300 >> 8) & 0xFF;  // Z = 300
	e[73] = 2;                                      // team

	opennova::BmsMission m;
	if (!expect(opennova::bms_parse(buf.data(), buf.size(), m),
			"synthetic minimum bms parses")) return false;
	if (!expect(m.version == 19, "version 19")) return false;
	if (!expect(m.game_type == 0x10003, "game_type 0x10003")) return false;
	if (!expect(m.entities.size() == 1, "1 entity")) return false;
	const auto &ent = m.entities.front();
	if (!expect(ent.type_id == 1234, "type_id round-trip")) return false;
	if (!expect(ent.x == 100, "X round-trip")) return false;
	if (!expect(ent.y == 200, "Y round-trip")) return false;
	if (!expect(ent.z == 300, "Z round-trip")) return false;
	if (!expect(ent.team == 2, "team round-trip")) return false;
	if (!expect(ent.pool == 1, "pool 1")) return false;
	if (!expect(ent.slot == 0, "slot 0")) return false;
	return true;
}

bool check_bad_magic_rejected() {
	std::vector<uint8_t> buf(0x268, 0);
	buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 19;
	opennova::BmsMission m;
	if (!expect(!opennova::bms_parse(buf.data(), buf.size(), m),
			"non-BMS magic rejected")) return false;
	return true;
}

bool check_old_version_rejected() {
	std::vector<uint8_t> buf(0x268, 0);
	buf[0] = 'B'; buf[1] = 'M'; buf[2] = 'S';
	buf[3] = 18;
	opennova::BmsMission m;
	if (!expect(!opennova::bms_parse(buf.data(), buf.size(), m),
			"version < 19 rejected")) return false;
	return true;
}

bool check_truncated_rejected() {
	std::vector<uint8_t> buf(100, 0); // way short of header
	opennova::BmsMission m;
	if (!expect(!opennova::bms_parse(buf.data(), buf.size(), m),
			"truncated input rejected")) return false;
	return true;
}

// Live-fire test against the real ASH_I5A.bms. Skipped at runtime if the
// fixture isn't present — keeps CI green on machines without the JO assets.
bool check_real_dvxi5_fixture() {
	const char *path = "C:\\Users\\taylor\\Desktop\\JO_ASSETS_t\\ASH_I5A.bms";
	opennova::BmsMission m;
	if (!opennova::bms_load_file(path, m)) {
		std::fprintf(stderr,
				"NOTE: ASH_I5A.bms fixture not present at %s — skipping real-file checks\n",
				path);
		return true;
	}
	if (!expect(m.version == 19, "real dvxi5 version 19")) return false;
	if (!expect(m.game_type == 0x10003, "real dvxi5 game_type 0x10003")) return false;
	// Per python pre-flight: pool counts pool0=0, pool1=76, pool2=1102, pool3=432.
	// Total entities (any non-zero record): 76 + 1102 + 432 + 0 = 1610.
	if (!expect(m.entities.size() == 1610, "real dvxi5 entity count = 1610")) return false;
	// Pool 1 entity 0 should be type 1215 at (31403009, -18642650, 958739).
	const auto &p1_first = m.entities[0];
	if (!expect(p1_first.pool == 1 && p1_first.slot == 0,
			"first entity is pool1[0]")) return false;
	if (!expect(p1_first.type_id == 1215, "pool1[0] type_id 1215")) return false;
	if (!expect(p1_first.x == 31403009 && p1_first.y == -18642650 && p1_first.z == 958739,
			"pool1[0] position matches expected")) return false;

	// bms_pick_safe_spawn should pick a non-zero, non-anomalous coord.
	opennova::BmsEntity spawn{};
	if (!expect(opennova::bms_pick_safe_spawn(m, spawn),
			"safe spawn pick succeeds")) return false;
	if (!expect(spawn.x != 0 || spawn.y != 0 || spawn.z != 0,
			"safe spawn is non-origin")) return false;
	std::fprintf(stderr,
			"INFO: dvxi5 safe spawn picked: pool=%u slot=%u type=%u pos=(%d,%d,%d) team=%u\n",
			spawn.pool, spawn.slot, spawn.type_id, spawn.x, spawn.y, spawn.z, spawn.team);
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_synthetic_minimum() && ok;
	ok = check_bad_magic_rejected() && ok;
	ok = check_old_version_rejected() && ok;
	ok = check_truncated_rejected() && ok;
	ok = check_real_dvxi5_fixture() && ok;
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
