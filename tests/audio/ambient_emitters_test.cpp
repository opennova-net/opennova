// opennova::audio::AmbientEmitters (runtime/audio/ambient_emitters.h): the mission's
// ambient emitter bookkeeping, moved out of the Godot mission audio (ADR 0077 R2) with
// the GUT logic pins it carried. The envs markers described through the bank chain
// (playable slots, distinct sets, stagger, the counts), the loudest eight bound with
// incumbents kept and entrants restarted [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0],
// the time-of-day slot and crossfade through the table [orig: Entity_UpdateEnvSoundEmitter
// @ 0x4a8080], one occlusion query per raw-audible marker, the entity-attached lanes
// replayed at their producer ticks [orig: SoundEmitter_Register @0x529270 ->
// SoundEmitter_RegisterSetLayers @ 0x528340] with their keyed ids retired and recycled,
// the unresolvable entrant cached out, and the world clock's latch.
#include <runtime/audio/ambient_emitters.h>
#include <runtime/world/sound_emitter_mailbox.h>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace opennova::audio;

static int failures = 0;
#define CHECK(c)                                                                       \
	do {                                                                               \
		if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
	} while (0)

namespace {

const float kOrigin[3] = { 0.0f, 0.0f, 0.0f };

// A bank chain of authored sets: each a list of layers (member 0's wave and radii).
struct FakeChain {
	std::map<std::string, std::vector<AmbientLayerRow>> sets;

	AmbientSetSource source() const {
		return AmbientSetSource{
			[this](const std::string &name) { return sets.count(name) != 0; },
			[this](const std::string &name) {
				const auto found = sets.find(name);
				return found != sets.end() ? found->second : std::vector<AmbientLayerRow>();
			},
		};
	}
};

AmbientLayerRow layer(const char *wave, int32_t falloff_u, int32_t min_u = 0) {
	AmbientLayerRow row;
	row.wave = wave;
	row.layer.falloff_u = falloff_u;
	row.layer.min_u = min_u;
	return row;
}

// One envs row at a mission position with its four slot sets.
EnvsMarker envs(float x, float y, float z, int32_t bms_id, const char *morning, const char *day,
		const char *evening, const char *night) {
	EnvsMarker row;
	row.x = x;
	row.y = y;
	row.z = z;
	row.bms_id = bms_id;
	row.slot_sets = { morning, day, evening, night };
	return row;
}

EnvsMarker envs_all(float x, const char *set, int32_t bms_id = 0) {
	return envs(x, 0.0f, 0.0f, bms_id, set, set, set, set);
}

// One entity-attached registration, its position in the mission frame.
opennova::world::SoundEmitterEvent lane_event(uint64_t source, float x, float y, float z,
		int32_t pitch_q16, uint16_t volume_q8_8, uint32_t emitted_tick, const char *set = "V_TRUCK",
		uint8_t lane = 0) {
	opennova::world::SoundEmitterEvent event;
	event.source_spawn_id = source;
	event.pos = opennova::world::Vec3{ x, y, z };
	event.emitted_tick = emitted_tick;
	event.lane = lane;
	event.lifetime_ticks = 30;
	event.pitch_q16 = pitch_q16;
	event.volume_q8_8 = volume_q8_8;
	event.set_name = set;
	return event;
}

bool always(int32_t, const AmbientCandidateRef &) {
	return true;
}

std::set<int32_t> bound(const AmbientEmitters &emitters) {
	std::set<int32_t> out;
	for (int c = 0; c < emitters.channel_count(); ++c)
		if (emitters.candidate_of(c) >= 0) out.insert(emitters.candidate_of(c));
	return out;
}

std::set<int32_t> ids(int32_t first, int32_t last) {
	std::set<int32_t> out;
	for (int32_t id = first; id <= last; ++id) out.insert(id);
	return out;
}

const AmbientChannelPlan &frame(AmbientEmitters &emitters, const float listener[3],
		float delta = 0.2f) {
	return emitters.frame(listener, delta, true, always);
}

// The envs rows become markers through the chain: a slot whose set the chain
// lacks is silent, each distinct playable set describes its layers as
// candidates (ids in walk order), a row with none registers nothing; the table
// takes them at restart. A marker plays at its presentation point.
void check_markers_describe_the_chain_s_sets() {
	FakeChain chain;
	chain.sets["AMB"] = { layer("amb.wav", 500) };
	chain.sets["NIGHT"] = { layer("night.wav", 500) };
	chain.sets["DAY2"] = { layer("day2a.wav", 500), layer("day2b.wav", 500) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.add_envs_markers({
			envs_all(10.0f, "AMB"),
			envs(20.0f, 0.0f, 0.0f, 0, "", "", "", "NIGHT"),
			envs_all(30.0f, "MISSING"),
			envs(1.0f, 2.0f, 3.0f, 7, "AMB", "DAY2", "AMB", "MISSING"),
	});
	CHECK(emitters.stats().markers_total == 4);
	CHECK(emitters.stats().markers_resolved == 3);
	CHECK(emitters.stats().candidates == 5);
	CHECK(emitters.marker_count() == 3);
	CHECK(!emitters.live() && emitters.candidate(1) == nullptr);
	emitters.restart();
	CHECK(emitters.live());
	const AmbientCandidateRef *amb = emitters.candidate(1);
	CHECK(amb != nullptr && amb->wave == "amb.wav" && amb->source == AmbientSource::Marker);
	CHECK(emitters.candidate(2) != nullptr && emitters.candidate(2)->wave == "night.wav");
	CHECK(emitters.candidate(4) != nullptr && emitters.candidate(4)->wave == "day2a.wav");
	CHECK(emitters.candidate(5) != nullptr && emitters.candidate(5)->wave == "day2b.wav");
	CHECK(emitters.candidate(6) == nullptr);
	// Noon is the day region: the fourth marker plays DAY2 at mission (1, 2, 3),
	// the presentation point (1, 3, -2).
	const float near_fourth[3] = { 1.0f, 3.0f, -2.0f };
	const AmbientChannelPlan &plan = frame(emitters, near_fourth);
	bool fourth = false;
	for (const AmbientChannelPlan::Bind &bind : plan.binds) {
		if (bind.row.candidate_id != 4) continue;
		fourth = bind.row.pos[0] == 1.0f && bind.row.pos[1] == 3.0f && bind.row.pos[2] == -2.0f &&
				bind.row.pitch_q16 == 0x10000;
	}
	CHECK(fourth);
	// The night-only marker is silent at noon.
	CHECK(bound(emitters).count(2) == 0);
}

// Twelve markers on a line, nearer = louder: the nearest eight take the eight
// channels loudest first; an unchanged mix only updates; walking along the line
// releases the dropouts and the entrants take their channels without a ninth;
// walking back restarts the dropped candidates (fresh binds) on free channels.
void check_the_loudest_eight_bind_and_incumbents_keep_their_channels() {
	FakeChain chain;
	chain.sets["AMB"] = { layer("amb.wav", 2000) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	std::vector<EnvsMarker> rows;
	for (int i = 0; i < 12; ++i) rows.push_back(envs_all(static_cast<float>(i * 100), "AMB"));
	emitters.add_envs_markers(rows);
	emitters.restart();
	CHECK(emitters.channel_count() == 0);
	const AmbientChannelPlan &first = frame(emitters, kOrigin);
	CHECK(first.channel_count == 8 && first.binds.size() == 8 && first.released.empty());
	CHECK(bound(emitters) == ids(1, 8));
	bool loudest_first = first.binds.size() == 8 && first.binds[0].row.candidate_id == 1;
	for (size_t i = 1; i < first.binds.size(); ++i)
		loudest_first = loudest_first && first.binds[i - 1].row.vol > first.binds[i].row.vol;
	CHECK(loudest_first);

	const AmbientChannelPlan &steady = frame(emitters, kOrigin);
	CHECK(steady.binds.empty() && steady.released.empty() && steady.updates.size() == 8);

	const int channel_of_five = emitters.channel_of(5);
	const float along[3] = { 1100.0f, 0.0f, 0.0f };
	const AmbientChannelPlan &moved = frame(emitters, along);
	CHECK(moved.channel_count == 8 && moved.released.size() == 4 && moved.binds.size() == 4);
	CHECK(bound(emitters) == ids(5, 12));
	CHECK(emitters.channel_of(5) == channel_of_five);

	const AmbientChannelPlan &back = frame(emitters, kOrigin);
	CHECK(back.channel_count == 8 && back.binds.size() == 4);
	CHECK(bound(emitters) == ids(1, 8));
	bool restarted = true;
	for (const AmbientChannelPlan::Bind &bind : back.binds)
		restarted = restarted && bind.row.candidate_id >= 1 && bind.row.candidate_id <= 4;
	CHECK(restarted);

	// A pinned pool (no home for a player) never grows.
	AmbientEmitters pinned;
	pinned.set_sets(chain.source());
	pinned.add_envs_markers(rows);
	pinned.restart();
	const AmbientChannelPlan &none = pinned.frame(kOrigin, 0.2f, false, always);
	CHECK(none.channel_count == 0 && none.binds.empty());
}

// A candidate at or beyond its layer's falloff takes no channel
// [orig: SoundBank_CalcDistanceVolPan @ 0x75ca20, the d >= r hard silence].
void check_beyond_the_falloff_is_silent() {
	FakeChain chain;
	chain.sets["AMB"] = { layer("amb.wav", 100) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.add_envs_markers({ envs_all(150.0f, "AMB") });
	emitters.restart();
	const AmbientChannelPlan &plan = frame(emitters, kOrigin);
	CHECK(plan.binds.empty() && plan.channel_count == 0);
}

struct OcclusionLog {
	int calls = 0;
	std::vector<int64_t> sources;
};

int64_t log_occlusion(void *ctx, const float[3], const float[3], int64_t dist_q16, int64_t source_id) {
	OcclusionLog *log = static_cast<OcclusionLog *>(ctx);
	++log->calls;
	log->sources.push_back(source_id);
	return dist_q16;
}

// Two layers of one audible marker share one two-ray result; a marker already
// silent by its raw falloff spends no query; the query carries the marker's id
// [orig: Sound_ApplyOcclusionDistance @ 0x529970, called from the mix @ 0x528659].
void check_occlusion_is_asked_once_per_raw_audible_marker() {
	FakeChain chain;
	chain.sets["PAIR"] = { layer("a.wav", 100), layer("b.wav", 100) };
	chain.sets["ONE"] = { layer("c.wav", 100) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.add_envs_markers({ envs_all(10.0f, "PAIR", 123), envs_all(150.0f, "ONE", 456) });
	emitters.restart();
	OcclusionLog log;
	const AmbientChannelPlan &plan =
			emitters.frame(kOrigin, 0.2f, true, always, &log_occlusion, &log);
	CHECK(log.calls == 1);
	CHECK(log.sources.size() == 1 && log.sources[0] == 123);
	CHECK(plan.binds.size() == 2);
}

int32_t only_volume(const AmbientChannelPlan &plan) {
	if (plan.binds.size() == 1) return plan.binds[0].row.vol;
	if (plan.updates.size() == 1) return plan.updates[0].row.vol;
	return -1;
}

// The region's slot picks the set: a night-only marker is silent at noon and
// plays at 23:00; a slot entering at 10:01 fades in through the region's
// crossfade [orig: @ 0x408203], unless its neighbour region names the same set
// [orig: @ 0x4a819d], which keeps full volume through the boundary.
void check_the_time_of_day_slot_and_its_crossfade() {
	FakeChain chain;
	chain.sets["NIGHT"] = { layer("night.wav", 500) };
	chain.sets["DAY"] = { layer("day.wav", 500) };
	chain.sets["AMB"] = { layer("amb.wav", 500) };

	AmbientEmitters night;
	night.set_sets(chain.source());
	night.add_envs_markers({ envs(10.0f, 0.0f, 0.0f, 0, "", "", "", "NIGHT") });
	night.restart();
	night.set_time_of_day_hhmm(1200.0);
	CHECK(frame(night, kOrigin).binds.empty());
	night.set_time_of_day_hhmm(2300.0);
	CHECK(frame(night, kOrigin).binds.size() == 1);

	AmbientEmitters day;
	day.set_sets(chain.source());
	day.add_envs_markers({ envs(10.0f, 0.0f, 0.0f, 0, "", "DAY", "", "") });
	day.restart();
	day.set_time_of_day_hhmm(1200.0);
	const int32_t full = only_volume(frame(day, kOrigin));
	CHECK(full > 0);
	day.set_time_of_day_hhmm(1001.0);
	const int32_t fading = only_volume(frame(day, kOrigin));
	CHECK(fading > 0 && fading < full);

	AmbientEmitters allday;
	allday.set_sets(chain.source());
	allday.add_envs_markers({ envs_all(10.0f, "AMB") });
	allday.restart();
	allday.set_time_of_day_hhmm(1200.0);
	const int32_t steady = only_volume(frame(allday, kOrigin));
	allday.set_time_of_day_hhmm(1001.0);
	CHECK(steady > 0 && only_volume(frame(allday, kOrigin)) == steady);
}

// An entity-attached lane competes with placed ambience in the one loudest-eight
// set: at full gain it displaces the quietest marker; a keyed refresh updates
// its channel in place (its new pitch word and volume); a zero pitch/volume
// registration clears it at once and the displaced marker takes the channel.
void check_lanes_share_the_eight_refresh_in_place_and_clear_by_key() {
	FakeChain chain;
	chain.sets["AMB"] = { layer("amb.wav", 2000) };
	chain.sets["V_TRUCK"] = { layer("truck.wav", 2000) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	// The truck at presentation (900, 4, -300) = mission (900, 300, 4).
	const float truck[3] = { 900.0f, 4.0f, -300.0f };
	std::vector<EnvsMarker> rows;
	for (int i = 0; i < 8; ++i) {
		const float offset = i == 7 ? 1500.0f : static_cast<float>(20 + i * 20);
		rows.push_back(envs(900.0f + offset, 300.0f, 4.0f, 0, "AMB", "AMB", "AMB", "AMB"));
	}
	emitters.add_envs_markers(rows);
	emitters.restart();
	emitters.queue_emitters({ lane_event(77, 900.0f, 300.0f, 4.0f, 0x10000, 0xFFFF, 0) });
	const AmbientChannelPlan &joined = frame(emitters, truck);
	CHECK(joined.channel_count == 8);
	int32_t lane_id = -1;
	for (const AmbientChannelPlan::Bind &bind : joined.binds) {
		const AmbientCandidateRef *candidate = emitters.candidate(bind.row.candidate_id);
		if (candidate == nullptr || candidate->source != AmbientSource::Emitter) continue;
		lane_id = bind.row.candidate_id;
		CHECK(candidate->wave == "truck.wav");
		CHECK(bind.row.vol == 252 && bind.row.pitch_q16 == 0x10000);
		CHECK(bind.row.pos[0] == 900.0f && bind.row.pos[1] == 4.0f && bind.row.pos[2] == -300.0f);
	}
	CHECK(lane_id > 8);
	CHECK(emitters.channel_of(8) < 0); // the far marker lost its channel
	const int lane_channel = emitters.channel_of(lane_id);

	emitters.queue_emitters({ lane_event(77, 901.0f, 300.0f, 4.0f, 0xC000, 0x8000, 0) });
	const float refreshed[3] = { 901.0f, 4.0f, -300.0f };
	const AmbientChannelPlan &refresh = frame(emitters, refreshed);
	CHECK(refresh.binds.empty() && refresh.released.empty());
	CHECK(emitters.channel_of(lane_id) == lane_channel);
	bool updated = false;
	for (const AmbientChannelPlan::Bind &update : refresh.updates) {
		if (update.row.candidate_id != lane_id) continue;
		updated = update.row.vol == 125 && update.row.pitch_q16 == 0xC000 &&
				update.row.pos[0] == 901.0f;
	}
	CHECK(updated);

	emitters.queue_emitters({ lane_event(77, 901.0f, 300.0f, 4.0f, 0, 0, 0) });
	const AmbientChannelPlan &cleared = frame(emitters, refreshed);
	CHECK(emitters.channel_of(lane_id) < 0 && emitters.candidate(lane_id) == nullptr);
	CHECK(cleared.channel_count == 8 && bound(emitters) == ids(1, 8));
}

// One render frame catching up 32 world ticks replays a registration at its
// producer tick (1), so its 30-tick keep-alive is spent: the slot is serviced
// once on the tick it reaches zero, the next mix releases it, and only then is
// its id issued again, to the next lane.
void check_a_catch_up_batch_replays_at_the_producer_tick_and_ids_recycle() {
	FakeChain chain;
	chain.sets["V_TRUCK"] = { layer("truck.wav", 2000) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.restart();
	const float near_truck[3] = { 0.0f, 0.0f, 0.0f };
	emitters.queue_emitters({ lane_event(77, 40.0f, 0.0f, 0.0f, 0x10000, 0xFFFF, 1) });
	emitters.advance_to_logic_tick(32);
	CHECK(emitters.clock_tick() == 32);
	frame(emitters, near_truck, 0.0f);
	const std::set<int32_t> expiring = bound(emitters);
	CHECK(expiring.size() == 1);
	const int32_t first_id = expiring.empty() ? -1 : *expiring.begin();

	frame(emitters, near_truck, 0.0f);
	CHECK(emitters.channel_of(first_id) < 0 && bound(emitters).empty());

	emitters.queue_emitters({ lane_event(88, 80.0f, 0.0f, 0.0f, 0x10000, 0xFFFF, 33) });
	emitters.advance_to_logic_tick(33);
	frame(emitters, near_truck, 0.0f);
	CHECK(emitters.channel_of(first_id) >= 0);
	const AmbientCandidateRef *reissued = emitters.candidate(first_id);
	CHECK(reissued != nullptr && reissued->source == AmbientSource::Emitter);
}

// A lane that changes its set retires the old ids (fresh ones for the new
// layers); a source-only row moves the lane without refreshing it.
void check_a_set_change_retires_the_lane_s_ids_and_a_source_row_moves_it() {
	FakeChain chain;
	chain.sets["IDLE"] = { layer("idle.wav", 2000) };
	chain.sets["REV"] = { layer("reva.wav", 2000), layer("revb.wav", 2000) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.restart();
	emitters.queue_emitters({ lane_event(5, 10.0f, 0.0f, 0.0f, 0x10000, 0xFFFF, 0, "IDLE") });
	frame(emitters, kOrigin);
	const std::set<int32_t> idle = bound(emitters);
	CHECK(idle.size() == 1);
	const int32_t idle_id = idle.empty() ? -1 : *idle.begin();

	emitters.queue_emitters({ lane_event(5, 10.0f, 0.0f, 0.0f, 0x10000, 0xFFFF, 0, "REV") });
	frame(emitters, kOrigin);
	const std::set<int32_t> rev = bound(emitters);
	CHECK(rev.size() == 2 && rev.count(idle_id) == 0);
	CHECK(emitters.candidate(idle_id) == nullptr);

	opennova::world::SoundEmitterEvent anchor = lane_event(5, 20.0f, 0.0f, 0.0f, 0, 0, 0, "");
	anchor.source_only = true;
	emitters.queue_emitters({ anchor });
	const AmbientChannelPlan &moved = frame(emitters, kOrigin);
	bool all_moved = moved.updates.size() == 2;
	for (const AmbientChannelPlan::Bind &update : moved.updates)
		all_moved = all_moved && update.row.pos[0] == 20.0f;
	CHECK(all_moved);
}

// An entrant whose wave the device cannot play is cached out (asked once) and
// the next-ranked candidate takes the channel; the counts say so.
void check_an_unresolvable_entrant_falls_through_to_the_next() {
	FakeChain chain;
	chain.sets["BAD"] = { layer("bad.wav", 2000) };
	chain.sets["GOOD"] = { layer("good.wav", 2000) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.add_envs_markers({ envs_all(1.0f, "BAD"), envs_all(10.0f, "GOOD") });
	emitters.restart();
	CHECK(emitters.stats().candidates == 2 && emitters.stats().candidates_validated == 0);
	int asked = 0;
	const auto playable = [&](int32_t, const AmbientCandidateRef &candidate) {
		++asked;
		return candidate.wave != "bad.wav";
	};
	const AmbientChannelPlan &first = emitters.frame(kOrigin, 0.2f, true, playable);
	CHECK(first.binds.size() == 1 && first.binds[0].row.candidate_id == 2);
	CHECK(emitters.stats().candidates_validated == 2 && emitters.stats().decode_failures == 1);
	CHECK(emitters.failed_count() == 1 && asked == 2);
	const AmbientChannelPlan &second = emitters.frame(kOrigin, 0.2f, true, playable);
	CHECK(second.binds.empty() && second.updates.size() == 1 && asked == 2);
}

// An owner the world never ticks free-runs the wall clock; the first world tick
// latches the table onto the world's clock at an offset that keeps the elapsed
// ticks, after which a frame's delta no longer moves it; a restart unlatches.
void check_the_world_clock_latches_with_an_offset() {
	FakeChain chain;
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.advance_to_logic_tick(10);
	CHECK(emitters.clock_tick() == 0); // not live: nothing runs
	emitters.restart();
	emitters.frame(kOrigin, 1.0f, true, always);
	CHECK(emitters.clock_tick() == 62);
	emitters.advance_to_logic_tick(10);
	CHECK(emitters.clock_tick() == 62);
	emitters.advance_to_logic_tick(11);
	CHECK(emitters.clock_tick() == 63);
	emitters.frame(kOrigin, 1.0f, true, always);
	CHECK(emitters.clock_tick() == 63);
	emitters.restart();
	CHECK(emitters.clock_tick() == 0);
	emitters.frame(kOrigin, 1.0f, true, always);
	CHECK(emitters.clock_tick() == 62);
}

// clear() is back to cold: nothing live, no channels, ids from 1, counts zero.
void check_clear_is_cold() {
	FakeChain chain;
	chain.sets["AMB"] = { layer("amb.wav", 2000) };
	AmbientEmitters emitters;
	emitters.set_sets(chain.source());
	emitters.add_envs_markers({ envs_all(10.0f, "AMB") });
	emitters.restart();
	frame(emitters, kOrigin);
	CHECK(emitters.channel_count() == 1);
	emitters.clear();
	CHECK(!emitters.live() && emitters.channel_count() == 0 && emitters.marker_count() == 0);
	CHECK(emitters.stats().markers_total == 0 && emitters.candidate(1) == nullptr);
	emitters.queue_emitters({ lane_event(1, 0.0f, 0.0f, 0.0f, 0x10000, 0xFFFF, 0, "AMB") });
	emitters.add_envs_markers({ envs_all(10.0f, "AMB") });
	emitters.restart();
	CHECK(emitters.candidate(1) != nullptr && frame(emitters, kOrigin).binds.size() == 1);
}

} // namespace

int main() {
	check_markers_describe_the_chain_s_sets();
	check_the_loudest_eight_bind_and_incumbents_keep_their_channels();
	check_beyond_the_falloff_is_silent();
	check_occlusion_is_asked_once_per_raw_audible_marker();
	check_the_time_of_day_slot_and_its_crossfade();
	check_lanes_share_the_eight_refresh_in_place_and_clear_by_key();
	check_a_catch_up_batch_replays_at_the_producer_tick_and_ids_recycle();
	check_a_set_change_retires_the_lane_s_ids_and_a_source_row_moves_it();
	check_an_unresolvable_entrant_falls_through_to_the_next();
	check_the_world_clock_latches_with_an_offset();
	check_clear_is_cold();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("ambient_emitters: ok\n");
	return 0;
}
