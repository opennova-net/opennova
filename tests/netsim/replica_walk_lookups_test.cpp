// D-NET-NEW-LF-26 (LF-s17 row 1, LP-s27 B): a joiner's parked replica rows are found at their
// own entries, not through a lookup built over every decoded row. Retail's pool-0 walk reaches
// each slot from the pool base by its stride, and the pool-1 walk visits each slot where it
// stands; ours parks each row with its index (re-checked against its handle at its visit, a
// row the first half erased moving the rest) and each pool-1 row by its slot. The visits, their
// order and the rows they reach are the ones the per-call lookups gave.
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9, the pool
//  base and stride @0x4C2426..0x4C2431, the +0x1C4 call @0x4C2472]

#include <runtime/replication/client_replica_pipeline.h>

#include <net/npwire/entity_class.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

namespace {

namespace nw = opennova;
namespace ns = opennova::replication;

constexpr uint16_t kSelf = 0x0005;
constexpr uint16_t kHullType = 0x0248;
constexpr uint16_t kPlayerType = 0x14B9;

int failures = 0;
bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	++failures;
	return false;
}

struct View {
	ns::ClientReplicaPipeline view;
	View(const std::vector<uint16_t> &pool1, const std::vector<uint16_t> &pool0) {
		view.set_remote_motion_mode(true);
		view.set_self_body_split(true);
		view.set_pool1_interleave(true);
		view.set_item_class_resolver([](uint16_t type)
				-> ns::ClientReplicaPipeline::ItemClassResolution {
			if (type == kHullType) return nw::EntityClass::Vehicle;
			if (type == kPlayerType) return nw::EntityClass::Player;
			return std::nullopt;
		});
		for (const uint16_t handle : pool1) {
			ns::ClientEntityState &row = view.state().upsert(handle);
			row.type_id = kHullType;
			row.cls = nw::EntityClass::Vehicle;
		}
		for (const uint16_t handle : pool0) {
			ns::ClientEntityState &row = view.state().upsert(handle);
			row.type_id = kPlayerType;
			row.cls = nw::EntityClass::Player;
		}
	}
	const ns::ClientEntityState *row(uint16_t handle) { return view.state().find(handle); }
	bool parked(uint16_t handle) {
		const ns::ClientEntityState *r = row(handle);
		return r != nullptr && r->rm_parked;
	}
	void erase(uint16_t handle) {
		auto &rows = view.state().entities;
		rows.erase(std::remove_if(rows.begin(), rows.end(),
				[handle](const ns::ClientEntityState &r) { return r.handle == handle; }),
				rows.end());
	}
};

// The pool-1 rows park for their slots; a slot's visit takes its row once, a row past an
// erased one included; the unreached rows list in walk order and take their visits in the
// middle leg, an erased row's slot visiting nothing.
void test_pool1_rows_take_their_visits_at_their_slots() {
	auto v = std::make_unique<View>(
			std::vector<uint16_t>{0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006},
			std::vector<uint16_t>{kSelf});
	v->view.tick_remote_motion(kSelf);
	bool all = true;
	for (uint16_t h = 0x1001; h <= 0x1006; ++h) all = all && v->parked(h);
	expect(all, "the walk parks every pool-1 row for its slot");
	expect(v->view.unvisited_pool1_rows() ==
					std::vector<uint16_t>({0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006}),
			"every parked row is unvisited, in walk order");
	ns::ClientEntityState *visited = v->view.visit_pool1_row(0x1003);
	expect(visited != nullptr && visited->handle == 0x1003 && !visited->rm_parked,
			"the slot's visit takes its row");
	expect(v->view.visit_pool1_row(0x1003) == nullptr, "a second visit this tick takes nothing");
	expect(v->view.visit_pool1_row(kSelf) == nullptr, "a pool-0 handle is no pool-1 slot");
	expect(v->view.visit_pool1_row(0x100F) == nullptr, "a slot the walk never parked");
	// The first row leaves the vector: every row past it moves down one entry.
	v->erase(0x1001);
	visited = v->view.visit_pool1_row(0x1004);
	expect(visited != nullptr && visited->handle == 0x1004 && !visited->rm_parked,
			"a row that moved since it parked is found by its handle");
	expect(v->view.unvisited_pool1_rows() ==
					std::vector<uint16_t>({0x1001, 0x1002, 0x1005, 0x1006}),
			"the unreached rows, in walk order (the erased row's slot still parked)");
	v->view.tick_remote_motion_through_self();
	expect(!v->parked(0x1002) && !v->parked(0x1005) && !v->parked(0x1006),
			"the middle leg visits every unreached row");
	expect(v->view.unvisited_pool1_rows().empty(), "nothing is left parked");
	expect(v->view.visit_pool1_row(0x1002) == nullptr, "a finished slot visits nothing");
	v->view.tick_remote_motion_after_self();
}

// The parked pool-0 rows: the one below the own slot visits in the middle leg, the ones past
// it in the after-self leg, each found at its entry though a row ahead of them left the
// vector after they parked.
void test_parked_pool0_rows_visit_at_their_halves() {
	auto v = std::make_unique<View>(std::vector<uint16_t>{0x1001, 0x1002},
			std::vector<uint16_t>{0x0002, kSelf, 0x0007, 0x0009});
	v->view.tick_remote_motion(kSelf);
	expect(v->parked(0x0002) && v->parked(0x0007) && v->parked(0x0009),
			"the walk parks the pool-0 rows");
	v->erase(0x1001);
	v->view.tick_remote_motion_through_self();
	expect(!v->parked(0x0002), "the row below the own slot visits in the middle leg");
	expect(v->parked(0x0007) && v->parked(0x0009), "the rows past it still wait");
	v->erase(0x1002);
	v->view.tick_remote_motion_after_self();
	expect(!v->parked(0x0007) && !v->parked(0x0009),
			"the rows past the own slot visit in the after-self leg, found though they moved");
}

// ClientState::find's per-handle row (D-NET-NEW-LF-26, LF-s18): a lookup returns the row the
// walk over the decoded rows finds, through every edit a caller makes to the public rows: an
// erase ahead of it moves it, a clear empties it, a re-add puts it back elsewhere, and a copy of
// the state finds its own rows.
void test_find_takes_the_row_holding_the_handle_through_every_edit() {
	auto state = std::make_unique<ns::ClientState>();
	for (const uint16_t handle : {0x1001, 0x1002, 0x0003, 0x2004})
		state->upsert(handle).type_id = static_cast<uint16_t>(handle + 1);
	const auto walk = [&state](uint16_t handle) -> const ns::ClientEntityState * {
		for (const ns::ClientEntityState &row : state->entities)
			if (row.handle == handle) return &row;
		return nullptr;
	};
	for (const uint16_t handle : {0x1001, 0x1002, 0x0003, 0x2004, 0x0042})
		expect(state->find(handle) == walk(handle), "a lookup returns the walk's row");
	// An erase ahead of a looked-up row moves it: the lookup follows.
	state->entities.erase(state->entities.begin());
	expect(state->find(0x1001) == nullptr, "an erased row is not found");
	expect(state->find(0x0003) == walk(0x0003) && state->find(0x0003)->type_id == 0x0004,
			"a row that moved is found where it now stands");
	// A swap of two rows' places (a caller reordering the public rows).
	std::swap(state->entities[0], state->entities[2]);
	expect(state->find(0x1002) == walk(0x1002) && state->find(0x2004) == walk(0x2004),
			"reordered rows are found at their new places");
	// A clear, then the handle again at another place.
	state->entities.clear();
	expect(state->find(0x0003) == nullptr, "a cleared state finds nothing");
	state->upsert(0x0042);
	state->upsert(0x0003).type_id = 0x0099;
	expect(state->find(0x0003) == &state->entities[1] && state->find(0x0003)->type_id == 0x0099,
			"a re-added handle is found at its new row");
	// A copy finds its own rows, never the original's.
	const auto copy = std::make_unique<ns::ClientState>(*state);
	expect(copy->find(0x0003) == &copy->entities[1], "a copy's lookup reads the copy's rows");
	const ns::ClientState &const_copy = *copy;
	expect(const_copy.find(0x0042) == &copy->entities[0], "the const lookup agrees");
}

} // namespace

int main() {
	test_pool1_rows_take_their_visits_at_their_slots();
	test_parked_pool0_rows_visit_at_their_halves();
	test_find_takes_the_row_holding_the_handle_through_every_edit();
	if (failures != 0) {
		std::printf("%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("replica_walk_lookups: ok\n");
	return 0;
}
