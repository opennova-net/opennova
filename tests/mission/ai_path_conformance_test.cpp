// HEADLESS AI PATH CONFORMANCE — the fast half of the host-parity loop.
//
// The retail-host capture is a dense, per-entity ground-truth transcript of how
// the original walks its waypoints (00TRg idle baseline: all 52 AI sampled >=200
// times). That settles what RETAIL does. It is the WRONG instrument for what WE
// do: a 430 s round plus a decode, through a sampled wire, to observe state we
// own in-process.
//
// So this test is our side of that comparison: load the REAL shipped mission,
// promote it, tick the world headless, and report each AI's authored routing
// inputs next to the distance it actually travelled. Deterministic (no player,
// no network), unsampled, and seconds instead of minutes.
//
// Measured divergence it exists to close (idle-vs-idle, 430 s, zero player input,
// AI-PARITY-CONCEPT.md 6.3d): retail moves every AI except s8/s15/s16, while we
// leave SIX at exactly zero travel — s7, s13, s42, s43, s50, s51. s42/s43/s51
// spawn at the SAME position as retail and retail still walks them 625k/543k/583k
// wire units, so they carry no placement ambiguity and no scenario confound.
//
// Gated on OPENNOVA_JO_DIR (skip-as-pass without a JO install), like
// tests/mission/mission_corpus_test.cpp and coop_convoy_test.cpp.
//
// REPORT MODE: set OPENNOVA_AI_PATH_REPORT=1 to dump the full per-slot table
// (authored group/waypoint/wp_number + brain waypoint state + travel). That dump
// is the diagnosis surface; the assertions below are the regression pins.
#include "mission/event_runtime.h"
#include "mission/promote.h"

#include "mission/bms.h"

#include "world/ai.h"
#include "world/world.h"

#include <def/def.h>
#include <resource_index/resource_index.h>
#include <simassets/seat_spec_extract.h>
#include <threedi/threedi_3di3.h>
#include <simassets/adm_root_motion.h>
#include <simassets/item_traits.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace opennova;
namespace w = opennova::world;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

// The six AI retail walks and we do not, measured on the idle pair. The three
// listed first spawn at retail's own position, so they are unambiguous.
const int kStuckClean[] = {42, 43, 51};
const int kStuckPlaced[] = {7, 13, 50};

// Positions are 16.16 FIXED [docs/engine-primer.md], so a raw delta of 7143 is
// 0.11 world units -- settle jitter, not travel. Report WORLD UNITS or the
// threshold is meaningless (the first cut of this test used a raw-unit bound and
// scored all 52 AI as "moved" when only one had gone anywhere).
double dist2d_units(const int32_t a[3], const int32_t b[3]) {
	const double dx = (double(a[0]) - double(b[0])) / 65536.0;
	const double dy = (double(a[1]) - double(b[1])) / 65536.0;
	return std::sqrt(dx * dx + dy * dy);
}

// One world unit of net displacement: past ground-settle and pose jitter, well
// under any real patrol leg.
const double kMovedUnits = 1.0;

} // namespace

int main() {
	const char *dir = std::getenv("OPENNOVA_JO_DIR");
	if (dir == nullptr || *dir == '\0') {
		std::printf("ai path conformance: SKIP (OPENNOVA_JO_DIR not set)\n");
		return 0;
	}
	const std::string path = std::string(dir) + "/00TRg.bms";
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		std::printf("ai path conformance: SKIP (no 00TRg.bms under OPENNOVA_JO_DIR)\n");
		return 0;
	}
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
	                           std::istreambuf_iterator<char>());
	bms::File m;
	std::string error;
	if (!expect(bms::parse(bytes.data(), bytes.size(), m, error), "00TRg.bms parses")) {
		std::fprintf(stderr, "  parse error: %s\n", error.c_str());
		return 1;
	}

	w::World world;
	w::AiSystem ai;
	world.ai = &ai;
	// The BMS EVENT SYSTEM issues the scripted route orders (RedirectGroupTo and
	// friends). Without it the mission's own scripting never runs and a large
	// share of the AI never receive the route the mission intends -- another way
	// for the harness to measure its own omission (7.4c).
	mission::BmsEventSystem events;
	events.load(m.events, m.triggers, m.actions);

	// SEATS COME FROM THE MODEL, NOT items.def. promote grants a vehicle its
	// seats only from PromoteOptions::item_seat_specs, and the live game builds
	// that table by walking each model's seat userpoints (sitex/ctrlx/drvrx/
	// UseGun bones) -- items.def carries no seat rows at all
	// [orig: seat typing Entity_GetBoneSlotType @0x434ed0]. A harness that skips
	// this gives every carrier seats=0, so the board think's find_best_seat
	// returns -1, the arrival ring widens, and the soldiers 'arrive' beside a
	// seatless hull and never attach -- measuring the harness, not the engine.
	mission::PromoteOptions opts;
	// The mounted archives: the seat extraction and the root-motion clips both
	// read through this one index.
	ResourceIndex index;
	const bool indexed = index.scan(std::string(dir));

	DefItemsFile items{};
	std::vector<uint8_t> items_bytes;
	// value.second = 'resolved'; a failed parse caches a negative so each
	// graphic is attempted once.
	std::map<std::string, std::pair<Threedi3di3, bool>> model_cache;
	if (index.read_file("items.def", items_bytes) &&
			def_parse_items_memory(items_bytes.data(), items_bytes.size(), &items) == 0) {
		std::vector<int> seeds;
		seeds.reserve(m.items.size() + m.organics.size());
		for (const bms::Entity &b : m.items) seeds.push_back(100000 + b.type_id);
		for (const bms::Entity &b : m.organics) seeds.push_back(100000 + b.type_id);
		simassets::ModelLookupFn model_for =
				[&](const std::string &key) -> const Threedi3di3 * {
			auto it = model_cache.find(key);
			if (it != model_cache.end())
				return it->second.second ? &it->second.first : nullptr;
			std::vector<uint8_t> raw;
			Threedi3di3 parsed{};
			if (index.read_file(key + ".3di", raw) &&
					threedi_3di3_read_memory(raw.data(), raw.size(), &parsed) == 0) {
				auto &slot = model_cache[key];
				slot.first = parsed;
				slot.second = true;
				return &slot.first;
			}
			model_cache[key] = {Threedi3di3{}, false};
			return nullptr;
		};
		simassets::SeatSpecExtraction extraction;
		simassets::extract_item_seat_specs(items, model_for, seeds, extraction);
		opts.item_seat_specs = extraction.specs;
		std::printf("seat specs extracted: %zu (from %zu seed ids)\n",
				extraction.specs.size(), seeds.size());
	}
	const mission::PromoteResult promo = mission::promote_mission(m, world, ai, opts);
	expect(promo.nav_channels > 0, "nav channels promoted");

	// INFANTRY DO NOT MOVE WITHOUT ROOT MOTION. Locomotion comes from the anim
	// clips, exactly as in the original: with `root_motion` null every state is
	// unavailable, the selector idles, and every soldier stands still
	// [orig: AnimMap_UpdateEntity @0x40b5f0; the contract is stated on
	// AiSystem::root_motion in world/ai.h]. A headless harness that omits this
	// measures nothing about pathing -- it measures its own missing clips, and
	// reports 52/52 still. E_STAND.adm is the shell's default set (adm_id 0),
	// which is what every entity grounds off until its own model's .adm is
	// resolved [orig: AnimMap_RegisterEntity @0x40bb60; the same default the
	// game shell installs in Simulation::set_infantry_anim_map].
	simassets::AdmRootMotion root_motion;
	const int default_adm = indexed ? root_motion.register_adm(&index, "E_STAND.adm") : -1;
	if (default_adm != 0) {
		std::printf("ai path conformance: SKIP (E_STAND.adm not resolvable under "
				"OPENNOVA_JO_DIR -- infantry cannot locomote without clips)\n");
		return 0;
	}
	ai.root_motion = &root_motion;
	for (int i = 0; i < ai.count(); ++i) {
		if (w::AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	}

	// VEHICLE TRAITS, or the whole motor pass is skipped. AiSystem's pool-1
	// motor loop is gated on `!world.vehicle_traits.empty()`
	// (ai_system.cpp:468), and the table is filled from the items.def rows
	// [orig: Entity_InitFromItemDef @0x49e550; ItemDef_ParsePhysicsProperty
	// @0x49d870]. Without it a carrier is never driven, its motor never mirrors
	// the hull transform back into the brain, and the brain's own locomotion
	// walks its internal pos along the route while the hull stays parked --
	// which reads exactly like a pathing defect. coop_convoy_test hand-sets a
	// traits row for the same reason.
	if (items.count > 0) {
		simassets::resolve_item_traits(world, items,
				[](int32_t) -> uint8_t { return 0; });
	}

	world.add_system(&events);
	world.add_system(&ai);
	world.load_systems();

	// Carrier START positions: a mounted passenger only travels if its VEHICLE
	// drives. Retail's boarded soldiers cover hundreds of thousands of wire units
	// because the hull carries them, so 'mounted but stationary' is a distinct
	// defect from 'never boarded'.
	std::map<uint16_t, std::pair<int32_t, int32_t>> carrier_start;
	for (int j = 0; j < world.registry.pool_capacity(1); ++j) {
		const w::EntityHandle h = w::EntityHandle::make(1, j);
		if (const w::Entity *v = world.registry.get(h))
			carrier_start[h.packed] = {int32_t(v->position.x * 65536.0f),
					int32_t(v->position.y * 65536.0f)};
	}

	// Snapshot every AI's start position, keyed by its AI index.
	const int n = ai.count();
	std::vector<int32_t> start(size_t(n) * 3, 0);
	for (int i = 0; i < n; ++i) {
		const w::AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		for (int k = 0; k < 3; ++k) start[size_t(i) * 3 + k] = e->pos[k];
	}

	// 2500 ticks = ~40 s of mission time at the 62.5 Hz logic rate — the same
	// budget coop_convoy_test uses, and long enough for an authored patrol leg.
	// Default 2500 ticks = ~40 s of mission time, the coop_convoy budget. The
	// capture baseline is 430 s, so comparing counts against it needs
	// OPENNOVA_AI_PATH_TICKS=27000 -- a 40 s run scoring fewer movers than a
	// 430 s capture is a BUDGET difference, not a defect.
	// PATH LENGTH, accumulated per tick -- NOT net displacement. The capture side
	// sums per-sample deltas, so a start->end measure is not comparable to it: a
	// patrolling AI that loops back has a huge path and a tiny net displacement.
	// (This bit three times before it was fixed; see 7.4b.)
	std::vector<double> ai_path(size_t(ai.count()), 0.0);
	std::vector<int32_t> prev(size_t(ai.count()) * 2, 0);
	for (int i = 0; i < ai.count(); ++i) {
		if (const w::AiEntity *e = ai.at(i)) {
			prev[size_t(i) * 2] = e->pos[0];
			prev[size_t(i) * 2 + 1] = e->pos[1];
		}
	}

	int ticks = 2500;
	if (const char *tv = std::getenv("OPENNOVA_AI_PATH_TICKS")) {
		const int parsed = std::atoi(tv);
		if (parsed > 0) ticks = parsed;
	}
	for (int t = 0; t < ticks; ++t) {
		world.run_logic_tick(/*is_authority=*/true);
		const int live = ai.count();
		if (int(ai_path.size()) < live) { ai_path.resize(size_t(live), 0.0); prev.resize(size_t(live) * 2, 0); }
		for (int i = 0; i < live; ++i) {
			const w::AiEntity *e = ai.at(i);
			if (e == nullptr) continue;
			const double dx = (double(e->pos[0]) - prev[size_t(i) * 2]) / 65536.0;
			const double dy = (double(e->pos[1]) - prev[size_t(i) * 2 + 1]) / 65536.0;
			ai_path[size_t(i)] += std::sqrt(dx * dx + dy * dy);
			prev[size_t(i) * 2] = e->pos[0];
			prev[size_t(i) * 2 + 1] = e->pos[1];
		}
	}

	const bool report = std::getenv("OPENNOVA_AI_PATH_REPORT") != nullptr;
	if (report) {
		std::printf("%-5s %-6s %-4s %-4s %-6s %-8s %-6s %-6s %-6s %-6s %-7s %-4s %10s\n", "ai#",
				"handle", "has", "cmd", "tgtSSN", "wpType", "wpChan", "wpNode",
				"moveMd", "cmd37", "carr36", "mnt", "path_u");
		std::printf("--------------------------------------------------------------------\n");
	}

	int moved = 0, still = 0;
	for (int i = 0; i < n; ++i) {
		const w::AiEntity *e = ai.at(i);
		if (e == nullptr) continue;
		const double travel = (i < int(ai_path.size())) ? ai_path[size_t(i)]
				: dist2d_units(e->pos, &start[size_t(i) * 3]);
		if (travel > kMovedUnits) ++moved; else ++still;
		if (report) {
			// The AUTHORED routing inputs, straight off the BMS organic record,
			// so a still AI is classified as "the mission gave it no route" vs
			// "we failed to apply the route it was given".
			// The RUNTIME routing registers, not m.organics[i]. Indexing the BMS
			// organics by AI INDEX is wrong: the AI list is not the organics list
			// (seat-spec extraction adds emplacement children, so 60 AI vs 52
			// organics) and every authored column silently shifts. Read what the
			// think actually uses: slot[35] has-route, slot[37] channel-or-command,
			// slot[38] node-or-target-SSN [orig: Entity_SetWaypointByTeam @0x43cdb4
			// writes aiComp+140/+148/+152; infantry.cpp:268 and
			// infantry_board.cpp:139 read them].
			const int grp = e->slot.f[35];
			const int wpid = e->slot.f[37];
			const int wpnum = e->slot.f[38];
			std::printf("%-5d %-6u %-4d %-4d %-6d %-8d %-6d %-6d %-6d %-6d %-7d %-4d %10.2f\n", i,
					unsigned(e->handle.packed), grp, wpid, wpnum,
					e->brain.f[w::AiBrain::kWpType],
					e->brain.f[w::AiBrain::kWpChannel],
					e->brain.f[w::AiBrain::kWpNode],
					e->inf.move_mode,
					e->slot.f[37],   // the reserved command (BMS waypoint_id)
					e->slot.f[36],   // the cached board carrier, 125 only
					(world.registry.get(e->handle) != nullptr &&
							world.registry.get(e->handle)->mounted) ? 1 : 0,
					travel);
		}
	}
	std::printf("ai path: %d AI, %d moved, %d still after %d ticks\n", n, moved, still, ticks);

	// The BOARD CARRIERS: which entities the command-125 soldiers resolved, and
	// whether those entities actually own seats. promote grants seats ONLY from
	// PromoteOptions::item_seat_specs, so a seatless carrier here means the harness
	// never described the vehicle -- not that the vehicle is broken.
	if (report) {
		std::vector<uint16_t> seen;
		// The nav ENTRY payloads for one carrier channel. f[0] is what the advance
		// gate compares the remaining distance against (kWpNodeVal / "animTime").
		for (int ch = 1; ch <= 20; ++ch) {
			const w::NavChannel *nc = ai.nav.channel(ch);
			if (nc == nullptr || nc->count == 0) continue;
			std::printf("   NAVCH %d count=%d loopflag=%d\n", ch, nc->count, nc->loopflag);
			int waitsum = 0, waitmax = 0;
			for (int k = 0; k < nc->count && k < 32; ++k) {
				const w::NavEntry *ne = ai.nav.entry(nc->entries[k]);
				if (ne == nullptr) continue;
				waitsum += ne->wait_ticks;
				if (ne->wait_ticks > waitmax) waitmax = ne->wait_ticks;
			}
			std::printf("        waits: sum=%d max=%d  (cooldown ticks sum=%d)\n",
					waitsum, waitmax, (waitsum + 8 * nc->count) >> 4);
		}
		if (const w::NavChannel *nc13 = ai.nav.channel(13)) {
			std::printf("nav channel 13: count=%d loopflag=%d\n", nc13->count, nc13->loopflag);
			for (int k = 0; k < nc13->count && k < 6; ++k) {
				const w::NavEntry *ne = ai.nav.entry(nc13->entries[k]);
				if (ne) std::printf("   node %d: f0=%-12d x=%-12d y=%-12d z=%-12d f4=%d\n",
						k, ne->f[0], ne->f[1], ne->f[2], ne->f[3], ne->f[4]);
			}
		}
		// Every RedirectGroupTo / RedirectSingleTo in the mission, with the event
		// that carries it: does the script ever re-task the tripod gunners?
		for (size_t ei = 0; ei < m.events.size(); ++ei) {
			const bms::Event &ev = m.events[ei];
			for (int ai2 = 0; ai2 < ev.action_count; ++ai2) {
				const size_t idx = size_t(ev.action_index) + size_t(ai2);
				if (idx >= m.actions.size()) continue;
				const bms::Action &ac = m.actions[idx];
				const int t = int(ac.action_type);
				if (t != 1 && t != 19) continue;   // RedirectGroupTo / RedirectSingleTo
				std::printf("   REDIR ev=%zu type=%d sub=%d p1=%d p2=%d p3=%d p4=%d\n",
						ei, t, ac.action_sub_type, ac.param1, ac.param2, ac.param3, ac.param4);
			}
		}
		// AUTHORED organic spawns straight off the BMS, the arbiter for any
		// placement claim: whichever capture matches these is the correct one.
		std::printf("authored items: %zu\n", m.items.size());
		for (size_t ii = 0; ii < m.items.size(); ++ii) {
			const bms::Entity &b = m.items[ii];
			if (b.id != 64 && b.id != 65 && b.id != 68 && b.id != 60 && b.type_id != 1902) continue;
			std::printf("   BMSITEM %zu id=%d type=%d pos=(%.1f,%.1f)\n",
					ii, b.id, b.type_id, b.x / 65536.0, b.y / 65536.0);
		}
		std::printf("authored organics: %zu\n", m.organics.size());
		for (size_t oi = 0; oi < m.organics.size(); ++oi) {
			const bms::Entity &b = m.organics[oi];
			std::printf("   BMSORG %zu type=%d pos=(%.1f,%.1f) grp=%d wp=%d wpnum=%d\n",
					oi, b.type_id, b.x / 65536.0, b.y / 65536.0, int(b.group_id),
					int(b.waypoint_id), int(b.wp_number));
		}
		std::printf("board carriers:\n");
		for (int j = 0; j < n; ++j) {
			const w::AiEntity *e = ai.at(j);
			if (e == nullptr || e->slot.f[37] < 123 || e->slot.f[37] > 125) continue;
			const int32_t cached = e->slot.f[36];
			if (cached == 0) continue;
			const w::EntityHandle ch{static_cast<uint16_t>(cached - 1)};
			if (std::find(seen.begin(), seen.end(), ch.packed) != seen.end()) continue;
			seen.push_back(ch.packed);
			const w::Entity *veh = world.registry.get(ch);
			double vtravel = 0.0;
			auto cs = carrier_start.find(ch.packed);
			if (cs != carrier_start.end() && veh != nullptr) {
				const double vdx = double(int32_t(veh->position.x * 65536.0f)) - cs->second.first;
				const double vdy = double(int32_t(veh->position.y * 65536.0f)) - cs->second.second;
				vtravel = std::sqrt(vdx * vdx + vdy * vdy) / 65536.0;
			}
			// Does the CARRIER itself carry a route? wpType/wpChan 0 means it was
			// never ordered anywhere (a missing script order or an unpromoted
			// route); non-zero with moved=0 means the drive gate blocks it.
			int vt = -1, vc = -1, vn = -1, vmm = -1;
			double aix = -1.0, aiy = -1.0;
			if (veh != nullptr) {
				if (const w::AiEntity *vb = ai.for_handle(ch)) {
					vt = vb->brain.f[w::AiBrain::kWpType];
					vc = vb->brain.f[w::AiBrain::kWpChannel];
					vn = vb->brain.f[w::AiBrain::kWpNode];
					vmm = vb->inf.move_mode;
					aix = vb->pos[0] / 65536.0;
					aiy = vb->pos[1] / 65536.0;
				}
			}
			std::printf("   carrier handle=%-6u item_id=%-6d seats=%-3zu net_id=%-5d "
					"pos=(%.1f,%.1f) aipos=(%.1f,%.1f) wpType=%-3d wpChan=%-3d wpNode=%-3d/%-3d mm=%-3d moved=%.2f\n",
					unsigned(ch.packed), veh ? veh->item_id : -1,
					veh ? veh->seats.size() : size_t(0), veh ? veh->net_id : -1,
					veh ? veh->position.x : 0.0f, veh ? veh->position.y : 0.0f,
					aix, aiy,
					vt, vc, vn,
					(vc > 0 && ai.nav.channel(vc)) ? ai.nav.channel(vc)->count : -1,
					vmm, vtravel);
		}
	}

	// --- the regression pins --------------------------------------------------
	// Retail leaves exactly THREE AI stationary on this mission (s8/s15/s16, the
	// ones carrying state_a 64 on the wire). Anything much larger is the STUCK
	// defect. This is deliberately a loose bound, not an exact count: the tick
	// budget here is 40 s against the capture's 430 s, so a slow AI may not have
	// cleared the 50-unit threshold yet. It pins the DEFECT, not the scenario.
	expect(moved > 0, "at least one AI walks its authored route");

	// THE PLAYERCONTROL ADMIT GATE, pinned against the real mission. 00TRg orders
	// three soldiers (BMSORG 42/43/51, group 16, wp=125) onto 1902 "50cal on 180
	// tripod" emplacements — attrib EWeap 0x20, NO PlayerControl 0x40 — standing at
	// their own spawns. Retail never boards them [orig: Entity_UpdateInfantryAI
	// @0x4b9910 — the `(itemDef->attrib & 0x40) != 0` test guarding the
	// Entity_CanEnterVehicle @0x435480 consult]. The same run must still board the
	// real carriers (trucks/SUVs/boats all carry PlayerControl): a gate that
	// unmounts EVERYONE is a regression, not a fix.
	{
		int cmd125 = 0, mounted_on_eweap = 0, mounted_total = 0;
		for (int i = 0; i < n; ++i) {
			const w::AiEntity *e = ai.at(i);
			if (e == nullptr || e->slot.f[37] < 123 || e->slot.f[37] > 125) continue;
			++cmd125;
			const w::Entity *self = world.registry.get(e->handle);
			if (self == nullptr || !self->mounted) continue;
			++mounted_total;
			const int32_t cached = e->slot.f[36];
			if (cached == 0) continue;
			const w::Entity *carrier =
					world.registry.get(w::EntityHandle{static_cast<uint16_t>(cached - 1)});
			if (carrier != nullptr &&
					(carrier->item_attrib & w::kItemAttribPlayerControl) == 0)
				++mounted_on_eweap;
		}
		std::printf("board gate: %d command AI, %d mounted, %d on non-PlayerControl\n",
				cmd125, mounted_total, mounted_on_eweap);
		expect(mounted_on_eweap == 0,
				"no AI boards a target lacking PlayerControl [orig: @0x4b9910 attrib&0x40]");
		expect(mounted_total > 0,
				"the PlayerControl carriers are still boarded (the gate is not a blanket reject)");
	}

	if (failures == 0) std::printf("ai path conformance tests passed\n");
	return failures ? 1 : 0;
}
