#include <editor/preview/effect_viewport.h>

#include <algorithm>
#include <cmath>
#include <set>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/particle_type.h>
#include <editor/model/text_document.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view/view_events.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The size its device draws at where no canvas sizes it (a headless Shell's): the model's.
constexpr ViewportState kHeadlessSize{800, 600};
// The mission header's wind as the effect system reads it: a speed and a heading in degrees.
constexpr int kMostWindSpeed = 1000;

// A new viewport's camera: the spawn point a metre below its target, a few metres off, from a little above.
OrbitCamera default_camera() {
	OrbitCamera camera;
	camera.target = PreviewVec3{0.0f, 1.0f, 0.0f};
	camera.yaw = 0.6f;
	camera.pitch = 0.3f;
	camera.distance = 8.0f;
	camera.near_plane = 0.05f;
	camera.far_plane = 2000.0f;
	return camera;
}

JsonValue camera_to_json(const OrbitCamera &camera) {
	return orbit_camera_to_json(camera);
}

// A SetViewport's options over `held`: {effect, loop, wind_speed, wind_direction, grid}, each optional;
// `effects` the file's (an effect must be one of them).
bool read_options(const JsonValue &json, const std::vector<EffectViewportEffect> &effects, EffectViewportOptions &held,
		std::string &error) {
	if (!json.is_object()) {
		error = "options is an object {effect, loop, wind_speed, wind_direction, grid}.";
		return false;
	}
	EffectViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "effect") {
			if (!value.is_string()) {
				error = "options.effect is the id of an effect the file defines (\"\" its first).";
				return false;
			}
			const auto defined = std::find_if(effects.begin(), effects.end(),
					[&](const EffectViewportEffect &effect) { return strutil::iequals(effect.id, value.string); });
			if (!value.string.empty() && !effects.empty() && defined == effects.end()) {
				std::string ids;
				for (size_t i = 0; i < effects.size() && i < 8; ++i) ids += (i ? ", " : "") + effects[i].id;
				error = "The file defines no effect \"" + value.string + "\" (" + ids + (effects.size() > 8 ? ", ..." : "") +
				        ").";
				return false;
			}
			options.effect = defined != effects.end() ? defined->id : value.string;
		} else if (key == "loop" || key == "grid") {
			if (!value.is_bool()) {
				error = "options." + key + " is true or false.";
				return false;
			}
			(key == "loop" ? options.play.loop : options.grid) = value.boolean;
		} else if (key == "wind_speed" || key == "wind_direction") {
			int64_t whole = 0;
			const bool speed = key == "wind_speed";
			if (!io::json_whole_in(value, speed ? 0 : -360, speed ? kMostWindSpeed : 360, whole)) {
				error = speed ? "options.wind_speed is the mission header's wind speed, a whole number from 0 (none) to 1000."
				              : "options.wind_direction is the mission header's wind heading, whole degrees from -360 to 360.";
				return false;
			}
			(speed ? options.play.wind_speed : options.play.wind_direction) = int(whole);
		} else {
			error = "Unknown effect option \"" + key + "\" (it takes effect, loop, wind_speed, wind_direction, grid).";
			return false;
		}
	}
	held = options;
	return true;
}

// The camera a `camera` member sets over `held` (read_orbit_camera); `frame` the camera on the live particles.
bool read_camera(const JsonValue &json, OrbitCamera &held, bool &frame, std::string &error) {
	return read_orbit_camera(json, held, frame, error);
}

const char *spawn_status_token(particle::EffectSpawnStatus status) {
	switch (status) {
	case particle::EffectSpawnStatus::Spawned: return "spawned";
	case particle::EffectSpawnStatus::Suppressed: return "suppressed";
	case particle::EffectSpawnStatus::InvalidHandle: return "invalid_handle";
	case particle::EffectSpawnStatus::EmptyEffect: return "empty_effect";
	case particle::EffectSpawnStatus::MissingSlot: return "missing_slot";
	case particle::EffectSpawnStatus::MissingOwner: return "missing_owner";
	case particle::EffectSpawnStatus::GroupCapacityReached: return "group_capacity_reached";
	case particle::EffectSpawnStatus::EmitterCapacityReached: return "emitter_capacity_reached";
	case particle::EffectSpawnStatus::Disabled: return "disabled";
	}
	return "invalid_handle";
}

} // namespace

const char *effect_view_status_token(EffectViewStatus status) {
	switch (status) {
	case EffectViewStatus::NoProject: return "no_project";
	case EffectViewStatus::NoFile: return "no_file";
	case EffectViewStatus::Unreadable: return "unreadable";
	case EffectViewStatus::NoEffect: return "no_effect";
	case EffectViewStatus::SpawnsNothing: return "spawns_nothing";
	case EffectViewStatus::Ready: return "ready";
	}
	return "no_project";
}

io::JsonValue effect_options_to_json(const EffectViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("effect", json_string(options.effect));
	out.set("loop", JsonValue::make_bool(options.play.loop));
	out.set("wind_speed", json_number(options.play.wind_speed));
	out.set("wind_direction", json_number(options.play.wind_direction));
	out.set("grid", JsonValue::make_bool(options.grid));
	return out;
}

std::string effect_options_change(const EffectViewportOptions &options) {
	return viewport_change(ViewportKind::Effect, "options", effect_options_to_json(options));
}

std::string effect_camera_change(const OrbitCamera &camera) {
	return viewport_change(ViewportKind::Effect, "camera", camera_to_json(camera));
}

EffectViewport::EffectViewport(std::string path) : ViewportModel(ViewportKind::Effect, std::move(path), kHeadlessSize) {
	camera_ = default_camera();
}

std::unique_ptr<ViewportModel> EffectViewport::make(const std::string &path) {
	return std::make_unique<EffectViewport>(path);
}

ViewportStatus EffectViewport::status() const {
	switch (reason_) {
	case EffectViewStatus::Ready: return ViewportStatus::Ready;
	case EffectViewStatus::Unreadable:
	case EffectViewStatus::SpawnsNothing: return ViewportStatus::Failed;
	case EffectViewStatus::NoProject:
	case EffectViewStatus::NoFile:
	case EffectViewStatus::NoEffect: break;
	}
	return ViewportStatus::Empty;
}

std::string EffectViewport::message() const {
	switch (reason_) {
	case EffectViewStatus::NoProject: return "Open a project to preview its particle effects.";
	case EffectViewStatus::NoFile: return "Open a particle file to preview its effects.";
	case EffectViewStatus::Unreadable: return "The game's particle reader does not read this file: " + detail_;
	case EffectViewStatus::NoEffect: return "The file defines no effect: an [effectdef] block with an id names one.";
	case EffectViewStatus::SpawnsNothing: return detail_;
	case EffectViewStatus::Ready: break;
	}
	return std::string();
}

std::string EffectViewport::caption() const {
	if (shown_.empty()) return std::string();
	std::string out = " - " + shown_;
	if (closure_.found && !closure_.source.empty() && closure_.source != path())
		out += " (the game spawns " + closure_.source + "'s)";
	return out;
}

ViewportAction EffectViewport::stop_(EffectViewStatus reason, const std::string &detail) {
	reason_ = reason;
	detail_ = detail;
	const bool held = playback_.scene() != nullptr || scene_handed_;
	playback_.close();
	closure_ = particle::EffectClosure();
	// A file that does not read keeps the effect it showed: read again, it shows it on (no seek).
	if (reason != EffectViewStatus::Unreadable) shown_.clear();
	scene_handed_ = false;
	shown_none();
	return held ? ViewportAction::Clear : ViewportAction::Keep;
}

ViewportAction EffectViewport::follow_(const ViewportInput &input, PreviewClock &clock) {
	const SessionView &view = input.view;
	if (!view.project.open) {
		catalog_.clear();
		effects_.clear();
		file_known_ = false;
		return stop_(EffectViewStatus::NoProject, std::string());
	}
	const TextDocument *text = input.document ? text_of(*input.document) : nullptr;
	if (!text) {
		effects_.clear();
		file_known_ = false;
		return stop_(EffectViewStatus::NoFile, std::string());
	}
	// The catalog the game would load were the project saved now, then the file as it stands through the
	// game's reader (again only when the document moved): its effects and the places they are written.
	catalog_.follow(view.project.scan, view.findings.assets);
	const DocumentBase &document = *input.document;
	const bool reread = !file_known_ || document.identity() != file_identity_ ||
			document.load_generation() != file_load_ || document.revision() != file_revision_;
	if (reread) {
		file_ = particle::ParticleFile();
		file_error_ = particle::ParseError();
		file_read_ = read_particle_text(*text, file_, file_error_);
		file_known_ = true;
		file_identity_ = document.identity();
		file_load_ = document.load_generation();
		file_revision_ = document.revision();
	}
	if (!file_read_) {
		effects_.clear();
		revealed_line_ = 0;
		const std::string where = file_error_.line > 0 ? " (line " + std::to_string(file_error_.line) + ")" : std::string();
		const ViewportAction action = stop_(EffectViewStatus::Unreadable, file_error_.message + where + ".");
		shown(document);
		return action;
	}
	const particle::ParticleFile &file = file_;
	// Each effect, and whether the catalog spawns this definition for its name: no earlier file of the
	// catalog's order, nor an earlier block of this one, defines it first (first registration wins).
	if (reread || registered_at_ != catalog_.serial()) {
		registered_at_ = catalog_.serial();
		std::set<std::string> earlier;
		for (const PreviewEffectCatalog::File &listed : catalog_.files()) {
			if (listed.path == path()) break;
			if (const particle::ParticleFile *before = catalog_.document_at(listed.path))
				for (const particle::EffectDef &effect : before->effects) earlier.insert(strutil::to_lower(effect.id));
		}
		effects_.clear();
		for (const particle::EffectDef &effect : file.effects) {
			if (effect.id.empty()) continue;
			effects_.push_back(
					{effect.id, effect.id_line, effect.id_column, earlier.insert(strutil::to_lower(effect.id)).second});
		}
	}
	// A revealed place shows the effect whose block holds it (a Go to of its name, a Problems row).
	if (revealed_line_ > 0) {
		const size_t at = particle_effect_at(file, revealed_line_);
		revealed_line_ = 0;
		if (at != std::string::npos && !file.effects[at].id.empty() &&
				!strutil::iequals(file.effects[at].id, options_.effect)) {
			options_.effect = file.effects[at].id;
			state_moved();
		}
	}
	// The effect shown: the options', where the file defines it, else the file's first.
	std::string effect;
	for (const EffectViewportEffect &listed : effects_)
		if (strutil::iequals(listed.id, options_.effect)) effect = listed.id;
	if (effect.empty() && !effects_.empty()) effect = effects_.front().id;
	if (effect.empty()) {
		const ViewportAction action = stop_(EffectViewStatus::NoEffect, std::string());
		shown(*input.document);
		return action;
	}
	// An effect newly shown starts at tick 0 of the clock, as a clip newly chosen does.
	const bool another = effect != shown_;
	if (another) clock.seek_ticks(0);
	loaded_ = catalog_.file_at(path()) != nullptr;
	// The scene opened again over the effect's closure where the effect or the catalog moved.
	if (another || !playback_.scene() || opened_catalog_ != catalog_.serial()) {
		closure_ = catalog_.closure(effect, particle::EffectSceneConfig());
		playback_.open(closure_.config, effect);
		opened_catalog_ = catalog_.serial();
		++opens_;
		scene_handed_ = false;
	}
	shown_ = effect;
	playback_.play_to(clock.ticks(), options_.play);
	if (closure_.spawns()) {
		reason_ = EffectViewStatus::Ready;
		detail_.clear();
	} else {
		reason_ = EffectViewStatus::SpawnsNothing;
		detail_ = closure_.unresolved_member < closure_.members.size()
				? "The game spawns nothing for " + effect + ": its member " + closure_.members[closure_.unresolved_member] +
				          " names no particle the game loads, and one missing member clears the whole effect."
				: "The game spawns nothing for " + effect + ": it names no particle (its pdefs is empty).";
	}
	shown(*input.document);
	// The device takes the scene opened last; a graphic it read that moved builds the picture again.
	if (!scene_handed_) {
		scene_handed_ = true;
		device_files_.clear();
		missing_.clear();
		return ViewportAction::Rebuild;
	}
	if (view.findings.assets && device_files_.moved(*view.findings.assets)) {
		device_files_.clear();
		missing_.clear();
		return ViewportAction::Rebuild;
	}
	return ViewportAction::Keep;
}

bool EffectViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool EffectViewport::check_(const io::JsonValue &json, std::string &error) const {
	EffectViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, effects_, options, error))
		return false;
	OrbitCamera camera = camera_;
	bool frame = false;
	if (const JsonValue *member = json.get("camera"); member && !read_camera(*member, camera, frame, error)) return false;
	return true;
}

void EffectViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	std::string error;
	if (const JsonValue *member = json.get("options")) read_options(*member, effects_, options_, error);
	if (const JsonValue *member = json.get("camera")) {
		bool frame = false;
		read_camera(*member, camera_, frame, error);
		if (frame) {
			const ViewportState at = size();
			camera_ = framed(at.width, at.height);
		}
	}
}

bool EffectViewport::report_(const ViewportDeviceReport &report) {
	device_files_.add(report.files);
	bool moved = false;
	for (const std::string &name : report.missing)
		if (std::find(missing_.begin(), missing_.end(), name) == missing_.end()) {
			missing_.push_back(name);
			moved = true;
		}
	return moved;
}

bool EffectViewport::spawned_place(std::string &file, std::string &locator) const {
	if (!closure_.found || closure_.source.empty()) return false;
	const particle::ParticleFile *defining = catalog_.document_at(closure_.source);
	if (!defining) return false;
	for (const particle::EffectDef &effect : defining->effects)
		if (strutil::iequals(effect.id, closure_.effect) && effect.id_line > 0) {
			file = closure_.source;
			locator = TextDocument::locator(size_t(effect.id_line), size_t(std::max(effect.id_column, 1)));
			return true;
		}
	return false;
}

std::string EffectViewport::place_of(const std::string &id) const {
	for (const EffectViewportEffect &effect : effects_)
		if (effect.id == id && effect.line > 0)
			return TextDocument::locator(size_t(effect.line), size_t(std::max(effect.column, 1)));
	return std::string();
}

void EffectViewport::receive(const ViewEvent &event) {
	if (event.kind != ViewEventKind::RevealText || event.path != path()) return;
	size_t line = 0, column = 0;
	if (TextDocument::read_locator(event.locator, line, column)) revealed_line_ = line;
}

OrbitCamera EffectViewport::framed(int width, int height) const {
	OrbitCamera camera = camera_;
	const std::shared_ptr<particle::EffectScene> &scene = playback_.scene();
	particle::Vec3 low{}, high{};
	bool any = false;
	if (scene) {
		const particle::EffectDebugSnapshot snapshot = scene->inspect(true);
		for (const particle::EffectGroupDebugSnapshot &group : snapshot.groups) {
			if (!group.bounds.valid) continue;
			if (!any) {
				low = group.bounds.minimum;
				high = group.bounds.maximum;
				any = true;
				continue;
			}
			low = {std::min(low.x, group.bounds.minimum.x), std::min(low.y, group.bounds.minimum.y),
			       std::min(low.z, group.bounds.minimum.z)};
			high = {std::max(high.x, group.bounds.maximum.x), std::max(high.y, group.bounds.maximum.y),
			        std::max(high.z, group.bounds.maximum.z)};
		}
	}
	if (!any) {
		const OrbitCamera fresh = default_camera();
		camera.target = fresh.target;
		camera.distance = fresh.distance;
		return camera;
	}
	const PreviewVec3 center{(low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f, (low.z + high.z) * 0.5f};
	const float dx = high.x - low.x, dy = high.y - low.y, dz = high.z - low.z;
	const float radius = std::max(0.5f * std::sqrt(dx * dx + dy * dy + dz * dz), 0.25f);
	camera.frame(center, radius, width > 0 ? width : kHeadlessSize.width, height > 0 ? height : kHeadlessSize.height);
	return camera;
}

std::unique_ptr<CanvasHalf> EffectViewport::make_canvas() const {
	OrbitCanvasHooks hooks;
	hooks.camera = [](const ViewportModel &viewport) -> const OrbitCamera & {
		return static_cast<const EffectViewport &>(viewport).camera();
	};
	hooks.framed = [](const ViewportModel &viewport, int width, int height) {
		return static_cast<const EffectViewport &>(viewport).framed(width, height);
	};
	hooks.change = effect_camera_change;
	return std::make_unique<OrbitCanvas>(hooks);
}

ViewportHit EffectViewport::hit(const ViewportContext &context, float, float) const {
	ViewportHit out;
	out.current = current(context.input);
	return out;
}

bool EffectViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
		std::string &error) const {
	error = "An effect's picture has no handles: a particle file's text holds no records.";
	return false;
}

bool EffectViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "Nothing is dragged in an effect's picture: its camera is set with set_viewport.";
	return false;
}

bool EffectViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &,
		CanvasRequests &out, std::string &error) const {
	if (name == "frame") {
		out.request(request::set_viewport(path(), effect_camera_change(framed(context.width, context.height))));
		return true;
	}
	if (name == "replay") {
		JsonValue clock = JsonValue::make_object();
		clock.set("ticks", json_number(0));
		out.request(request::set_viewport(path(), viewport_change(ViewportKind::Effect, "clock", std::move(clock))));
		return true;
	}
	error = "An effect's picture has no command \"" + name + "\" (frame, replay).";
	return false;
}

io::JsonValue EffectViewport::options_json() const {
	return effect_options_to_json(options_);
}

io::JsonValue EffectViewport::camera_json() const {
	return camera_to_json(camera_);
}

io::JsonValue EffectViewport::body_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_object();
	out.set("effect", json_string(shown_));
	// Whether the game loads this file at all (a gore-set file of the set the project does not pick is
	// never read: the name's definition is another file's, or the stock effect).
	out.set("loaded", JsonValue::make_bool(loaded_));
	// What the game's spawn of the name reads: the file that defines it first (another file's where this
	// one's is passed over), its members as the game resolves them, the definitions it instantiates.
	JsonValue resolved = JsonValue::make_object();
	resolved.set("found", JsonValue::make_bool(closure_.found));
	resolved.set("stock", JsonValue::make_bool(closure_.stock));
	resolved.set("defined_in", json_string(closure_.source));
	resolved.set("this_file", JsonValue::make_bool(closure_.source == path()));
	resolved.set("defined_again", json_number(double(closure_.shadowed)));
	JsonValue members = JsonValue::make_array();
	for (size_t i = 0; i < closure_.members.size(); ++i) {
		JsonValue member = JsonValue::make_object();
		member.set("name", json_string(closure_.members[i]));
		member.set("resolved", JsonValue::make_bool(i < closure_.unresolved_member));
		members.array.push_back(std::move(member));
	}
	resolved.set("members", std::move(members));
	JsonValue particles = JsonValue::make_array();
	for (const particle::EffectClosureDefinition &definition : closure_.definitions) {
		JsonValue row = JsonValue::make_object();
		row.set("id", json_string(definition.id));
		row.set("file", json_string(definition.source));
		row.set("member", json_number(double(definition.member)));
		row.set("child", JsonValue::make_bool(definition.child));
		particles.array.push_back(std::move(row));
	}
	resolved.set("particles", std::move(particles));
	resolved.set("spawns", JsonValue::make_bool(closure_.spawns()));
	out.set("resolved", std::move(resolved));
	// The playing cycle on the preview clock, and what the scene holds now.
	JsonValue play = JsonValue::make_object();
	play.set("tick", json_number(playback_.tick()));
	play.set("cycle_start", json_number(playback_.cycle_start()));
	play.set("age", json_number(playback_.age()));
	play.set("pre_aged", json_number(playback_.pre_aged()));
	play.set("spawns", json_number(double(playback_.spawns())));
	play.set("last_spawn", json_string(spawn_status_token(playback_.last_status())));
	play.set("alive", JsonValue::make_bool(playback_.alive()));
	if (const std::shared_ptr<particle::EffectScene> &scene = playback_.scene()) {
		const particle::EffectLiveCounts counts = scene->live_counts();
		play.set("groups", json_number(double(counts.group_count)));
		play.set("emitters", json_number(double(counts.emitter_count)));
		play.set("particles", json_number(double(counts.particle_count)));
	}
	out.set("play", std::move(play));
	// The catalog it was resolved over: its files in the effect system's order, and those the reader
	// stops in (their effects missing for the game too).
	JsonValue catalog = JsonValue::make_object();
	catalog.set("files", json_number(double(catalog_.files().size())));
	JsonValue unread = JsonValue::make_array();
	for (const PreviewEffectCatalog::File &file : catalog_.files()) {
		if (file.read) continue;
		JsonValue row = JsonValue::make_object();
		row.set("file", json_string(file.path));
		row.set("error", json_string(file.error.message));
		row.set("line", json_number(file.error.line));
		unread.array.push_back(std::move(row));
	}
	catalog.set("unread", std::move(unread));
	out.set("catalog", std::move(catalog));
	JsonValue missing = JsonValue::make_array();
	for (const std::string &name : missing_) missing.array.push_back(json_string(name));
	out.set("missing_graphics", std::move(missing));
	return out;
}

io::JsonValue EffectViewport::items_json(const ViewportInput &) const {
	JsonValue out = JsonValue::make_array();
	for (size_t i = 0; i < effects_.size(); ++i) {
		JsonValue item = JsonValue::make_object();
		item.set("index", json_number(double(i)));
		item.set("name", json_string(effects_[i].id));
		item.set("kind", json_string("effect"));
		item.set("line", json_number(effects_[i].line));
		item.set("registered", JsonValue::make_bool(effects_[i].registered));
		item.set("shown", JsonValue::make_bool(effects_[i].id == shown_));
		out.array.push_back(std::move(item));
	}
	return out;
}

} // namespace opennova::editor
