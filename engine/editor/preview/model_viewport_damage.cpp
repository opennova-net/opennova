// The model viewport's damage state (ADR 0046 DI-10, preview/model_damage): the uses read, the item, the husk
// models, the plan and the death's sounds (model_viewport.h).

#include <editor/preview/model_viewport.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <variant>

#include <base/io/fixed.h>
#include <base/io/hash.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/model_ctrl_words.h>
#include <editor/documents/model_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/animation_uses.h>
#include <editor/preview/model_canvas.h>
#include <editor/preview/model_placement.h>
#include <editor/preview/orbit_canvas.h>
#include <editor/preview/viewport_device.h>
#include <editor/project/project_document.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/present_passes.h>

namespace opennova::editor {

using io::json_number;
using io::json_string;
using io::JsonValue;

void ModelViewport::reset_damage_() {
	damage_uses_.clear();
	damage_graph_ = nullptr;
	damage_generation_ = 0;
	damage_use_ = SIZE_MAX;
	damage_catalog_.clear();
	damage_record_.clear();
	damage_catalog_stamp_ = 0;
	damage_item_ = DamageItem();
	husk_model_ = DamageModel();
	piece_model_ = DamageModel();
	damage_models_ = DamageModels();
	damage_plan_ = DamagePlan();
	damage_drives_ = false;
	damage_drawn_.clear();
	damage_note_.clear();
	damage_cursor_ = -1;
}

const DamageUse *ModelViewport::damage_use() const {
	return damage_use_ < damage_uses_.size() ? &damage_uses_[damage_use_] : nullptr;
}

std::string ModelViewport::follow_damage_(const ViewportInput &input, const PreviewClock &clock) {
	const SessionView &view = input.view;
	// The items naming the model, again as the graph moves (the edges are not kept: a graph replaced frees
	// them).
	const AssetGraph *graph = view.findings.graph.get();
	if (graph != damage_graph_ || (graph && graph->generation() != damage_generation_)) {
		damage_graph_ = graph;
		damage_generation_ = graph ? graph->generation() : 0;
		damage_uses_ = graph ? model_damage_uses(*graph, path()) : std::vector<DamageUse>();
		for (DamageUse &use : damage_uses_) use.edge = nullptr;
	}
	// The use the options name, else the first.
	damage_use_ = SIZE_MAX;
	for (size_t i = 0; i < damage_uses_.size() && !options_.damage.item.empty(); ++i)
		if (strutil::iequals(damage_uses_[i].item, options_.damage.item)) {
			damage_use_ = i;
			break;
		}
	if (damage_use_ == SIZE_MAX && !damage_uses_.empty()) damage_use_ = 0;
	const DamageUse *use = damage_use();
	// The item as its catalog reads, again as the catalog's stamp moves.
	const FileSource &files = *view.findings.assets;
	const std::string catalog = use ? io::utf8_file_name(use->file) : std::string();
	const std::string record = use ? use->item : std::string();
	const uint64_t stamp = catalog.empty() ? 0 : files.stamp(catalog);
	bool moved = false;
	if (catalog != damage_catalog_ || record != damage_record_ || stamp != damage_catalog_stamp_) {
		damage_catalog_ = catalog;
		damage_record_ = record;
		damage_catalog_stamp_ = stamp;
		damage_item_ = DamageItem();
		if (!catalog.empty()) read_damage_item(files, catalog, record, damage_item_);
		moved = true;
	}
	// The husk the game draws (husk first) and the model its pieces come from (huskfinal first), each the
	// project's file the game loads by its name, read again as its stamp moves.
	const AssetScan *scan = view.project.scan.get();
	const auto file_named = [&](const std::string &name) -> std::string {
		if (!scan || name.empty()) return std::string();
		const AssetEntry *entry = scan->find(name);
		if (!entry && !strutil::ends_with_icase(name, ".3di")) entry = scan->find(name + ".3di");
		return entry && entry->kind == AssetKind::Model ? entry->logical_name : std::string();
	};
	const auto refresh = [&](const std::string &file, DamageModel &held) {
		const uint64_t now = file.empty() ? 0 : files.stamp(file);
		if (file == held.file && now == held.stamp && held.read) return false;
		held = DamageModel();
		held.file = file;
		held.stamp = now;
		held.read = true;
		std::vector<uint8_t> bytes;
		if (!file.empty() && files.read(file, bytes)) held.model = assets::parse_model(bytes.data(), bytes.size());
		return true;
	};
	const DamageItem &item = damage_item_;
	const std::string husk_file =
			item.found ? file_named(world::husk_render_graphic(item.husk, item.huskfinal)) : std::string();
	const std::string piece_file =
			item.found ? file_named(world::death_piece_graphic(item.husk, item.huskfinal)) : std::string();
	moved = refresh(husk_file, husk_model_) || moved;
	moved = refresh(piece_file, piece_model_) || moved;
	if (moved) {
		damage_models_ = DamageModels();
		if (husk_model_.model) {
			note_damage_husk(*husk_model_.model, damage_models_);
			// The KZ walk reads the first husk alone: a def with only a final husk has no KZ point.
			if (item.husk.empty()) damage_models_.kz_points = 0;
		}
		if (piece_model_.model) note_damage_pieces(*piece_model_.model, damage_models_);
		damage_plan_ = editor::damage_plan(item, damage_models_);
	}
	// What the picture draws at the clock, and why where the destroyed state draws other than the husk.
	const std::string self = io::utf8_file_name(path());
	const bool graphic = use && (use->role == DamageRole::Graphic || use->role == DamageRole::EnemyGraphic);
	const bool husk_is_self = !husk_file.empty() && strutil::iequals(husk_file, self);
	damage_drives_ = use && item.found && damage_plan_.swaps && !damage_plan_.husk.empty() &&
	                 (husk_is_self || (graphic && husk_model_.model));
	damage_note_.clear();
	std::string drawn;
	if (options_.damage.state != DamageState::Destroyed) {
		damage_drawn_.clear();
		return drawn;
	}
	// The death's sounds play from the project's banks, read as the game reads them (DI-04's sources).
	sound_sources_.refresh(*view.findings.assets,
	                       view.project.document ? view.project.document->expansion.name : std::string(), PreviewRig());
	if (!use) {
		damage_note_ = "No item names this model as its graphic or its husk: it has no damage state.";
	} else if (!item.found) {
		damage_note_ = use->item + " does not read in " + catalog + ".";
	} else if (!damage_plan_.swaps) {
		damage_note_ = damage_plan_.class_words;
	} else if (graphic) {
		if (damage_plan_.husk.empty())
			damage_note_ = item.name + " authors no husk model: the preview keeps drawing its graphic.";
		else if (husk_file.empty())
			damage_note_ = "The project has no " + damage_plan_.husk + ": the preview keeps drawing the graphic.";
		else if (!husk_is_self && !husk_model_.model)
			damage_note_ = husk_file + " does not read as a model: the preview keeps drawing the graphic.";
		else if (!husk_is_self && damage_frame(damage_plan_, item, clock.ticks()).husked)
			drawn = husk_file;
	} else if (!husk_is_self) {
		damage_note_ = self + " is " + item.name + "'s final husk: the game draws it only as the death pieces, each " +
		               "its own section; " + damage_plan_.husk + " is the husk drawn.";
	}
	damage_drawn_ = drawn;
	return drawn;
}

bool ModelViewport::damage_playing() const {
	return options_.damage.state == DamageState::Destroyed && !animating_ && damage_use() && damage_item_.found &&
	       damage_plan_.swaps;
}

bool ModelViewport::damage_driven() const {
	return damage_playing() && damage_drives_;
}

DamageFrame ModelViewport::damage_frame_at(const PreviewClock &clock) const {
	return damage_playing() ? damage_frame(damage_plan_, damage_item_, clock.ticks()) : DamageFrame();
}

std::map<std::string, int64_t> ModelViewport::ctrl_at(const PreviewClock &clock) const {
	std::map<std::string, int64_t> out = options_.ctrl;
	if (!damage_driven()) return out;
	// The game writes the six every time the model draws: zero first, the fade's phases once husked [orig:
	// Entity_PublishSwapFadePhases @ 0x5C3F40].
	const DamageFrame frame = damage_frame(damage_plan_, damage_item_, clock.ticks());
	for (int i = 0; i < 6; ++i) {
		const char *name = threedi::threedi_ctrl_register_name(size_t(threedi::THREEDI_CTRL_OBJECT_DESTROY + i));
		for (auto it = out.begin(); it != out.end();) it = strutil::iequals(it->first, name) ? out.erase(it) : std::next(it);
		if (frame.fade.phases_q16[size_t(i)] != 0) out[name] = frame.fade.phases_q16[size_t(i)];
	}
	return out;
}

uint32_t ModelViewport::hidden_sections_at(const PreviewClock &clock) const {
	return damage_driven() ? damage_frame(damage_plan_, damage_item_, clock.ticks()).hidden_sections : 0;
}

std::vector<ClipSoundFired> ModelViewport::fire_damage_sounds(const PreviewClock &clock, const AssetScan *scan,
		audio::SoundSelector &selector, uint64_t &next_seq) {
	std::vector<ClipSoundFired> out;
	const int32_t now = clock.ticks();
	// After a seek, from where it put the clock (the death's tick 0 after a Play from it).
	const int32_t from = clock.heard_from(damage_cursor_, damage_seeks_);
	damage_cursor_ = now;
	if (!damage_playing() || reason_ != ModelViewStatus::Ready || from < 0 || now <= from ||
	    now - from > kClipSoundCatchUpTicks)
		return out;
	// The legs on the ticks [from, now): the death on tick 0 heard as the clock leaves it.
	for (const DamageLeg &leg : damage_plan_.legs) {
		if (leg.kind != "sound" || leg.tick < from || leg.tick >= now) continue;
		// Played at the item, the preview's origin, as the camera hears it.
		ClipSoundFired fired =
				plan_set_at_origin(leg.name, leg.tick, "the death sound", sound_sources_, camera().eye(), selector);
		fired.seq = ++next_seq;
		fired.path = path();
		if (scan) find_clip_sound_waves(fired, *scan);
		fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	if (fired_.size() > kSoundsFiredKept) fired_.erase(fired_.begin(), fired_.end() - kSoundsFiredKept);
	return out;
}

} // namespace opennova::editor
