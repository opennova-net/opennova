#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/effect_catalog.h>
#include <editor/preview/effect_playback.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/viewport_follow.h>
#include <editor/preview/viewport_model.h>
#include <formats/particle/parser.h>
#include <runtime/particle/effect_closure.h>

namespace opennova::editor {

// What an effect viewport shows, and why not (ADR 0046 DI-14): the kind's reason.
enum class EffectViewStatus : uint8_t {
	NoProject, // no project is open
	NoFile, // no particle file is open at its path
	Unreadable, // the game's particle reader does not read the file (detail: where it stops)
	NoEffect, // the file defines no effect
	SpawnsNothing, // the effect the game resolves spawns nothing (a member no particle the game loads names)
	Ready,
};
// "no_project", "no_file", "unreadable", "no_effect", "spawns_nothing", "ready": its token on the wire.
const char *effect_view_status_token(EffectViewStatus status);

// How an effect viewport plays and draws its effect: the effect shown (its id; "" the file's first,
// and an id the file no longer defines the first too), how it plays (effect_playback.h: loop, wind),
// and the ground grid under it (the editor's aid: a metre's squares on the plane the effect spawns on).
struct EffectViewportOptions {
	std::string effect;
	EffectPlayOptions play;
	bool grid = true;
	bool operator==(const EffectViewportOptions &other) const {
		return effect == other.effect && play == other.play && grid == other.grid;
	}
	bool operator!=(const EffectViewportOptions &other) const { return !(*this == other); }
};

// The options on the wire (the envelope's `options`, a SetViewport's): {effect, loop, wind_speed,
// wind_direction, grid}.
io::JsonValue effect_options_to_json(const EffectViewportOptions &options);
// The change a SetViewport makes to set an effect viewport's options to `options`, and its camera.
std::string effect_options_change(const EffectViewportOptions &options);
std::string effect_camera_change(const OrbitCamera &camera);

// One effect of the file as the viewport lists it: its id, where its id is written (its line), and
// whether the catalog spawns this definition for the name (false: an earlier file, or an earlier block
// of this one, defines it first).
struct EffectViewportEffect {
	std::string id;
	int line = 0;
	int column = 0;
	bool registered = true;
};

// A particle file's effect viewport (ADR 0046 DI-14; ViewportKind::Effect, the Preview role of the
// particle type; CONTEXT.md "Effect preview"): the effect the file defines that its options name,
// spawned by the engine's own effect scene as the game spawns one alone (preview/effect_playback: at the
// origin with no orientation, on the preview clock's ticks, spawned again as it dies while it loops,
// pre-aged on a seek), over the catalog the game would load were the project saved now
// (preview/effect_catalog: every particle file in the effect system's order, the open documents standing
// in), so what it shows is what the game spawns for the name: the first definition registered, its
// members as the game resolves them (all or nothing), its tables. An edit of any particle file shows at
// once (the scene opened again over the effect's closure, its age kept). The effect shown follows a
// place revealed in its file (a Go to, a Problems row: the effect whose block holds the place, a change
// its follow derives), and an effect newly shown starts at tick 0 of the preview clock (the clock
// sought, as a clip newly chosen is). Its camera orbits the spawn point (framed on the live particles
// by its frame command); a point of its picture names nothing (an effect holds no records). Its device
// draws the scene's snapshot through the game's particle renderer, the graphics read from the
// project's files (godot/src/authoring/effect_viewport_applier).
class EffectViewport final : public ViewportModel {
public:
	explicit EffectViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	EffectViewStatus view_status() const { return reason_; }
	const EffectViewportOptions &options() const { return options_; }
	const OrbitCamera &camera() const { return camera_; }
	// The file's effects, in its order, and the one shown (its id; "" none).
	const std::vector<EffectViewportEffect> &effects() const { return effects_; }
	const std::string &shown_effect() const { return shown_; }
	// What the game's spawn of the shown effect reads of the catalog (where it is defined, its members).
	const particle::EffectClosure &closure() const { return closure_; }
	// Whether the game loads the file at all: the catalog holds it (a gore-set file of the set the project
	// does not pick is never read).
	bool loaded() const { return loaded_; }
	const PreviewEffectCatalog &catalog() const { return catalog_; }
	// The playing scene (EffectPlayback: what the device draws, its clock).
	const EffectPlayback &playback() const { return playback_; }
	// How many times the scene was opened (an effect shown, an edit of the catalog): a test's measure.
	uint64_t opens() const { return opens_; }
	// Where the definition the game spawns for the effect shown is written: its file (project-relative)
	// and its id's place there ("line:column"), what a Go to of it opens; false for none.
	bool spawned_place(std::string &file, std::string &locator) const;
	// Where the effect `id` is written in this file (its place), what Go to its line opens; "" for none.
	std::string place_of(const std::string &id) const;

	ViewportStatus status() const override;
	const char *reason() const override { return effect_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	std::string caption() const override;
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	// "frame" (the camera on the live particles), "replay" (the clock sought to tick 0: the effect spawned
	// anew).
	bool command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &ids,
			CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue camera_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	// The file's effects, each an item: its index, its id as its name, kind "effect".
	io::JsonValue items_json(const ViewportInput &input) const override;
	void receive(const ViewEvent &event) override;

	// The camera looking at the live particles on a picture `width` x `height` (its angles kept), else
	// at the spawn point as a new viewport looks.
	OrbitCamera framed(int width, int height) const;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;
	bool report_(const ViewportDeviceReport &report) override;

private:
	ViewportAction stop_(EffectViewStatus reason, const std::string &detail);

	EffectViewStatus reason_ = EffectViewStatus::NoProject;
	std::string detail_;
	EffectViewportOptions options_;
	OrbitCamera camera_;
	std::vector<EffectViewportEffect> effects_;
	std::string shown_;
	PreviewEffectCatalog catalog_;
	particle::EffectClosure closure_;
	EffectPlayback playback_;
	uint64_t opened_catalog_ = 0; // the catalog serial the scene was opened over
	uint64_t opens_ = 0;
	// The file as the last follow read it (its document's identity, load and revision then: read again only
	// when one moved), and the catalog serial its effects' registrations were taken at.
	particle::ParticleFile file_;
	particle::ParseError file_error_;
	bool file_read_ = false;
	bool file_known_ = false;
	uint64_t file_identity_ = 0;
	uint64_t file_load_ = 0;
	uint64_t file_revision_ = 0;
	uint64_t registered_at_ = UINT64_MAX;
	// The place a RevealText asked for (its line; 0 none), taken at the next follow.
	size_t revealed_line_ = 0;
	// The graphics the device asked the project's files for and did not find (each once).
	std::vector<std::string> missing_;
	// The graphics the device read for the picture, each with its stamp: one moved builds it again.
	FileStamps device_files_;
	bool scene_handed_ = false; // the device was told to take the scene opened last
	bool loaded_ = false; // the catalog holds the file (the game loads it)
};

} // namespace opennova::editor
