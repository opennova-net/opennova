// The mission view's Shoot tool (ADR 0046 DI-23): MissionViewport's half that fires shots and follows their run
// (preview/mission_shots), beside mission_viewport.cpp.
#include <editor/preview/mission_viewport.h>

#include <algorithm>

#include <base/io/hash.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/display_names.h>
#include <editor/model/document.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/sound_preview.h>
#include <editor/preview/viewport_device.h>
#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <runtime/world/presentation_frame.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

particle::Vec3 presented(const double at[3]) {
	const PreviewVec3 p = mission_to_preview(at);
	return particle::Vec3{p.x, p.y, p.z};
}

particle::Vec3 presented_direction(const world::Vec3 &direction) {
	const float m[3] = {direction.x, direction.y, direction.z};
	float p[3];
	world::presentation_from_mission(m, p);
	return particle::Vec3{p[0], p[1], p[2]};
}

// A record by the project's names (the display names: an entity by its item's name and SSN).
std::string record_title(const SessionView &view, const Document &document, NodeId row) {
	const NodeAddress record = document.address_of(row);
	const AssetGraph *graph = view.findings.graph.get();
	if (!graph) return record_display(document, record, nullptr);
	const GraphNameSource names(*graph);
	return record_display(document, record, &names);
}

uint64_t mix(uint64_t hash, uint64_t value) {
	return io::fnv1a64_bytes(hash, &value, sizeof(value));
}

} // namespace

bool MissionViewport::follow_shots_(const ViewportInput &input, const Document &document, PreviewClock &clock) {
	const SessionView &view = input.view;
	if (shots_.shots().empty()) {
		// No shot: nothing runs, nothing is drawn of one.
		const bool had = !shots_.spawns().empty() || shots_.tick() != 0 || shots_.scar_count() > 0;
		if (had) {
			shots_.clear();
			shot_cursor_ = -1;
			// The next shot configures the run afresh (clear forgot what it fires in).
			shots_key_ = 0;
		}
		return had;
	}
	// The world the shots fire in is the mission as it stands: built again where the scene, its ground or the items'
	// bounds moved, or a file it read moved (never under a drag, whose end builds it).
	follow_ground_(view);
	uint64_t key = io::kFnv1a64Offset;
	key = mix(key, scene_.serial());
	key = mix(key, uint64_t(terrain_ground_.reads()));
	key = mix(key, bounds_graph_);
	if (!gesture_open_ && (key != shots_key_ || shots_.files_moved())) {
		MissionShotsSetup setup;
		setup.files = view.findings.assets;
		setup.key = key;
		if (const auto *mission = dynamic_cast<const MissionDocument *>(&document)) {
			auto file = std::make_shared<bms::File>();
			if (mission->compose(*file)) setup.mission = std::move(file);
		}
		setup.ground = &terrain_ground_;
		setup.entities = scene_.entities();
		setup.bounds = items_.radii();
		for (const MissionEntityMark &mark : scene_.entities())
			if (mark.pool != MissionPool::Marker) setup.titles[mark.row] = record_title(view, document, mark.row);
		shots_.configure(std::move(setup));
		shots_key_ = key;
	}
	const uint64_t before = shots_.serial();
	shots_.run_to(clock.ticks());
	// Its sounds play from the project's banks, read as the game reads them (DI-04's sources).
	shot_sources_.refresh(*view.findings.assets,
	                      view.project.document ? view.project.document->expansion.name : std::string(), PreviewRig());
	return shots_.serial() != before;
}

std::vector<DefinitionSpawn> MissionViewport::shot_spawns_() const {
	// Each at its place as the game's descriptor poses it (an object's impact along the round's flight, the
	// terrain's and the water's with none), in the presentation frame the effect scene runs in.
	std::vector<DefinitionSpawn> spawns;
	for (const MissionShotSpawn &spawn : shots_.spawns())
		spawns.push_back({spawn.effect, effect_descriptor_pose(presented(spawn.at), presented_direction(spawn.direction)),
		                  spawn.tick, spawn.source, std::string()});
	return spawns;
}

renderer::ScarDrawList MissionViewport::shot_scars() const {
	const renderer::ScarDrawList list = shots_.scars();
	// Every ring in the presentation frame: the shared ring's slots as they stand, each entity ring's through its
	// owner's section matrix (its world form), the device drawing them where they are.
	renderer::ScarDrawList out;
	out.slots_live = list.slots_live;
	out.slots_culled = list.slots_culled;
	for (const renderer::ScarDrawBatch &batch : list.batches) {
		const std::vector<renderer::ScarVertex> *from = &list.vertices;
		uint32_t first = batch.first_vertex;
		if (batch.entity_local) {
			if (!batch.world_resolved) continue;
			from = &list.world_vertices;
			first = batch.world_first_vertex;
		}
		renderer::ScarDrawBatch drawn = batch;
		drawn.entity_local = false;
		drawn.world_resolved = false;
		drawn.first_vertex = uint32_t(out.vertices.size());
		for (uint32_t i = first; i < first + batch.vertex_count && i < from->size(); ++i) {
			renderer::ScarVertex vertex = (*from)[i];
			const double at[3] = {vertex.x, vertex.y, vertex.z};
			const PreviewVec3 p = mission_to_preview(at);
			vertex.x = p.x;
			vertex.y = p.y;
			vertex.z = p.z;
			out.vertices.push_back(vertex);
		}
		drawn.vertex_count = uint32_t(out.vertices.size()) - drawn.first_vertex;
		out.batches.push_back(drawn);
	}
	return out;
}

std::vector<ClipSoundFired> MissionViewport::fire_sounds(const PreviewClock &clock, const AssetScan *scan,
		audio::SoundSelector &selector, uint64_t &next_seq) {
	std::vector<ClipSoundFired> out;
	// What the run reached (the follow runs it to the clock): its sounds on the ticks [from, now), none over a seek
	// (from where a seek put the clock, PreviewClock::heard_from).
	const int32_t now = std::min(clock.ticks(), shots_.tick());
	const int32_t from = clock.heard_from(shot_cursor_, shot_seeks_);
	shot_cursor_ = now;
	if (shots_.shots().empty() || from < 0 || now <= from || now - from > kClipSoundCatchUpTicks) return out;
	const PreviewVec3 listener = camera_.eye();
	for (const MissionShotEvent &event : shots_.events()) {
		if (event.kind != MissionShotEvent::Kind::Sound || event.tick < from || event.tick >= now || event.set.empty())
			continue;
		// Where it plays, heard at the camera: a 3D one-shot at its distance [orig: Sound_Play3DPositional @0x527CB0].
		PreviewHearing heard;
		const PreviewVec3 at = mission_to_preview(event.at);
		heard.source[0] = at.x;
		heard.source[1] = at.y;
		heard.source[2] = at.z;
		heard.listener[0] = listener.x;
		heard.listener[1] = listener.y;
		heard.listener[2] = listener.z;
		const PreviewPlay play = plan_set_play(shot_sources_.banks(), shot_sources_.expansion(), event.set, std::string(),
		                                       selector, kClipSoundListenerView, &heard);
		ClipSoundFired fired;
		fired.tick = event.tick;
		fired.slot = -1;
		fired.set = event.set;
		fired.bank = play.bank;
		fired.words = "Tick " + std::to_string(event.tick) + " (" + event.words + "): " + play.words;
		fired.state = !play.found ? "missing" : !play.in_range ? "out_of_range" : play.voices.empty() ? "silent" : "played";
		for (const PreviewVoice &voice : play.voices)
			fired.voices.push_back({voice.wave, voice.file, std::string(), voice.pitch_q16, voice.volume});
		fired.seq = ++next_seq;
		fired.path = path();
		if (scan) find_clip_sound_waves(fired, *scan);
		shot_fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	if (shot_fired_.size() > kSoundsFiredKept) shot_fired_.erase(shot_fired_.begin(), shot_fired_.end() - kSoundsFiredKept);
	return out;
}

bool MissionViewport::shoot_(const ViewportContext &context, const ViewportCommand &command, CanvasRequests &out,
		std::string &error) const {
	if (!command.has_at || !command.ids.empty() || !command.by.empty()) {
		error = "shoot takes at [x, y], the picture's point the shot is fired at, and no ids or by.";
		return false;
	}
	if (reason_ != MissionViewStatus::Ready || !current(context.input)) {
		error = "The viewport shows no mission as it is.";
		return false;
	}
	if (options_.ammo.empty()) {
		error = "Pick the ammo Shoot fires (options.ammo, an ammo.def record by name).";
		return false;
	}
	// Where the point meets the ground or an object, as the ground readout finds it (DI-07); else the ground's plane.
	const MissionGroundFacts facts = ground_under(context, command.at_x, command.at_y);
	double at[3] = {facts.at[0], facts.at[1], facts.at[2]};
	if (facts.on == MissionGroundOn::Nothing && !ground_of_(context, command.at_x, command.at_y, at)) {
		error = "The point meets neither the ground nor an object.";
		return false;
	}
	MissionShot shot;
	shot.ammo = options_.ammo;
	for (int i = 0; i < 3; ++i) shot.at[i] = at[i];
	preview_to_mission(camera_.eye(), shot.eye);
	JsonValue change = JsonValue::make_object();
	change.set("kind", io::json_string(viewport_kind_token(ViewportKind::Mission)));
	JsonValue fired = mission_shot_to_json(shot);
	fired.object.erase(std::remove_if(fired.object.begin(), fired.object.end(),
	                                  [](const io::JsonMember &member) { return member.key == "tick"; }),
	                   fired.object.end());
	change.set("shot", std::move(fired));
	JsonValue clock = JsonValue::make_object();
	clock.set("playing", JsonValue::make_bool(true));
	change.set("clock", std::move(clock));
	out.request(request::set_viewport(path(), io::json_write(change)));
	return true;
}

} // namespace opennova::editor
