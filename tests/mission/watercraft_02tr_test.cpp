// The authored water-training hulls must retain support while idling. In
// particular the selector-zero amphibious LCAC uses the seven-point solve.
// Isolate the hulls over a zero-height water plane, with no terrain support.
#include "common/retail_mission_files.h"
#include "common/retail_paths.h"

#include <cmath>
#include <cstdio>
#include <string>

int main() {
	using namespace opennova;
	RETAIL_REQUIRE_OR_SKIP(install, retail::install(), "OPENNOVA_JO_DIR (02TR.bms)");
	testrig::RetailMissionRig rig;
	std::string error;
	if (!rig.open(install, "02TR.bms", error))
		return retail::skip(error.c_str());
	testrig::BootOptions options;
	options.wac = false;
	options.terrain = false;
	if (!rig.boot(options, error)) {
		std::fprintf(stderr, "FAIL: 02TR boot: %s\n", error.c_str());
		return 1;
	}
	// Production binds the same retained item table for traits, animation,
	// and collision. A read-side rebind must preserve enriched motor geometry.
	rig.resolve_item_traits([](int32_t) -> uint8_t { return 0; });
	const auto *refreshed = rig.world.vehicles.traits.get(1296);
	if (!refreshed || refreshed->box_y_hi <= refreshed->box_y_lo) {
		std::fprintf(stderr, "FAIL: explicit trait sweep lost authored LCAC geometry\n");
		return 1;
	}
	rig.set_items_table(rig.items_table());
	rig.resolve_collision_instances();
	int failures = 0;
	const uint16_t ssns[] = { 4, 7, 1182 };
	float initial_z[3] = {};
	for (int i = 0; i < 3; ++i) {
		const auto *e = rig.world.registry.by_net_id(ssns[i]);
		if (!e) {
			std::fprintf(stderr, "FAIL: missing hull %u\n", ssns[i]);
			return 1;
		}
		initial_z[i] = e->position.z;
		const auto *t = rig.world.vehicles.traits.get(e->item_id);
		if (!t) {
			std::fprintf(stderr, "FAIL: missing traits %u\n", ssns[i]);
			return 1;
		}
		if (t->box_y_hi <= t->box_y_lo || t->box_z_hi <= t->box_z_lo) {
			std::fprintf(stderr, "FAIL: hull %u lost model-derived contact geometry\n", ssns[i]);
			++failures;
		}
	}
	rig.tick(625);
	for (int i = 0; i < 3; ++i) {
		const auto *e = rig.world.registry.by_net_id(ssns[i]);
		if (!e || !std::isfinite(e->position.z) || std::abs(e->position.z - initial_z[i]) > 5.0f) {
			std::fprintf(stderr, "FAIL: hull %u lost idle support (%.4f -> %.4f)\n", ssns[i],
					initial_z[i], e ? e->position.z : -99999.0f);
			++failures;
		}
		if (e)
			std::printf("idle ssn=%u pos=(%.4f,%.4f,%.4f) flags=%08x slide=%d\n", ssns[i],
					e->position.x, e->position.y, e->position.z, e->flags, e->veh.slide_z);
	}
	return failures ? 1 : 0;
}
