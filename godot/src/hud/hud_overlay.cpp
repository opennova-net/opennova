#include "hud/hud_overlay.h"
#include "hud/hud_chat_entry.h"
#include "simulation/hud_view_records.h" // HudMapOverlays
#include "util/color_convert.h"
#include "hud/hud_draw_list_stats.h"
#include "hud/vehicle_hud_block.h"

#include "hud/hud_pos.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/simulation.h"
#include "simulation/player_local_view.h"
#include "terrain/terrain_data.h"
#include "util/axes.h"
#include "util/string_convert.h"

#include <formats/def/def.h> // DefVehicleHudBlock (the VEHICLE_HUD block the panel feed reads)
#include <runtime/hud/feed_format.h> // kGameTextLineColor (the 0x32 join/leave lines)
#include <base/gameprofile/game_type.h> // the conquest arm of the zone panel

#include <godot_cpp/classes/image.hpp>
#include <base/io/fixed.h>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace godot;

#include <runtime/hud/hud_minimap_feed.h> // the marker feed layout (decode)
#include <runtime/hud/hud_game_text.h> // hud_session_text (the session lines' strings)
#include <runtime/hud/hud_layout_from_hudpos.h> // the hudpos.def parse applied to the layout
#include <runtime/world/friendly_tags.h> // FriendlyTagSource (the D-HUD-20 gather)
#include <runtime/world/vehicle_attach.h> // AttachLabel (the seat/armory label scan)

using namespace opennova::def;
using namespace opennova::fnt;

namespace {

using opennova::hud::HudDrawList;
using opennova::hud::HudLayout;
using opennova::hud::HudPosRecord;
using opennova::hud::HudRectRecord;

constexpr int kMinimapFootprintFeedVersion = 1;

constexpr const char *kMinimapWaterShader = R"(
shader_type canvas_item;
render_mode unshaded, blend_mix;

varying vec4 map_color;

void vertex() {
	map_color = COLOR;
}

void fragment() {
	// The RG8 atlas is data, not color. Canvas sampler state can still resolve
	// TEXTURE with nearest filtering on some renderers, so reproduce retail's
	// four-tap linear sample explicitly before comparing the two height fields.
	ivec2 texture_size = textureSize(TEXTURE, 0);
	ivec2 max_cell = texture_size - ivec2(1);
	vec2 texel_position = UV * vec2(texture_size) - vec2(0.5);
	ivec2 cell = ivec2(floor(texel_position));
	vec2 fraction = fract(texel_position);
	vec2 h00 = texelFetch(TEXTURE, clamp(cell, ivec2(0), max_cell), 0).rg;
	vec2 h10 = texelFetch(TEXTURE, clamp(cell + ivec2(1, 0), ivec2(0), max_cell), 0).rg;
	vec2 h01 = texelFetch(TEXTURE, clamp(cell + ivec2(0, 1), ivec2(0), max_cell), 0).rg;
	vec2 h11 = texelFetch(TEXTURE, clamp(cell + ivec2(1, 1), ivec2(0), max_cell), 0).rg;
	vec2 height_and_water = mix(
			mix(h00, h10, fraction.x),
			mix(h01, h11, fraction.x), fraction.y);
	// Retail's fixed-function texture stage writes the filtered UNORM sample
	// back to an 8-bit channel before its ADDSIGNED/alpha-test chain. Comparing
	// the unquantized float makes every fractional shoreline height dry. Round
	// both operands to the same UNORM8 lattice before applying the cutoff.
	vec2 quantized_height_and_water = floor(height_and_water * 255.0 + vec2(0.5));
	if (quantized_height_and_water.r > quantized_height_and_water.g) {
		discard;
	}
	COLOR = map_color;
}
)";

} // namespace

int HudOverlay::hud_color_index_default() { return opennova::hud::kHudColorIndexDefault; }
int HudOverlay::clamp_hud_color_index(int p_index) { return opennova::hud::clamp_hud_color_index(p_index); }
int HudOverlay::hud_detail_level_default() { return opennova::hud::kHudDetailLevelDefault; }
int HudOverlay::hud_detail_level_blank() { return opennova::hud::kHudDetailLevelBlank; }
int HudOverlay::next_hud_detail_level(int p_level) { return opennova::hud::next_hud_detail_level(p_level); }
int HudOverlay::sight_scale_index_default() { return opennova::hud::kSightScaleIndexDefault; }
HudOverlay::FriendlyTagMode HudOverlay::next_friendly_tag_mode(FriendlyTagMode p_mode) {
	return static_cast<FriendlyTagMode>(opennova::hud::next_friendly_tag_mode(
			static_cast<opennova::hud::FriendlyTagMode>(p_mode)));
}

void HudOverlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_combat_state", "view", "camera", "projection", "has_camera",
								 "gametext", "use_key"),
			&HudOverlay::set_combat_state);
	ClassDB::bind_method(D_METHOD("set_scope_state", "view", "gametext"), &HudOverlay::set_scope_state);
    ClassDB::bind_method(D_METHOD("set_player_context", "view"), &HudOverlay::set_player_context);
	BIND_ENUM_CONSTANT(SHOWHUD_FLAG_GUN);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_OFF);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_FAR_BRIEF);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_FULL);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_BRIEF);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("hud_color_index_default"), &HudOverlay::hud_color_index_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("clamp_hud_color_index", "index"), &HudOverlay::clamp_hud_color_index);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("hud_detail_level_default"), &HudOverlay::hud_detail_level_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("hud_detail_level_blank"), &HudOverlay::hud_detail_level_blank);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_hud_detail_level", "level"), &HudOverlay::next_hud_detail_level);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("sight_scale_index_default"), &HudOverlay::sight_scale_index_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_friendly_tag_mode", "mode"), &HudOverlay::next_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("configure", "hudpos", "root"), &HudOverlay::configure);
	ClassDB::bind_method(D_METHOD("is_configured"), &HudOverlay::is_configured);
	ClassDB::bind_method(D_METHOD("set_crosshair_style", "style"), &HudOverlay::set_crosshair_style);
	ClassDB::bind_method(D_METHOD("get_crosshair_style"), &HudOverlay::get_crosshair_style);
	ClassDB::bind_method(D_METHOD("set_crosshair_color", "rgb"), &HudOverlay::set_crosshair_color);
	ClassDB::bind_method(D_METHOD("get_crosshair_color"), &HudOverlay::get_crosshair_color);
	ClassDB::bind_method(D_METHOD("set_crosshair_spread_enabled", "enabled"),
			&HudOverlay::set_crosshair_spread_enabled);
	ClassDB::bind_method(D_METHOD("is_crosshair_spread_enabled"),
			&HudOverlay::is_crosshair_spread_enabled);
	ClassDB::bind_method(D_METHOD("set_weapon", "weapon_name", "display_name",
								  "clipsize", "rounds_per_icon", "clipgfx_texture", "clipgfx_offset",
								  "rndgfx_texture", "rndgfx_offset", "rndgfx_step"),
			&HudOverlay::set_weapon);
	ClassDB::bind_method(D_METHOD("clear_weapon"), &HudOverlay::clear_weapon);
	ClassDB::bind_method(D_METHOD("push_message", "text"), &HudOverlay::push_message);
	ClassDB::bind_method(D_METHOD("set_kill_announcement", "text", "tick"), &HudOverlay::set_kill_announcement);
    ClassDB::bind_method(D_METHOD("reset_overlay_buffers"), &HudOverlay::reset_overlay_buffers);
	ClassDB::bind_method(D_METHOD("push_feed_line", "text", "argb"),
			&HudOverlay::push_feed_line);
	ClassDB::bind_method(D_METHOD("set_player_state", "ticks", "health_fraction", "stance", "fov_deg"),
			&HudOverlay::set_player_state);
	ClassDB::bind_method(D_METHOD("set_weapon_state", "active", "clip", "reserve", "heat",
								  "hud_spread_fp16", "aimed_shot_available",
								  "keep_crosshair_while_aimed", "windup_active", "windup_held_ticks"),
			&HudOverlay::set_weapon_state);
	ClassDB::bind_method(D_METHOD("set_view_state", "binoculars_view_active", "aim_screen"),
			&HudOverlay::set_view_state);
	ClassDB::bind_method(D_METHOD("set_objectives_header", "text"), &HudOverlay::set_objectives_header);
	ClassDB::bind_method(
			D_METHOD("set_scoreboard", "shown", "game_type", "frame_counter", "strings", "sim",
					"gametext"),
			&HudOverlay::set_scoreboard);
	ClassDB::bind_method(D_METHOD("scoreboard_page_key", "forward", "in_session", "board_open"),
			&HudOverlay::scoreboard_page_key);
	ClassDB::bind_method(D_METHOD("set_chat_input", "chat", "frame", "mp_session_peer"),
			&HudOverlay::set_chat_input);
	ClassDB::bind_method(D_METHOD("reset_scoreboard_page"), &HudOverlay::reset_scoreboard_page);
	ClassDB::bind_method(D_METHOD("set_vehicle_panel", "shown", "block", "stance", "sim"),
			&HudOverlay::set_vehicle_panel);
	ClassDB::bind_method(D_METHOD("push_chat_line", "text", "argb"),
			&HudOverlay::push_chat_line);
	ClassDB::bind_method(D_METHOD("post_feed_lines", "sim", "gametext", "mp_verbose"),
			&HudOverlay::post_feed_lines);
	ClassDB::bind_method(D_METHOD("set_end_round_statistics", "shown", "raised",
			"title", "labels", "values"),
			&HudOverlay::set_end_round_statistics);
	ClassDB::bind_method(D_METHOD("set_message_log_shown", "shown"),
			&HudOverlay::set_message_log_shown);
	ClassDB::bind_method(D_METHOD("set_help_screen", "shown", "title", "page_line", "footer",
								 "keys", "texts"),
			&HudOverlay::set_help_screen);
	ClassDB::bind_method(D_METHOD("set_map_legend", "shown", "title", "labels", "frame_counter"),
			&HudOverlay::set_map_legend);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("map_legend_keys"),
			&HudOverlay::map_legend_keys);
	ClassDB::bind_method(D_METHOD("set_briefing", "shown", "sim"), &HudOverlay::set_briefing);
	ClassDB::bind_method(D_METHOD("cycle_briefing_page", "direction", "in_session"),
			&HudOverlay::cycle_briefing_page);
	ClassDB::bind_method(D_METHOD("set_message_log_title", "title"),
			&HudOverlay::set_message_log_title);
	ClassDB::bind_method(D_METHOD("set_lfp_panel", "shown", "game_type", "local_team",
			"frame_counter", "strings", "sim"), &HudOverlay::set_lfp_panel);
	ClassDB::bind_method(D_METHOD("set_waypoint", "name", "distance_m",
			"mission_position", "altitude_wu"),
			&HudOverlay::set_waypoint, DEFVAL(Vector2()), DEFVAL(0.0f));
	ClassDB::bind_method(D_METHOD("clear_waypoint"), &HudOverlay::clear_waypoint);
	ClassDB::bind_method(D_METHOD("set_objectives", "shown", "mission_text", "sim"),
			&HudOverlay::set_objectives);
	ClassDB::bind_method(D_METHOD("set_attach_labels", "camera_xform", "camera_projection",
								  "gametext", "sim"),
			&HudOverlay::set_attach_labels);
	ClassDB::bind_method(D_METHOD("get_attach_label_count"), &HudOverlay::get_attach_label_count);
	ClassDB::bind_method(D_METHOD("get_attach_label_selected"),
			&HudOverlay::get_attach_label_selected);
	ClassDB::bind_method(D_METHOD("get_attach_label_text", "index"),
			&HudOverlay::get_attach_label_text);
	ClassDB::bind_method(D_METHOD("get_attach_label_position", "index"),
			&HudOverlay::get_attach_label_position);
	ClassDB::bind_method(D_METHOD("set_friendly_tags", "shown", "camera_xform",
								  "camera_projection", "fog_distance_units", "sim"),
			&HudOverlay::set_friendly_tags);
	ClassDB::bind_method(D_METHOD("set_radio_request_icon_viewer", "viewer"),
			&HudOverlay::set_radio_request_icon_viewer);
	ClassDB::bind_method(D_METHOD("set_role_facts", "sim", "gametext"),
			&HudOverlay::set_role_facts);
	ClassDB::bind_method(D_METHOD("set_weapon_ammo_key", "ammo_bucket", "ammo_class_id"),
			&HudOverlay::set_weapon_ammo_key);
	ClassDB::bind_method(D_METHOD("set_no_hud", "no_hud"), &HudOverlay::set_no_hud);
	ClassDB::bind_method(D_METHOD("set_end_round_overlay", "shown", "top", "bottom",
								  "texts", "ys"),
			&HudOverlay::set_end_round_overlay);
	ClassDB::bind_method(D_METHOD("set_friendly_tag_mode", "mode"),
			&HudOverlay::set_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("get_friendly_tag_mode"),
			&HudOverlay::get_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("set_hud_color_index", "index"),
			&HudOverlay::set_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_hud_color_index"),
			&HudOverlay::get_hud_color_index);
	ClassDB::bind_method(D_METHOD("set_hud_detail_level", "level"),
			&HudOverlay::set_hud_detail_level);
	ClassDB::bind_method(D_METHOD("get_hud_detail_level"),
			&HudOverlay::get_hud_detail_level);
	ClassDB::bind_method(D_METHOD("set_item_flash", "index", "value"),
			&HudOverlay::set_item_flash);
	ClassDB::bind_method(D_METHOD("set_showhud_flags", "flags"),
			&HudOverlay::set_showhud_flags);
    ClassDB::bind_method(D_METHOD("set_aspect_mode", "mode"), &HudOverlay::set_aspect_mode);
	ClassDB::bind_method(D_METHOD("cycle_sight_scale"), &HudOverlay::cycle_sight_scale);
	ClassDB::bind_method(D_METHOD("get_sight_scale_index"),
			&HudOverlay::get_sight_scale_index);
	ClassDB::bind_method(D_METHOD("set_minimap_terrain", "terrain", "water_mask"),
			&HudOverlay::set_minimap_terrain, DEFVAL(Ref<Texture2D>()));
	ClassDB::bind_method(D_METHOD("get_minimap_water_mask"),
			&HudOverlay::get_minimap_water_mask);
	ClassDB::bind_method(D_METHOD("set_minimap_state", "mission_position",
			"altitude_wu", "heading_bam", "zoom_q16", "big_zoom_q16",
			"map_mode", "flip_180", "snapshot"),
			&HudOverlay::set_minimap_state);
	ClassDB::bind_method(D_METHOD("set_minimap_grid_origin", "mission_position",
			"present"),
			&HudOverlay::set_minimap_grid_origin);
	ClassDB::bind_method(D_METHOD("set_minimap_footprints", "feed"),
			&HudOverlay::set_minimap_footprints);
	ClassDB::bind_method(D_METHOD("set_minimap_overlays", "overlays"),
			&HudOverlay::set_minimap_overlays);
	ClassDB::bind_method(D_METHOD("set_minimap_radar", "feed"),
			&HudOverlay::set_minimap_radar);
	ClassDB::bind_method(D_METHOD("get_radar_frame_gates"),
			&HudOverlay::get_radar_frame_gates);
	ClassDB::bind_method(D_METHOD("get_draw_list_stats"), &HudOverlay::get_draw_list_stats);
	ClassDB::bind_method(D_METHOD("set_draw_timing_enabled", "enabled"),
			&HudOverlay::set_draw_timing_enabled);
	ClassDB::bind_method(D_METHOD("consume_draw_timing_us"),
			&HudOverlay::consume_draw_timing_us);

	BIND_CONSTANT(MIN_CROSSHAIR_STYLE);
	BIND_CONSTANT(MAX_CROSSHAIR_STYLE);
	BIND_CONSTANT(DEFAULT_CROSSHAIR_COLOR);
	BIND_CONSTANT(CROSSHAIR_COLOR_MASK);
	BIND_CONSTANT(DEFAULT_CROSSHAIR_SPREAD);
}

HudOverlay::HudOverlay() {
	// Default view state matches the presenter's pre-first-update defaults.
	state_.health_fraction = 1.0f;
	state_.fov_deg = 80.0f;
}

HudOverlay::~HudOverlay() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		if (additive_item_.is_valid()) rs->free_rid(additive_item_);
		if (top_item_.is_valid()) rs->free_rid(top_item_);
	}
	corner_map_.release();
	big_map_.release();
	additive_item_ = RID();
	top_item_ = RID();
	clear_font_();
}

void HudOverlay::clear_font_() {
	if (font_valid_) {
		fnt_free(&font_);
	}
	font_ = {};
	font_valid_ = false;
	if (label_font_valid_) {
		fnt_free(&label_font_);
	}
	label_font_ = {};
	label_font_valid_ = false;
	if (label_font_bold_valid_) {
		fnt_free(&label_font_bold_);
	}
	label_font_bold_ = {};
	label_font_bold_valid_ = false;
	if (label_font_large_valid_) {
		fnt_free(&label_font_large_);
	}
	label_font_large_ = {};
	label_font_large_valid_ = false;
	if (label_font_impact38_valid_) {
		fnt_free(&label_font_impact38_);
	}
	label_font_impact38_ = {};
	label_font_impact38_valid_ = false;
	label_tier_ = -1;
	page_textures_ = {};
}

bool HudOverlay::load_fnt_(const String &p_name, fnt_font_t &r_font, int p_slot) {
	if (root_.is_null() || p_name.is_empty()) {
		return false;
	}
	const PackedByteArray bytes = root_->read_file(p_name.get_file());
	if (bytes.is_empty() ||
			fnt_parse(bytes.ptr(), static_cast<size_t>(bytes.size()), &r_font) != FNT_OK) {
		return false;
	}
	const size_t base = static_cast<size_t>(p_slot) * FNT_MAX_PAGES;
	for (uint32_t page = 0; page < r_font.num_pages && page < FNT_MAX_PAGES; ++page) {
		const uint8_t *data = fnt_get_page_data_const(&r_font, page);
		if (data == nullptr) {
			continue;
		}
		PackedByteArray page_bytes;
		page_bytes.resize(FNT_TEXTURE_SIZE);
		memcpy(page_bytes.ptrw(), data, FNT_TEXTURE_SIZE);
		const Ref<Image> image = Image::create_from_data(FNT_TEXTURE_WIDTH,
				FNT_TEXTURE_HEIGHT, false, Image::FORMAT_RGBA8, page_bytes);
		if (image.is_valid()) {
			page_textures_[base + page] = ImageTexture::create_from_image(image);
		}
	}
	return true;
}

void HudOverlay::ensure_label_fonts_(float p_surface_w) {
	// Device leg only: the tier/name/scale policy is the engine's
	// hud_label_font_choice; this loads the chosen .fnt pair through the VFS
	// and re-uploads when the width tier changes.
	const int w = static_cast<int>(p_surface_w);
	if (w <= 0 || root_.is_null()) {
		return;
	}
	const opennova::hud::HudLabelFontChoice choice =
			opennova::hud::hud_label_font_choice(w);
	if (choice.tier == label_tier_) {
		// Same files, new width: retail recomputes the slot scales on every
		// resolution change (the witness rides hud_label_font_choice), and the
		// HUD slot's bold copy follows them.
		if (w != label_width_) {
			label_width_ = w;
			push_label_fonts_(choice);
		}
		return;
	}
	if (label_font_valid_) {
		fnt_free(&label_font_);
		label_font_ = {};
		label_font_valid_ = false;
	}
	if (label_font_bold_valid_) {
		fnt_free(&label_font_bold_);
		label_font_bold_ = {};
		label_font_bold_valid_ = false;
	}
	if (label_font_large_valid_) {
		fnt_free(&label_font_large_);
		label_font_large_ = {};
		label_font_large_valid_ = false;
	}
	label_font_valid_ = load_fnt_(String(choice.normal_fnt), label_font_,
			opennova::hud::kHudFontSlotLabel);
	label_font_bold_valid_ =
			load_fnt_(String(choice.bold_fnt), label_font_bold_,
					opennova::hud::kHudFontSlotLabelBold);
	label_font_large_valid_ =
			load_fnt_(String(choice.large_fnt), label_font_large_,
					opennova::hud::kHudFontSlotLabelLarge);
	if (label_font_impact38_valid_) {
		fnt_free(&label_font_impact38_);
		label_font_impact38_ = {};
		label_font_impact38_valid_ = false;
	}
	label_font_impact38_valid_ =
			load_fnt_(String(choice.impact38_fnt), label_font_impact38_,
					opennova::hud::kHudFontSlotImpact38);
	label_tier_ = choice.tier;
	label_width_ = w;
	push_label_fonts_(choice);

	// The hudpos HUD font for this width, after the label fonts so an empty
	// name or a failed load falls back to the bold slot just loaded (the
	// witness rides hudpos_font_for_width and HudFrameCompiler::set_hudpos_font).
	if (font_valid_) {
		fnt_free(&font_);
		font_ = {};
		font_valid_ = false;
	}
	opennova::hud::HudLayoutAssets names;
	names.font_lo = hudpos_font_lo_;
	names.font_hi = hudpos_font_hi_;
	font_valid_ = load_fnt_(opennova::to_gd(opennova::hud::hudpos_font_for_width(names, w)),
			font_, opennova::hud::kHudFontSlotHud);
	compiler_.set_hudpos_font(font_valid_ ? &font_ : nullptr);
}

void HudOverlay::push_label_fonts_(const opennova::hud::HudLabelFontChoice &p_choice) {
	compiler_.configure_label_fonts(
			label_font_valid_ ? &label_font_ : nullptr,
			label_font_bold_valid_ ? &label_font_bold_ : nullptr,
			label_font_large_valid_ ? &label_font_large_ : nullptr,
			p_choice.scale, p_choice.large_scale,
			label_font_impact38_valid_ ? &label_font_impact38_ : nullptr);
}

Ref<Texture2D> HudOverlay::double_saturate_texture_(
		const Ref<Texture2D> &p_texture) const {
	if (p_texture.is_null()) {
		return p_texture;
	}
	Ref<Image> image = p_texture->get_image();
	if (image.is_null()) {
		return p_texture;
	}
	if (image->is_compressed()) {
		image->decompress();
	}
	image->convert(Image::FORMAT_RGBA8);
	PackedByteArray data = image->get_data();
	uint8_t *bytes = data.ptrw();
	const int64_t size = data.size();
	for (int64_t i = 0; i + 3 < size; i += 4) {
		for (int c = 0; c < 3; ++c) {
			const int v = bytes[i + c] * 2;
			bytes[i + c] = static_cast<uint8_t>(v > 255 ? 255 : v);
		}
	}
	const Ref<Image> doubled = Image::create_from_data(image->get_width(),
			image->get_height(), false, Image::FORMAT_RGBA8, data);
	return ImageTexture::create_from_image(doubled);
}

Ref<Texture2D> HudOverlay::load_hud_texture_(const String &p_name,
		bool p_generate_mipmaps) const {
	// HUD art ships as .tga; loaded by flat name through the mounted VFS.
	if (root_.is_null() || p_name.is_empty()) {
		return Ref<Texture2D>();
	}
	const PackedByteArray bytes = root_->read_file(p_name.get_file());
	if (bytes.is_empty()) {
		return Ref<Texture2D>();
	}
	Ref<Image> image;
	image.instantiate();
	if (image->load_tga_from_buffer(bytes) != OK) {
		return Ref<Texture2D>();
	}
	if (p_generate_mipmaps && image->generate_mipmaps() != OK) {
		return Ref<Texture2D>();
	}
	return ImageTexture::create_from_image(image);
}

void HudOverlay::load_crosshair_texture_() {
	// Retail's "cross%02d.tga" (style + 1) selection; witness in hud-re.md.
	char name[16];
	std::snprintf(name, sizeof(name), "cross%02d.tga", crosshair_style_ + 1);
	const Ref<Texture2D> tex = load_hud_texture_(String(name));
	textures_[opennova::hud::kHudTexCrosshair] = tex;
	layout_.crosshair_texture_valid = tex.is_valid();
	layout_.crosshair_tex_w = tex.is_valid() ? tex->get_width() : 0;
	layout_.crosshair_tex_h = tex.is_valid() ? tex->get_height() : 0;
}

void HudOverlay::configure(const Ref<HudPos> &p_hudpos, const Ref<ResourceRoot> &p_root) {
	root_ = p_root;
	layout_ = HudLayout{};
	textures_ = {};
	combat_texture_names_ = {};
	clear_font_();
	// The freed label pair must leave the compiler too; the first draw's
	// ensure_label_fonts_ reloads it for the fresh root.
	compiler_.configure_label_fonts(nullptr, nullptr, nullptr, 1.0f, 1.0f);
	configured_ = false;
	if (p_hudpos.is_null() || !p_hudpos->is_loaded()) {
		// No hudpos -> the declutter module's all-visible default (the level
		// itself persists across rebuilds like retail's global).
		const int level = declutter_.level();
		declutter_ = opennova::hud::HudDeclutter();
		declutter_.set_level(level);
		apply_declutter_();
		queue_redraw();
		return;
	}

	// The layout globals the parse authors are the engine's fill; this leg
	// resolves the names it hands back and stamps the texture-derived fields.
	opennova::hud::HudLayoutAssets assets;
	opennova::hud::hud_layout_from_hudpos(p_hudpos->native_file(), layout_, assets);
	configure_combat_(assets);

	// The HUDDECLUT mask table from the parsed rows: the engine's
	// declutter_from_hudpos (see docs/interface/hud-re.md); a file with no
	// declutter rows keeps the module's all-visible default.
	{
		const int level = declutter_.level();
		opennova::hud::HudDeclutter authored;
		declutter_ = opennova::hud::declutter_from_hudpos(p_hudpos->native_file(), authored)
				? authored
				: opennova::hud::HudDeclutter();
		declutter_.set_level(level);
		apply_declutter_();
	}

	// The static HUD frame background the engine picked (the last authored
	// StaticFrame line, hud_static_frame_index); absent, nothing is loaded.
	if (layout_.frame_pos.present) {
		const Ref<Texture2D> tex = load_hud_texture_(opennova::to_gd(assets.static_frame));
		textures_[opennova::hud::kHudTexFrame] = tex;
		layout_.frame_texture_valid = tex.is_valid();
		layout_.frame_tex_w = tex.is_valid() ? tex->get_width() : 0;
		layout_.frame_tex_h = tex.is_valid() ? tex->get_height() : 0;
	}
	{
		// The Tab board's stdbox atlases + the connection-icon strip. Absent
		// files simply leave the board unframed rather than failing the HUD.
		// The drawer samples only the border atlas today (the fill's brush
		// cell lives inside it); boxtile.tga stays registered for the
		// recorded border-x-camo combine residual, and the valid gate still
		// requires it because retail's style ctor draws NOTHING without the
		// secondary texture (the combined material rec+0x30 gates the whole
		// drawer) [orig: the combine @0x56af3c, the null gate @0x56b71f,
		// see docs/interface/hud-re.md].
		const Ref<Texture2D> border = load_hud_texture_("border.tga");
		const Ref<Texture2D> brush = load_hud_texture_("boxtile.tga");
		const Ref<Texture2D> icon = load_hud_texture_("neticon2.tga");
		textures_[opennova::hud::kHudTexBoxBorder] = border;
		textures_[opennova::hud::kHudTexBoxTile] = brush;
		textures_[opennova::hud::kHudTexNetIcon] = icon;
		layout_.box_texture_valid = border.is_valid() && brush.is_valid();
		// The piece size comes off the border atlas's own width, not a
		// constant [orig: the 4x4 cell grid, see docs/interface/hud-re.md].
		layout_.box_tex_w = border.is_valid() ? border->get_width() : 0;
		layout_.net_icon_texture_valid = icon.is_valid();
	}
	{
		// The AAS zone status panel's three team-icon atlases and the two
		// tiles [orig: HUD_LoadAllTextures @0x59dda0 — JO_LFP.tga team 1,
		// R_LFP.tga team 2, N_LFP.tga neutral; lfp_alf.tga the tile for
		// everyone else's zones -> 0x27239C0 @0x59e104/@0x59e10e, lfp_dlf.tga
		// the tile for the viewer's OWN zones -> 0x27239D0 (textureId +4 =
		// 0x27239D4) @0x59e11a/@0x59e11f, see docs/interface/hud-re.md].
		const Ref<Texture2D> team1 = load_hud_texture_("JO_LFP.tga");
		const Ref<Texture2D> team2 = load_hud_texture_("R_LFP.tga");
		const Ref<Texture2D> neutral = load_hud_texture_("N_LFP.tga");
		const Ref<Texture2D> tile_own = load_hud_texture_("lfp_dlf.tga");
		const Ref<Texture2D> tile_other = load_hud_texture_("lfp_alf.tga");
		textures_[opennova::hud::kHudTexLfpTeam1] = team1;
		textures_[opennova::hud::kHudTexLfpTeam2] = team2;
		textures_[opennova::hud::kHudTexLfpNeutral] = neutral;
		textures_[opennova::hud::kHudTexLfpTileOwn] = tile_own;
		textures_[opennova::hud::kHudTexLfpTileOther] = tile_other;
		layout_.lfp_icon_texture_valid[0] = team1.is_valid();
		layout_.lfp_icon_texture_valid[1] = team2.is_valid();
		layout_.lfp_icon_texture_valid[2] = neutral.is_valid();
		layout_.lfp_tile_own_texture_valid = tile_own.is_valid();
		layout_.lfp_tile_other_texture_valid = tile_other.is_valid();
	}

	{
		// The HUDLS bar's bracket and more-available marker, named by hudpos.
		const Ref<Texture2D> bracket = load_hud_texture_(opennova::to_gd(assets.hudls_bracket));
		const Ref<Texture2D> moreav = load_hud_texture_(opennova::to_gd(assets.hudls_moreav));
		textures_[opennova::hud::kHudTexSlotBarBracket] = bracket;
		textures_[opennova::hud::kHudTexSlotBarMoreAv] = moreav;
		layout_.hudls.bracket_texture_valid = bracket.is_valid();
		layout_.hudls.bracket_tex_w = bracket.is_valid() ? bracket->get_width() : 0;
		layout_.hudls.bracket_tex_h = bracket.is_valid() ? bracket->get_height() : 0;
		layout_.hudls.moreav_texture_valid = moreav.is_valid();
		layout_.hudls.moreav_tex_w = moreav.is_valid() ? moreav->get_width() : 0;
		layout_.hudls.moreav_tex_h = moreav.is_valid() ? moreav->get_height() : 0;
	}

	// The six stance frames' textures by slot (the engine resolved the
	// HUDSTANCE ids and offsets).
	for (int i = 0; i < 6; ++i) {
		const Ref<Texture2D> tex =
				load_hud_texture_(opennova::to_gd(assets.stance_textures[static_cast<size_t>(i)]));
		textures_[opennova::hud::kHudTexStance0 + i] = tex;
		layout_.stance_texture_valid[static_cast<size_t>(i)] = tex.is_valid();
	}
	const Ref<Texture2D> frame0 = textures_[opennova::hud::kHudTexStance0];
	layout_.stance_frame0_w = frame0.is_valid() ? frame0->get_width() : 0;
	layout_.stance_frame0_h = frame0.is_valid() ? frame0->get_height() : 0;

	load_crosshair_texture_();
	apply_crosshair_options_();
	// Retail uploads the strip at its authored resolution with its full
	// box-filtered mip chain; the default spinmap badges then sample near the
	// 16px level. A mipless upload aliases the source into a visibly broken
	// armory "A". The compiler's half-texel cell insets ride the PHYSICAL
	// dimensions of whatever strip this install mounts (stock 16x480,
	// RevX02 64x1920), so stamp the measured size.
	const Ref<Texture2D> map_icons = load_hud_texture_("TSDicon.tga", true);
	textures_[opennova::hud::kHudTexMapIcons] = map_icons;
	if (map_icons.is_valid()) {
		state_.minimap.icon_strip_w_px =
				static_cast<float>(map_icons->get_width());
		state_.minimap.icon_strip_h_px =
				static_cast<float>(map_icons->get_height());
	} else {
		state_.minimap.icon_strip_w_px =
				opennova::hud::HudMinimapInput{}.icon_strip_w_px;
		state_.minimap.icon_strip_h_px =
				opennova::hud::HudMinimapInput{}.icon_strip_h_px;
	}
	// The compass ring draws white-modulated through the fixed-function HUD
	// pipeline, whose output stage is MODULATE2X — for a static sprite that
	// is exactly a pre-doubled texture (the retail capture's band/letters
	// read ~2x ours before this). RGB doubles with saturation; alpha stays.
	textures_[opennova::hud::kHudTexMapCompass] =
			double_saturate_texture_(load_hud_texture_("compring.tga"));
	// The radar sector-slice marks load like the compass ring; their
	// MODULATE2X stage folds into the coloured diffuse at compile, so the
	// textures stay raw. The marks draw only while both loaded
	// (HudMinimapInput::radar_slices_loaded carries the witness).
	textures_[opennova::hud::kHudTexMapRadar] = load_hud_texture_("dmgslice.tga");
	textures_[opennova::hud::kHudTexMapRadarNarrow] = load_hud_texture_("dmgslc_n.tga");
	state_.minimap.radar_slices_loaded =
			textures_[opennova::hud::kHudTexMapRadar].is_valid() &&
			textures_[opennova::hud::kHudTexMapRadarNarrow].is_valid();
	textures_[opennova::hud::kHudTexMapWpIndicator] =
			load_hud_texture_("WPIndctr.tga");

	// The two hudpos font names; the width pick and the load ride
	// ensure_label_fonts_, which runs on the first draw and on every width-tier
	// change, as retail's HUD_InitAllFonts reloads them together.
	hudpos_font_lo_ = assets.font_lo;
	hudpos_font_hi_ = assets.font_hi;

	configured_ = true;
	compiler_.configure(layout_, nullptr);
	// Retail loads the overlay fonts at HUD init as well as on a resolution
	// change; clear_font_ reset the tier, so this loads them for the fresh root.
	ensure_label_fonts_(draw_surface_().x);
	queue_redraw();
}

bool HudOverlay::is_configured() const {
	return configured_;
}

void HudOverlay::set_crosshair_style(int p_style) {
	crosshair_style_ = std::clamp(p_style, static_cast<int>(MIN_CROSSHAIR_STYLE),
			static_cast<int>(MAX_CROSSHAIR_STYLE));
	if (!configured_) {
		return; // picked up by configure()
	}
	load_crosshair_texture_();
	compiler_.update_layout(layout_);
	queue_redraw();
}

int HudOverlay::get_crosshair_style() const {
	return crosshair_style_;
}

void HudOverlay::set_crosshair_color(int p_rgb) {
	crosshair_color_ = static_cast<uint32_t>(p_rgb) & opennova::hud::HudLayout::kCrosshairColorMask;
	if (!configured_) {
		return; // picked up by configure()
	}
	apply_crosshair_options_();
	compiler_.update_layout(layout_);
	queue_redraw();
}

int HudOverlay::get_crosshair_color() const {
	return static_cast<int>(crosshair_color_);
}

void HudOverlay::set_crosshair_spread_enabled(bool p_enabled) {
	crosshair_spread_enabled_ = p_enabled;
	if (!configured_) {
		return; // picked up by configure()
	}
	apply_crosshair_options_();
	compiler_.update_layout(layout_);
	queue_redraw();
}

bool HudOverlay::is_crosshair_spread_enabled() const {
	return crosshair_spread_enabled_;
}

void HudOverlay::apply_crosshair_options_() {
	// The stored RGB forced opaque; the semantics and defaults live on the
	// engine layout fields (hud_frame.h).
	layout_.crosshair_color = 0xFF000000u | crosshair_color_;
	layout_.crosshair_spread_enabled = crosshair_spread_enabled_;
}

void HudOverlay::set_weapon(const String &p_weapon_name, const String &p_display_name,
		int p_clipsize, int p_rounds_per_icon,
		const String &p_clipgfx_texture, const Vector2i &p_clipgfx_offset,
		const String &p_rndgfx_texture, const Vector2i &p_rndgfx_offset,
		const Vector2i &p_rndgfx_step) {
	(void)p_weapon_name; // identity lives presenter-side; the state is the HUD slice
	opennova::hud::HudWeaponState &wep = state_.weapon;
	const bool was_active = wep.active;
	wep = opennova::hud::HudWeaponState{};
	wep.active = was_active;
	wep.display_name = opennova::to_std(p_display_name);
	wep.capacity = p_clipsize;
	wep.rounds_per_icon = p_rounds_per_icon;
	wep.clipgfx_offset_x = p_clipgfx_offset.x;
	wep.clipgfx_offset_y = p_clipgfx_offset.y;
	wep.rndgfx_offset_x = p_rndgfx_offset.x;
	wep.rndgfx_offset_y = p_rndgfx_offset.y;
	wep.rndgfx_step_x = p_rndgfx_step.x;
	wep.rndgfx_step_y = p_rndgfx_step.y;

	const Ref<Texture2D> clip_tex = load_hud_texture_(p_clipgfx_texture);
	textures_[opennova::hud::kHudTexClipGfx] = clip_tex;
	wep.clip_texture_valid = clip_tex.is_valid();
	wep.clip_tex_w = clip_tex.is_valid() ? clip_tex->get_width() : 0;
	wep.clip_tex_h = clip_tex.is_valid() ? clip_tex->get_height() : 0;

	const Ref<Texture2D> round_tex = load_hud_texture_(p_rndgfx_texture);
	textures_[opennova::hud::kHudTexRoundGfx] = round_tex;
	wep.round_texture_valid = round_tex.is_valid();
	wep.round_tex_w = round_tex.is_valid() ? round_tex->get_width() : 0;
	wep.round_tex_h = round_tex.is_valid() ? round_tex->get_height() : 0;
	queue_redraw();
}

void HudOverlay::clear_weapon() {
	state_.weapon = opennova::hud::HudWeaponState{};
	textures_[opennova::hud::kHudTexClipGfx] = Ref<Texture2D>();
	textures_[opennova::hud::kHudTexRoundGfx] = Ref<Texture2D>();
	queue_redraw();
}

void HudOverlay::reset_overlay_buffers() {
    compiler_.reset_overlay_buffers();
    queue_redraw();
}

void HudOverlay::set_kill_announcement(const String &text, int64_t tick) {
	state_.kill_announcement.text = opennova::to_std(text);
	state_.kill_announcement.tick = static_cast<uint32_t>(tick);
	queue_redraw();
}

void HudOverlay::push_message(const String &p_text) {
	compiler_.push_message(opennova::to_std(p_text), state_.ticks);
	queue_redraw();
}

void HudOverlay::push_feed_line(const String &p_text, int64_t p_argb) {
	// The SYSTEM feed sink (kills, joins, system lines) — the packed ARGB is
	// stored raw and drawn as stored [orig: the stored-color read @0x59ae97].
	compiler_.push_feed_line(opennova::to_std(p_text),
			static_cast<uint32_t>(p_argb), state_.ticks);
	queue_redraw();
}

String HudOverlay::post_feed_lines(const Ref<Simulation> &p_sim,
		const Ref<RtxtStringFile> &p_gametext, bool p_mp_verbose) {
	String announcement;
	if (p_sim.is_null()) return announcement;
	std::vector<opennova::hud::FeedPost> posts;
	p_sim->drain_feed_posts(game_text_lookup(p_gametext), p_mp_verbose, posts);
	for (const opennova::hud::FeedPost &post : posts) {
		switch (post.sink) {
			case opennova::hud::ChatSink::System:
				compiler_.push_feed_line(post.text, post.argb, state_.ticks);
				break;
			case opennova::hud::ChatSink::Chat:
				compiler_.push_chat_line(post.text, post.argb, state_.ticks);
				break;
			case opennova::hud::ChatSink::Queue:
			case opennova::hud::ChatSink::Channel3:
				break; // neither is a ring
		}
		if (post.announce) announcement = opennova::to_gd(post.text);
	}
	if (!posts.empty()) queue_redraw();
	return announcement;
}

void HudOverlay::set_player_state(int p_ticks, float p_health_fraction, int p_stance,
		float p_fov_deg) {
	state_.ticks = p_ticks;
	// The HUD item flash countdown runs once per HUD frame on the logic
	// tick (hud_declutter.h HudItemFlash::tick).
	item_flash_.tick(p_ticks);
	state_.item_flash = item_flash_.timers();
	state_.health_fraction = p_health_fraction;
	state_.stance = p_stance;
	state_.fov_deg = p_fov_deg;
	queue_redraw();
}

void HudOverlay::set_weapon_state(bool p_active, int p_clip, int p_reserve, int p_heat,
		int p_hud_spread_fp16, bool p_aimed_shot_available,
		bool p_keep_crosshair_while_aimed, bool p_windup_active,
		int p_windup_held_ticks) {
	state_.weapon.active = p_active;
	state_.weapon.clip = p_clip;
	state_.weapon.reserve = p_reserve;
	state_.weapon.heat = p_heat;
	state_.hud_spread_fp16 = p_hud_spread_fp16;
	state_.aimed_shot_available = p_aimed_shot_available;
	state_.keep_crosshair_while_aimed = p_keep_crosshair_while_aimed;
	state_.windup_active = p_windup_active;
	state_.windup_held_ticks = p_windup_held_ticks;
	queue_redraw();
}

void HudOverlay::set_player_context(const Ref<PlayerLocalView> &p_view) {
    state_.mount_slot = p_view.is_valid() ? p_view->native_frame().hud_mount_slot : 0;
    state_.weapon_category = p_view.is_valid() ? p_view->native_frame().hud_weapon_category : 0;
    queue_redraw();
}

void HudOverlay::set_scope_state(const Ref<PlayerLocalView> &p_view, const Ref<RtxtStringFile> &p_gametext) {
    auto &scope = state_.scope;
    scope = {};
    if (p_view.is_valid()) {
        const auto &v = p_view->native_frame();
        scope.active = v.scope_details_active;
        scope.scoped = v.scope_details_scoped;
        scope.rangefinder = (v.scope_weapon_flags & 0x400u) != 0;
        scope.zeroable = (v.scope_weapon_flags & 0x800u) != 0;
        scope.range_q16 = v.aim_range_q16;
        scope.max_range_q16 = v.scope_max_range_q16;
        scope.zero_word = v.scope_zero_word;
        scope.zero_step_metres = v.scope_zero_step;
        scope.magnification = v.scope_magnification;
    }
    if (scope.active && p_gametext.is_valid()) {
        const auto text = [&](const char *section, const char *key) -> std::string {
            return opennova::to_std(p_gametext->get_string_in_section(section, key));
        };
        scope.range_format = text("Overlays", "STROVER_DIST");
        scope.range_over_1km = text("Overlays", "STROVER_DIST1KM");
        scope.zero_format = text("hud", "hud_scope_zero");
        scope.zero_auto = text("hud", "hud_scope_zero_auto");
        scope.zero_none = text("hud", "hud_scope_zero_none");
        scope.magnification_format = text("hud", "hud_scope_mag");
    }
    queue_redraw();
}

void HudOverlay::set_view_state(bool p_binoculars_view_active, const Vector2 &p_aim_screen) {
	state_.binoculars_view_active = p_binoculars_view_active;
	state_.aim_valid = p_aim_screen.is_finite();
	state_.aim_screen_x = state_.aim_valid ? p_aim_screen.x : 0.0f;
	state_.aim_screen_y = state_.aim_valid ? p_aim_screen.y : 0.0f;
	queue_redraw();
}

void HudOverlay::set_weapon_ammo_key(int p_ammo_bucket, int p_ammo_class_id) {
	state_.weapon.ammo_bucket = p_ammo_bucket;
	state_.weapon.ammo_class_id = static_cast<uint8_t>(p_ammo_class_id);
}

void HudOverlay::set_no_hud(bool p_no_hud) {
	state_.overlay_master = opennova::hud::hud_overlay_master(p_no_hud);
	queue_redraw();
}

void HudOverlay::set_objectives_header(const String &p_text) {
	state_.objectives_header = opennova::to_std(p_text);
}

void HudOverlay::set_scoreboard(bool p_shown, int64_t p_game_type, int p_frame_counter,
		const Dictionary &p_strings, const Ref<Simulation> &p_sim,
		const Ref<RtxtStringFile> &p_gametext) {
	opennova::hud::HudScoreboardState &sb = state_.scoreboard;
	sb.shown = p_shown;
	sb.game_type = static_cast<uint32_t>(p_game_type);
	// The 4-team page clock: the shell's 62 Hz HUD tick, the same fold (and the
	// same frame-rate caveat) as the LFP panel's blink counter below.
	sb.frame_counter = p_frame_counter;
	sb.title = opennova::to_std(String(p_strings.get("title", "")));
	sb.server_name = opennova::to_std(String(p_strings.get("server", "")));
	sb.mission_title = opennova::to_std(String(p_strings.get("mission", "")));
	sb.game_type_label = opennova::to_std(String(p_strings.get("game_type", "")));
	sb.players_line = opennova::to_std(String(p_strings.get("players", "")));
	sb.spectators_line = opennova::to_std(String(p_strings.get("spectators", "")));
	sb.footer = opennova::to_std(String(p_strings.get("footer", "")));
	// The drawers' own gametext (class names, the team-score labels, the
	// flag-carrier label) resolves through the engine's key table.
	const opennova::hud::ScoreboardText text =
			opennova::hud::scoreboard_text(game_text_lookup(p_gametext));
	sb.class_names = text.class_names;
	sb.header_text = text.header;
	sb.flag_carrier_label = text.flag_carrier_label;
	// Rows and session facts come straight from the role feed — no
	// script-side Dictionary round-trip to drop fields or lose the score sign.
	if (p_shown && p_sim.is_valid()) {
		p_sim->fill_scoreboard(sb);
	} else {
		sb.rows.clear();
		sb.team_count = 0;
		sb.flag_carrier = false;
	}
	queue_redraw();
}

void HudOverlay::set_chat_input(const Ref<HudChatEntry> &p_chat, int64_t p_frame,
		bool p_mp_session_peer) {
	opennova::hud::HudChatInputState &ci = state_.chat_input;
	const bool shown = p_chat.is_valid() && p_chat->is_capturing();
	if (!shown && !ci.shown) return;
	ci.shown = shown;
	if (shown) {
		const opennova::hud::ChatEntry &entry = p_chat->entry();
		ci.prompt = entry.prompt();
		ci.text = entry.text();
		ci.color = opennova::hud::chat_input_line_color(entry.open_dispatch(), p_mp_session_peer);
		ci.frame = static_cast<uint32_t>(p_frame);
	}
	queue_redraw();
}

// The special-key handler's scoreboard arm (hud_scoreboard.h
// scoreboard_takes_page_keys): true = consumed.
bool HudOverlay::scoreboard_page_key(bool p_forward, bool p_in_session, bool p_board_open) {
	if (!opennova::hud::scoreboard_takes_page_keys(p_in_session, p_board_open)) return false;
	compiler_.scoreboard_page_step(p_forward ? 1 : -1);
	queue_redraw();
	return true;
}

void HudOverlay::reset_scoreboard_page() {
	compiler_.reset_scoreboard_page();
}

void HudOverlay::set_end_round_overlay(bool p_shown, int p_top, int p_bottom,
		const PackedStringArray &p_texts, const PackedInt32Array &p_ys) {
	opennova::hud::HudEndRoundOverlayState &er = state_.end_round;
	er.shown = p_shown;
	er.top = p_top;
	er.bottom = p_bottom;
	er.lines.clear();
	const int64_t count = std::min(p_texts.size(), p_ys.size());
	for (int64_t i = 0; i < count; ++i) {
		opennova::hud::HudEndRoundLine line;
		line.text = opennova::to_std(p_texts[i]);
		line.y = p_ys[i];
		er.lines.push_back(line);
	}
	queue_redraw();
}

void HudOverlay::set_end_round_statistics(bool p_shown, bool p_raised,
		const String &p_title, const PackedStringArray &p_labels,
		const PackedStringArray &p_values) {
	// The SP Show Score panel (hud/end_round_statistics.h). The shell resolves
	// the title/labels and formats the values; the compiler owns the layout.
	opennova::hud::HudEndRoundStatisticsState &st = state_.end_round_statistics;
	st.shown = p_shown;
	st.raised = p_raised;
	st.title = opennova::to_std(p_title);
	for (int64_t i = 0; i < 4; ++i) {
		st.labels[i] = i < p_labels.size()
				? opennova::to_std(p_labels[i]) : std::string();
		st.values[i] = i < p_values.size()
				? opennova::to_std(p_values[i]) : std::string();
	}
	queue_redraw();
}

void HudOverlay::set_vehicle_panel(bool p_shown, const Ref<VehicleHudBlock> &p_block, int p_stance,
		const Ref<Simulation> &p_sim) {
	opennova::hud::HudVehiclePanelState &vp = state_.vehicle_panel;
	if (!p_shown || p_block.is_null()) {
		vp = opennova::hud::HudVehiclePanelState{};
		textures_[opennova::hud::kHudTexVehiclePanel] = Ref<Texture2D>();
		vehicle_panel_sid_ = String();
		queue_redraw();
		return;
	}
	const DefVehicleHudBlock &block = p_block->native();
	// The silhouette is per item: reload the slot when the rider's vehicle
	// changes (the set_weapon per-weapon art idiom).
	const String sid = p_block->get_sid();
	if (sid != vehicle_panel_sid_ || textures_[opennova::hud::kHudTexVehiclePanel].is_null()) {
		textures_[opennova::hud::kHudTexVehiclePanel] =
				load_hud_texture_(p_block->get_interface_texture());
		vehicle_panel_sid_ = sid;
	}
	const Ref<Texture2D> silhouette = textures_[opennova::hud::kHudTexVehiclePanel];
	vp.silhouette_valid = silhouette.is_valid() && silhouette->get_width() > 0 &&
			silhouette->get_height() > 0;
	vp.silhouette_w = vp.silhouette_valid ? silhouette->get_width() : 0;
	vp.silhouette_h = vp.silhouette_valid ? silhouette->get_height() : 0;
	// The base is the HUDVEHSTANCEPOS anchor plus the rider's HUDSTANCE
	// offset — the panel rides the stance icon.
	const int stance = CLAMP(p_stance, 0, 5);
	vp.anchor_x = layout_.veh_stance_pos.x;
	vp.anchor_y = layout_.veh_stance_pos.y;
	vp.stance_offset_x = layout_.stance_offset_x[static_cast<size_t>(stance)];
	vp.stance_offset_y = layout_.stance_offset_y[static_cast<size_t>(stance)];
	// Hull band + seat rows straight from the sim's feed, no script round-trip
	// (the set_scoreboard shape: the state from the args, the rows from the
	// sim; a null sim leaves the rows empty). The panel's one witnessed gate
	// is the interface texture: without it the whole panel is skipped, seats
	// included [orig: HUD_DrawVehicleHealthBars @0x5a5038 tests the loaded
	// texture's w/h, see docs/interface/hud-re.md].
	bool riding = true;
	if (p_sim.is_valid()) {
		riding = p_sim->fill_vehicle_panel(block, vp);
	} else {
		vp.seats.clear();
		vp.hull_health = 0;
		vp.hull_max_health = 0;
	}
	vp.shown = vp.silhouette_valid && riding;
	if (!vp.shown) vp.seats.clear();
	queue_redraw();
}

void HudOverlay::push_chat_line(const String &p_text, int64_t p_argb) {
	compiler_.push_chat_line(opennova::to_std(p_text),
			static_cast<uint32_t>(p_argb), state_.ticks);
	queue_redraw();
}

void HudOverlay::set_message_log_shown(bool p_shown) {
	state_.message_log_shown = p_shown;
	queue_redraw();
}

void HudOverlay::set_message_log_title(const String &p_title) {
	state_.message_log_title = opennova::to_std(p_title);
	queue_redraw();
}

void HudOverlay::set_lfp_panel(bool p_shown, int64_t p_game_type, int p_local_team,
		int p_frame_counter, const Dictionary &p_strings, const Ref<Simulation> &p_sim) {
	opennova::hud::HudLfpPanelState &lp = state_.lfp_panel;
	lp.local_team = p_local_team;
	// The blink clock the marker masks (`& 0x18`). The shell feeds the 62 Hz
	// HUD tick here; retail's g_HUDFrameCounter increments once per MAIN FRAME
	// [orig: Game_ProcessMainFrame @0x5265d5 -> Game_TickHudFrameCounters
	// @0x434c23, see docs/interface/hud-re.md], so retail's blink is
	// frame-rate dependent and matches this fold only at 62 fps.
	lp.frame_counter = p_frame_counter;
	// The conquest arm is the other branch of the same drawer and is
	// unmodelled [orig: g_GameType == 0x50010 @0x5a24a1].
	lp.conquest_mode = static_cast<uint32_t>(p_game_type) ==
			opennova::game_type::kConquerAndControl;
	lp.under_attack_text =
			opennova::to_std(String(p_strings.get("under_attack", "")));
	lp.ready_text = opennova::to_std(String(p_strings.get("ready", "")));
	if (p_shown && p_sim.is_valid()) {
		lp.shown = p_sim->fill_lfp_zones(p_local_team, lp.zones);
	} else {
		lp.shown = false;
		lp.zones.clear();
	}
	queue_redraw();
}

void HudOverlay::set_waypoint(const String &p_name, int p_distance_m,
		const Vector2 &p_mission_position, float p_altitude_wu) {
	state_.waypoint.present = true;
	state_.waypoint.name = opennova::to_std(p_name);
	state_.waypoint.distance_m = p_distance_m;
	state_.waypoint.world_x = opennova::io::float_to_fp16_16_sat(p_mission_position.x);
	state_.waypoint.world_y = opennova::io::float_to_fp16_16_sat(p_mission_position.y);
	state_.waypoint.world_z = opennova::io::float_to_fp16_16_sat(p_altitude_wu);
	queue_redraw();
}

void HudOverlay::clear_waypoint() {
	state_.waypoint = opennova::hud::HudWaypointState{};
	queue_redraw();
}

namespace {

// The play camera's projection of a world point to overlay pixels: false when
// the point is behind the near plane (Camera3D::is_position_behind's test),
// else Camera3D::unproject_position's math over the viewport's visible size.
bool project_to_overlay(const Transform3D &p_camera, const Projection &p_projection,
		const Vector2 &p_viewport_size, const Vector3 &p_world, Vector2 &r_screen) {
	const Vector3 eyedir = -p_camera.basis.get_column(2).normalized();
	if (eyedir.dot(p_world - p_camera.origin) < p_projection.get_z_near()) {
		return false;
	}
	Plane p(p_camera.xform_inv(p_world), 1.0f);
	p = p_projection.xform4(p);
	if (p.d == 0.0f) {
		return false;
	}
	p.normal /= p.d;
	r_screen = Vector2((p.normal.x * 0.5f + 0.5f) * p_viewport_size.x,
			(-p.normal.y * 0.5f + 0.5f) * p_viewport_size.y);
	return true;
}

String overlay_text(const Ref<RtxtStringFile> &p_gametext, const String &p_key,
		const String &p_fallback) {
	if (p_gametext.is_valid() &&
			p_gametext->has_string_in_section("Overlays", StringName(p_key))) {
		return p_gametext->get_string_in_section("Overlays", StringName(p_key));
	}
	return p_fallback;
}

// The label text per seat type, resolved in the gametext table's Overlays
// section with the witnessed missing-string fallbacks. The Gunner label
// prefers the weapon's attachtextid key: a PRESENT key resolves even to an
// empty string (the original stores the parse-time GameText_GetString result,
// "" on a miss, and draws it) — only an ABSENT key falls to the STROVER_USEGUN
// default.
// [orig: HUD_InitOverlaySystem @0x5a479c..0x5a481e — STROVER_SIT "!sit" /
//  STROVER_CONTROL "!Control" / STROVER_USEGUN "!UseGun" / STROVER_USEARMORY
//  "!UseArmory"; the USEGUN def-text pick @0x5a350c..0x5a3544; the parse resolve
//  @0x544d87. The STROVER_USEARMORYD "Armory in %d Seconds" delay variant is the MP
//  armory-delay state — deferred with it: docs/interface/hud-re.md (D-HUD-14).]
String attach_label_text(const Ref<RtxtStringFile> &p_gametext,
		opennova::world::SeatType p_seat_type, const std::string &p_attach_text_key) {
	using opennova::world::SeatType;
	switch (p_seat_type) {
		case SeatType::Passenger: // sitex [orig: dword_2723860]
			return overlay_text(p_gametext, "STROVER_SIT", "!sit");
		case SeatType::Controller:
		case SeatType::Driver:
			// ctrlx/drvrx share the Control label [orig: g_HUDLabelTextControl @0x5a34db/0x5a34fb]
			return overlay_text(p_gametext, "STROVER_CONTROL", "!Control");
		case SeatType::Gunner: // UseGun [orig: def+0x3A0 else dword_2723868]
			if (p_attach_text_key.empty()) {
				return overlay_text(p_gametext, "STROVER_USEGUN", "!UseGun");
			}
			// the witnessed empty-label quirk (parse-miss stores "")
			return overlay_text(p_gametext, opennova::to_gd(p_attach_text_key), "");
		case SeatType::ArmoryPoint: // armory [orig: dword_272386C]
			return overlay_text(p_gametext, "STROVER_USEARMORY", "!UseArmory");
		default:
			return String();
	}
}

} // namespace

void HudOverlay::set_objectives(bool p_shown, const Ref<RtxtStringFile> &p_mission_text,
		const Ref<Simulation> &p_sim) {
	state_.objectives.clear();
	if (p_shown && p_sim.is_valid()) {
		p_sim->fill_objectives(p_mission_text, state_.objectives);
	}
	queue_redraw();
}

// [orig: HUD_DrawVehicleSeatAndArmoryLabels @0x5a3290 — the projection
//  Math_FixedPointTransformPoint22 + HUD_ClipPointToFrustumAndProject @0x5a3655]
void HudOverlay::set_attach_labels(const Transform3D &p_camera_xform,
		const Projection &p_camera_projection, const Ref<RtxtStringFile> &p_gametext,
		const Ref<Simulation> &p_sim) {
	state_.attach_labels.clear();
	std::vector<opennova::world::AttachLabel> labels;
	if (p_sim.is_valid() && p_sim->fill_attach_labels(labels) && !labels.empty()) {
		const Viewport *viewport = get_viewport();
		const Vector2 viewport_size =
				viewport != nullptr ? viewport->get_visible_rect().size : Vector2();
		state_.attach_labels.reserve(labels.size());
		for (const opennova::world::AttachLabel &l : labels) {
			const Vector3 world_pos = mission_to_godot(l.world_pos);
			Vector2 screen;
			if (!project_to_overlay(p_camera_xform, p_camera_projection, viewport_size,
						world_pos, screen)) {
				continue; // [orig: HUD_ClipPointToFrustumAndProject nonzero = clipped @0x5a3655]
			}
			opennova::hud::HudAttachLabel label;
			label.screen_x = screen.x;
			label.screen_y = screen.y;
			label.text = opennova::to_std(attach_label_text(p_gametext, l.type, l.attach_text_key));
			label.nearest = l.nearest;
			state_.attach_labels.push_back(label);
		}
	}
	queue_redraw();
}

int HudOverlay::get_attach_label_count() const {
	return static_cast<int>(state_.attach_labels.size());
}

int HudOverlay::get_attach_label_selected() const {
	for (size_t i = 0; i < state_.attach_labels.size(); ++i) {
		if (state_.attach_labels[i].nearest) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

String HudOverlay::get_attach_label_text(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(state_.attach_labels.size())) {
		return String();
	}
	return opennova::to_gd(state_.attach_labels[static_cast<size_t>(p_index)].text);
}

Vector2 HudOverlay::get_attach_label_position(int p_index) const {
	if (p_index < 0 || p_index >= static_cast<int>(state_.attach_labels.size())) {
		return Vector2();
	}
	const auto &label = state_.attach_labels[static_cast<size_t>(p_index)];
	return Vector2(label.screen_x, label.screen_y);
}

// [orig: HUD_DrawFriendlyTagsPass @0x5a4480 -> HUD_DrawEntityLabel @0x5a39b0 —
//  distance @0x5a3aba, projection Math_FixedPointTransformPoint22 +
//  HUD_ClipPointToFrustumAndProject @0x5a3b47, fog g_EnvFogDistCurrent
//  @0x5a3b28. The speaking-pulse level feed is the dialog-channel follow-up.]
void HudOverlay::set_friendly_tags(bool p_shown, const Transform3D &p_camera_xform,
		const Projection &p_camera_projection, float p_fog_distance_units,
		const Ref<Simulation> &p_sim) {
	state_.friendly_tags.clear();
	set_friendly_tag_env(p_fog_distance_units, 0);
	std::vector<opennova::world::FriendlyTagSource> tags;
	if (p_shown && p_sim.is_valid() &&
			state_.friendly_tag_mode != static_cast<int>(opennova::hud::FriendlyTagMode::kOff) &&
			p_sim->fill_friendly_tags(tags) && !tags.empty()) {
		const Viewport *viewport = get_viewport();
		const Vector2 viewport_size =
				viewport != nullptr ? viewport->get_visible_rect().size : Vector2();
		state_.friendly_tags.reserve(tags.size());
		for (const opennova::world::FriendlyTagSource &t : tags) {
			// The anchor: the entity position + the eye height above its origin
			// (16.16 -> float) + the tag lift; the anchor witness lives at the
			// gather (friendly_tags.h).
			Vector3 world_pos = mission_to_godot(t.position);
			world_pos.y += static_cast<float>(t.eye_offset_z) / 65536.0f +
					opennova::hud::kFriendlyTagLiftUnits;
			Vector2 screen;
			if (!project_to_overlay(p_camera_xform, p_camera_projection, viewport_size,
						world_pos, screen)) {
				continue; // [orig: the nonzero-clip bail @0x5a3b80]
			}
			opennova::hud::HudFriendlyTag tag;
			tag.screen_x = screen.x;
			tag.screen_y = screen.y;
			tag.dist_q16 = opennova::io::float_to_fp16_16_sat(
					p_camera_xform.origin.distance_to(world_pos));
			tag.name = t.name;
			tag.entity_id = t.net_id;
			tag.health_ratio_fp16 = t.health_ratio_fp16;
			tag.medic = t.medic;
			// The speaking pulse is the overlay's own env feed, not a sim fact.
			tag.speaking = false;
			tag.player = t.player;
			// The downed legs (D-HUD-20 residue a): the compiler's recolor / count.
			tag.dead = t.dead;
			tag.has_slot = t.has_slot;
			tag.medic_request = t.medic_request;
			tag.revive_seconds = t.revive_seconds;
			// The radio-request icon's per-tag fold (the gather carries the
			// witness); the viewer gate is set_radio_request_icon_viewer's.
			tag.radio_request = t.radio_request;
			tag.squad_color_index = t.squad_color_index;
			state_.friendly_tags.push_back(tag);
		}
	}
	queue_redraw();
}

void HudOverlay::set_friendly_tag_mode(FriendlyTagMode p_mode) {
	state_.friendly_tag_mode =
			CLAMP(static_cast<int>(p_mode), 0, opennova::hud::kFriendlyTagModeCount - 1);
	queue_redraw();
}

HudOverlay::FriendlyTagMode HudOverlay::get_friendly_tag_mode() const {
	return static_cast<FriendlyTagMode>(state_.friendly_tag_mode);
}

void HudOverlay::set_hud_color_index(int p_index) {
	state_.hud_color_index = CLAMP(p_index, 0, 5);
	queue_redraw();
}

int HudOverlay::get_hud_color_index() const {
	return state_.hud_color_index;
}

void HudOverlay::apply_declutter_() {
	state_.declutter_visible = declutter_.visible();
	state_.hud_detail_level = declutter_.level();
}

void HudOverlay::set_hud_detail_level(int p_level) {
	// The level write + visibility rebuild [orig: the hud_detail global
	// @0x24D20BC -> CRenderState_SetLayerVisibility @0x59B0F0, see
	// docs/interface/hud-re.md]. The presenter owns the persistence and the
	// cycle/death-force policy. The level is stored VERBATIM: retail clamps
	// nowhere, and an out-of-range level hides every gated element until the
	// huddetail cycle wraps it (the engine module carries the arithmetic).
	declutter_.set_level(p_level);
	apply_declutter_();
	queue_redraw();
}

int HudOverlay::get_hud_detail_level() const {
	return declutter_.level();
}

void HudOverlay::set_item_flash(int p_index, int p_value) {
	// The timer store, then the level-0 layer rebuild that leaves the stored
	// level alone (hud_declutter.h HudItemFlash::set / apply_level).
	item_flash_.set(p_index, p_value);
	state_.item_flash = item_flash_.timers();
	declutter_.apply_level(0);
	state_.declutter_visible = declutter_.visible();
	queue_redraw();
}

void HudOverlay::set_showhud_flags(int p_flags) {
	// [orig: g_FpWeaponViewFlags — the compiler consumes bit 1 for the
	// corner spinmap block @0x5A8635; bit 0 is the viewmodel rig's, see
	// docs/interface/hud-re.md]
	state_.showhud_flags = static_cast<uint32_t>(p_flags) & 3u;
	queue_redraw();
}

int HudOverlay::cycle_sight_scale() {
	state_.sight_scale_index = opennova::hud::next_sight_scale_index(state_.sight_scale_index);
	queue_redraw();
	return state_.sight_scale_index;
}

int HudOverlay::get_sight_scale_index() const {
	return state_.sight_scale_index;
}

void HudOverlay::set_friendly_tag_env(float p_fog_distance_units,
		int p_speaking_level255) {
	const int32_t fog_q16 = opennova::io::float_to_fp16_16_sat(p_fog_distance_units);
	state_.fog_dist_q16 = fog_q16 > 0 ? fog_q16 : INT32_MAX;
	state_.speaking_level255 = p_speaking_level255;
}

void HudOverlay::set_radio_request_icon_viewer(bool p_viewer) {
	state_.radio_request_icon_viewer = p_viewer;
	queue_redraw();
}

// The HUD's role facts off the sim's role view (inmatch::hud_role_facts):
// the breath bar, the MP session lines with their gametext strings
// (hud::hud_session_text), and the HUDLS scan, whose icons load by name
// through the combat sprite cache only while HUDLS_SYSTEM makes the bar draw.
void HudOverlay::set_role_facts(const Ref<Simulation> &p_sim,
		const Ref<RtxtStringFile> &p_gametext) {
	const opennova::inmatch::HudRoleFacts facts = p_sim.is_valid()
			? p_sim->hud_role_facts()
			: opennova::inmatch::HudRoleFacts();
	state_.breath_samples = facts.breath.samples;
	state_.breath_time = facts.breath.breath_time;
	state_.spawn_success_gate = facts.breath.spawn_success_gate;
	state_.breath_label = opennova::to_std(overlay_text(p_gametext, "STROVER91", ""));
	state_.session = facts.session;
	opennova::hud::hud_session_text(game_text_lookup(p_gametext), state_.session.text);
	for (size_t c = 0; c < state_.slot_bar.size(); ++c) {
		opennova::hud::HudSlotBarSlot &slot = state_.slot_bar[c];
		slot = opennova::hud::HudSlotBarSlot{};
		if (layout_.hudls.system == 0) continue;
		slot.present = facts.slot_bar[c].adm_index >= 0;
		slot.count = facts.slot_bar[c].count;
		opennova::hud::HudSprite icon;
		combat_texture_(opennova::hud::kHudTexSlotBarIcon0 + static_cast<int>(c),
				slot.present ? opennova::to_gd(facts.slot_bar_icons[c]) : String(), icon);
		slot.icon_valid = icon.valid;
		slot.icon_w = icon.width;
		slot.icon_h = icon.height;
	}
	queue_redraw();
}

Ref<Texture2D> HudOverlay::get_minimap_water_mask() const {
	return textures_[opennova::hud::kHudTexMapWater];
}

void HudOverlay::set_minimap_terrain(const Ref<TerrainData> &p_terrain,
		const Ref<Texture2D> &p_water_mask) {
	state_.minimap.terrain = opennova::hud::HudMinimapTerrain{};
	textures_[opennova::hud::kHudTexMapTerrain].unref();
	textures_[opennova::hud::kHudTexMapWater].unref();
	if (p_terrain.is_null()) {
		queue_redraw();
		return;
	}

	opennova::hud::HudMinimapTerrain &terrain = state_.minimap.terrain;
	const PackedInt32Array grid = p_terrain->get_sector_grid();
	const int64_t copy_count = std::min<int64_t>(grid.size(),
			static_cast<int64_t>(terrain.sector_grid.size()));
	for (int64_t i = 0; i < copy_count; ++i) {
		terrain.sector_grid[static_cast<size_t>(i)] = grid[i];
	}
	terrain.origin_x = p_terrain->get_origin_x();
	terrain.origin_y = p_terrain->get_origin_y();
	terrain.sector_count = p_terrain->get_sector_count();
	terrain.sector_rows = p_terrain->get_sector_rows();
	terrain.present = copy_count == static_cast<int64_t>(terrain.sector_grid.size()) &&
			terrain.sector_count > 0 && terrain.sector_rows > 0;
	// Retail's base pass samples the original 512x512 colormap quadrants
	// directly. Bind an RGB copy so authored alpha cannot ghost the backing,
	// then keep depthspin's transparent shore cutout in its own texture slot.
	Ref<Texture2D> colormap = p_terrain->get_colormap();
	if (colormap.is_valid()) {
		Ref<Image> image = colormap->get_image();
		if (image.is_valid()) {
			if (image->is_compressed()) {
				image->decompress();
			}
			image->convert(Image::FORMAT_RGB8);
			colormap = ImageTexture::create_from_image(image);
		}
	}
	textures_[opennova::hud::kHudTexMapTerrain] = colormap;
	textures_[opennova::hud::kHudTexMapWater] = p_water_mask;
	terrain.water_present = p_water_mask.is_valid();
	queue_redraw();
}

void HudOverlay::set_minimap_state(const Vector2 &p_mission_position,
		float p_altitude_wu, int64_t p_heading_bam, int p_zoom_q16,
		int p_big_zoom_q16, int p_map_mode, bool p_flip_180,
		const PackedInt32Array &p_snapshot) {
	state_.minimap.player_x = opennova::io::float_to_fp16_16_sat(p_mission_position.x);
	state_.minimap.player_y = opennova::io::float_to_fp16_16_sat(p_mission_position.y);
	state_.minimap.player_z = opennova::io::float_to_fp16_16_sat(p_altitude_wu);
	state_.minimap.player_heading_bam = static_cast<int32_t>(p_heading_bam);
	state_.minimap.zoom_q16 = std::clamp(p_zoom_q16,
			opennova::hud::kSpinmapZoomMin, opennova::hud::kSpinmapZoomMax);
	state_.minimap.big_zoom_q16 = std::clamp(p_big_zoom_q16,
			opennova::hud::kSpinmapZoomMin, opennova::hud::kSpinmapZoomMax);
	// Retail's cycle only produces 0/2/3.
	state_.minimap.map_mode =
			(p_map_mode == 2 || p_map_mode == 3) ? p_map_mode : 0;
	state_.minimap.flip_180 = p_flip_180;
	state_.minimap.markers.clear();

	// The feed layout and its parse are the engine's (hud/hud_minimap_feed.h):
	// a foreign header leaves the marker list empty.
	opennova::hud::minimap_feed_decode(p_snapshot.ptr(),
			static_cast<size_t>(p_snapshot.size()), state_.minimap.markers);
	queue_redraw();
}

void HudOverlay::set_minimap_radar(const PackedInt32Array &p_feed) {
	// The layout and its parse are the engine's (hud/hud_minimap_feed.h): a
	// foreign header leaves the radar legs with nothing lit.
	opennova::hud::radar_feed_decode(p_feed.ptr(), static_cast<size_t>(p_feed.size()),
			state_.minimap.radar);
	queue_redraw();
}

int HudOverlay::get_radar_frame_gates() const {
	return static_cast<int>(compiler_.radar_frame_gates(state_));
}

void HudOverlay::set_minimap_footprints(const PackedInt32Array &p_feed) {
	// {version, count} then per row {handle, fill_argb, fill_value_count,
	// xy..., edge_value_count, xy...} — mission 16.16 world space, baked
	// once per mission by the sim feed.
	state_.map_footprints.clear();
	if (p_feed.size() < 2 || p_feed[0] != kMinimapFootprintFeedVersion) {
		queue_redraw();
		return;
	}
	const int64_t total = p_feed.size();
	int64_t cursor = 2;
	const int32_t rows = p_feed[1];
	for (int32_t row = 0; row < rows; ++row) {
		if (cursor + 3 > total) break;
		opennova::hud::HudMinimapFootprint footprint;
		footprint.handle = static_cast<uint16_t>(p_feed[cursor++]);
		footprint.fill_argb = static_cast<uint32_t>(p_feed[cursor++]);
		const int32_t fill_values = p_feed[cursor++];
		if (fill_values < 0 || cursor + fill_values > total) break;
		footprint.fill_xy_q16.reserve(static_cast<size_t>(fill_values));
		for (int32_t i = 0; i < fill_values; ++i)
			footprint.fill_xy_q16.push_back(p_feed[cursor++]);
		if (cursor + 1 > total) break;
		const int32_t edge_values = p_feed[cursor++];
		if (edge_values < 0 || cursor + edge_values > total) break;
		footprint.edge_xy_q16.reserve(static_cast<size_t>(edge_values));
		for (int32_t i = 0; i < edge_values; ++i)
			footprint.edge_xy_q16.push_back(p_feed[cursor++]);
		opennova::hud::hud_minimap_finalize_footprint(footprint);
		state_.map_footprints.push_back(std::move(footprint));
	}
	queue_redraw();
}

void HudOverlay::set_minimap_overlays(const Ref<HudMapOverlays> &p_overlays) {
	// The engine owns the gather (inmatch/minimap_overlays.h); the frame state
	// holds the value the spinmap compile borrows.
	state_.map_overlays = p_overlays.is_valid() ? p_overlays->value()
			: opennova::hud::HudMinimapOverlays{};
	queue_redraw();
}

void HudOverlay::set_minimap_grid_origin(const Vector2 &p_mission_position,
		bool p_present) {
	// The grid-label origin: the mission's type-2043 marker entity, if one
	// exists (witness at HudMinimapInput::grid_origin_x).
	state_.minimap.grid_origin_present = p_present;
	state_.minimap.grid_origin_x = opennova::io::float_to_fp16_16_sat(p_mission_position.x);
	state_.minimap.grid_origin_y = opennova::io::float_to_fp16_16_sat(p_mission_position.y);
	queue_redraw();
}

Vector2 HudOverlay::draw_surface_() const {
	// This overlay is a Control parented to a CanvasLayer, which does not
	// drive a child Control's layout — its own size can stay (0,0) and every
	// scaled element would collapse. Draw against the viewport rect (the real
	// screen the original scales its HUD to); an owner that sizes this Control
	// (tests pin exact geometry) wins over the viewport rect.
	const Vector2 own = get_size();
	if (own.x > 1.0f && own.y > 1.0f) {
		return own;
	}
	if (is_inside_tree()) {
		return get_viewport_rect().size;
	}
	return Vector2(1024.0f, 768.0f);
}

Ref<HudDrawListStats> HudOverlay::get_draw_list_stats() {
	Ref<HudDrawListStats> out;
	out.instantiate();
	int64_t quads_filled = 0;
	int64_t quads_wire = 0;
	int64_t quads_textured = 0;
	int64_t quads_additive = 0;
	int64_t tris = 0;
	int64_t lines = 0;
	int64_t glyphs = 0;
	int64_t underlines = 0;
	int64_t elements = 0;
	bool map_visible = false;
	int64_t map_terrain_tris = 0;
	int64_t map_footprint_tris = 0;
	int64_t map_sprites = 0;
	int64_t map_lines_under = 0;
	int64_t map_lines = 0;
	int64_t map_labels = 0;
	bool big_map_visible = false;
	int64_t big_map_terrain_tris = 0;
	int64_t big_map_footprint_tris = 0;
	int64_t big_map_sprites = 0;
	int64_t big_map_lines_under = 0;
	int64_t big_map_lines = 0;
	int64_t big_map_labels = 0;
	int64_t big_map_glyphs = 0;
	if (configured_) {
		const Vector2 surface = draw_surface_();
		ensure_label_fonts_(surface.x); // the fonts the draw would have loaded
		const HudDrawList &list = compiler_.compile(state_, surface.x, surface.y);
		for (const opennova::hud::HudQuad &quad : list.quads) {
			if (!quad.filled) {
				++quads_wire;
			} else {
				++quads_filled;
			}
			if (quad.texture != opennova::hud::kHudTexNone) {
				++quads_textured;
			}
			if (quad.additive) {
				++quads_additive;
			}
		}
		tris = static_cast<int64_t>(list.tris.size());
		lines = static_cast<int64_t>(list.lines.size());
		glyphs = static_cast<int64_t>(list.glyphs.size());
		underlines = static_cast<int64_t>(list.underlines.size());
		elements = list.elements_drawn;
		map_visible = list.map.visible;
		map_terrain_tris = static_cast<int64_t>(list.map.terrain.size());
		map_footprint_tris = static_cast<int64_t>(list.map.overlays.size());
		map_sprites = static_cast<int64_t>(list.map.sprites.size());
		map_lines_under = static_cast<int64_t>(list.map.lines_under.size());
		map_lines = static_cast<int64_t>(list.map.lines.size());
		map_labels = static_cast<int64_t>(list.map.labels.size());
		big_map_visible = list.big_map.visible;
		big_map_terrain_tris = static_cast<int64_t>(list.big_map.terrain.size());
		big_map_footprint_tris = static_cast<int64_t>(list.big_map.overlays.size());
		big_map_sprites = static_cast<int64_t>(list.big_map.sprites.size());
		big_map_lines_under = static_cast<int64_t>(list.big_map.lines_under.size());
		big_map_lines = static_cast<int64_t>(list.big_map.lines.size());
		big_map_labels = static_cast<int64_t>(list.big_map.labels.size());
		big_map_glyphs = static_cast<int64_t>(list.big_map_glyphs.size());
	}
	out->set_quads(quads_filled + quads_wire);
	out->set_quads_filled(quads_filled);
	out->set_quads_wire(quads_wire);
	out->set_quads_textured(quads_textured);
	out->set_quads_additive(quads_additive);
	out->set_tris(tris);
	out->set_lines(lines);
	out->set_glyphs(glyphs);
	out->set_underlines(underlines);
	out->set_elements_drawn(elements);
	out->set_map_visible(map_visible);
	out->set_map_terrain_tris(map_terrain_tris);
	out->set_map_footprint_tris(map_footprint_tris);
	out->set_map_sprites(map_sprites);
	out->set_map_lines_under(map_lines_under);
	out->set_map_lines(map_lines);
	out->set_map_labels(map_labels);
	out->set_big_map_visible(big_map_visible);
	out->set_big_map_terrain_tris(big_map_terrain_tris);
	out->set_big_map_footprint_tris(big_map_footprint_tris);
	out->set_big_map_sprites(big_map_sprites);
	out->set_big_map_lines_under(big_map_lines_under);
	out->set_big_map_lines(big_map_lines);
	out->set_big_map_labels(big_map_labels);
	out->set_big_map_glyphs(big_map_glyphs);
	const Ref<Texture2D> map_icons = textures_[opennova::hud::kHudTexMapIcons];
	const Ref<Image> map_icon_image =
			map_icons.is_valid() ? map_icons->get_image() : Ref<Image>();
	out->set_map_icon_mipmaps(map_icon_image.is_valid() && map_icon_image->has_mipmaps());
	out->set_map_icon_width(map_icon_image.is_valid() ? map_icon_image->get_width() : 0);
	out->set_map_icon_height(map_icon_image.is_valid() ? map_icon_image->get_height() : 0);
	out->set_map_texture_filter(corner_map_.top_sampling_configured()
			? static_cast<int64_t>(
					RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_map_texture_repeat(corner_map_.top_sampling_configured()
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	out->set_map_water_texture_filter(corner_map_.water_sampling_configured()
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_map_water_texture_repeat(corner_map_.water_sampling_configured()
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	out->set_big_map_texture_filter(big_map_.top_sampling_configured()
			? static_cast<int64_t>(
					RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_big_map_texture_repeat(big_map_.top_sampling_configured()
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	out->set_big_map_water_texture_filter(big_map_.water_sampling_configured()
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_big_map_water_texture_repeat(big_map_.water_sampling_configured()
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	return out;
}

void HudOverlay::ensure_additive_item_() {
	if (additive_item_.is_valid()) {
		return;
	}
	if (additive_material_.is_null()) {
		additive_material_.instantiate();
		additive_material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	additive_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(additive_item_, get_canvas_item());
	rs->canvas_item_set_material(additive_item_, additive_material_->get_rid());
	// Above the four-layer corner-map sandwich like every non-map additive row.
	rs->canvas_item_set_draw_index(additive_item_, 4);
}

void HudOverlay::ensure_minimap_water_material_() {
	if (minimap_water_material_.is_valid()) {
		return;
	}
	minimap_water_shader_.instantiate();
	minimap_water_shader_->set_code(kMinimapWaterShader);
	minimap_water_material_.instantiate();
	minimap_water_material_->set_shader(minimap_water_shader_);
}

void HudOverlay::ensure_map_materials_() {
	if (additive_material_.is_null()) {
		additive_material_.instantiate();
		additive_material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
	}
	ensure_minimap_water_material_();
}

RID HudOverlay::map_additive_material() {
	ensure_map_materials_();
	return additive_material_->get_rid();
}

RID HudOverlay::map_water_material() {
	ensure_map_materials_();
	return minimap_water_material_->get_rid();
}

HudMapPassTextures HudOverlay::map_pass_textures() const {
	HudMapPassTextures out;
	out.terrain = textures_[opennova::hud::kHudTexMapTerrain];
	out.water = textures_[opennova::hud::kHudTexMapWater];
	out.slots = textures_.data();
	out.slot_count = kTextureSlots;
	out.pages = page_textures_.data();
	out.page_count = page_textures_.size();
	return out;
}

const opennova::hud::HudFrameCompiler::MapWindowDraw *HudOverlay::compile_death_map(
		const opennova::hud::DeathMapFrame &p_frame,
		const opennova::hud::DeathMapFacts &p_facts, float p_surface_w, float p_surface_h) {
	if (!configured_) return nullptr;
	// The window's letters lay out in this overlay's label fonts, loaded for
	// the width tier like every other map pass (engine hud_label_font_choice).
	ensure_label_fonts_(p_surface_w);
	return &compiler_.compile_death_map(state_, p_frame, p_facts, p_surface_w, p_surface_h);
}

const opennova::hud::HudFrameCompiler::MapWindowDraw *HudOverlay::compile_command_map(
		opennova::hud::CommandMapView &p_cmap, const opennova::hud::MapViewRect &p_rect,
		int32_t p_scaled_800, const opennova::hud::DeathMapFacts &p_facts, float p_surface_w,
		float p_surface_h) {
	if (!configured_) return nullptr;
	ensure_label_fonts_(p_surface_w);
	return &compiler_.compile_command_map(state_, p_cmap, p_rect, p_scaled_800, p_facts,
			p_surface_w, p_surface_h);
}

void HudOverlay::_notification(int p_what) {
	if (p_what == NOTIFICATION_RESIZED) {
		queue_redraw();
	}
}

void HudOverlay::_draw() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (additive_item_.is_valid()) {
		rs->canvas_item_clear(additive_item_);
	}
	corner_map_.clear();
	big_map_.clear();
	if (top_item_.is_valid()) {
		rs->canvas_item_clear(top_item_);
	}
	if (!configured_) {
		return;
	}
	const Vector2 surface = draw_surface_();
	// Retail re-inits the overlay fonts on resolution change; the lazy tier
	// check is that re-init (the policy lives in hud_label_font_choice).
	ensure_label_fonts_(surface.x);
	const bool timing = draw_timing_enabled_;
	Time *clock = timing ? Time::get_singleton() : nullptr;
	const uint64_t t0 = timing ? clock->get_ticks_usec() : 0;
	const HudDrawList &list = compiler_.compile(state_, surface.x, surface.y);
	const uint64_t t1 = timing ? clock->get_ticks_usec() : 0;
	render_list_(list);
	if (timing) {
		const uint64_t t2 = clock->get_ticks_usec();
		draw_compile_us_ += static_cast<int64_t>(t1 - t0);
		draw_emit_us_ += static_cast<int64_t>(t2 - t1);
	}
}

void HudOverlay::set_draw_timing_enabled(bool p_enabled) {
	if (draw_timing_enabled_ == p_enabled) {
		return;
	}
	draw_timing_enabled_ = p_enabled;
	draw_compile_us_ = 0;
	draw_emit_us_ = 0;
}

PackedInt64Array HudOverlay::consume_draw_timing_us() {
	PackedInt64Array out;
	out.resize(2);
	out.set(0, draw_compile_us_);
	out.set(1, draw_emit_us_);
	draw_compile_us_ = 0;
	draw_emit_us_ = 0;
	return out;
}

void HudOverlay::render_list_(const HudDrawList &p_list) {
	render_map_(p_list.map, p_list.map_glyphs, false);
	// The M-cycle big map rides its OWN sandwich above the flat HUD and the
	// corner map — retail draws it as a second pass over the whole overlay
	// set. Each pass carries its own glyphs.
	render_map_(p_list.big_map, p_list.big_map_glyphs, true);
	// The flat HUD below the big map, then the gameplay-overlay windows the
	// compiler marked (HudDrawList::top_begin) on the child item above it.
	const auto &top = p_list.top_begin;
	FlatRange below;
	below.quads_end = std::min(top.quads, p_list.quads.size());
	below.tris_end = std::min(top.tris, p_list.tris.size());
	below.lines_end = std::min(top.lines, p_list.lines.size());
	below.glyphs_end = std::min(top.glyphs, p_list.glyphs.size());
	below.underlines_end = std::min(top.underlines, p_list.underlines.size());
	render_flat_(get_canvas_item(), p_list, below);
	FlatRange above;
	above.quads_begin = below.quads_end;
	above.quads_end = p_list.quads.size();
	above.tris_begin = below.tris_end;
	above.tris_end = p_list.tris.size();
	above.lines_begin = below.lines_end;
	above.lines_end = p_list.lines.size();
	above.glyphs_begin = below.glyphs_end;
	above.glyphs_end = p_list.glyphs.size();
	above.underlines_begin = below.underlines_end;
	above.underlines_end = p_list.underlines.size();
	if (above.quads_begin == above.quads_end && above.tris_begin == above.tris_end &&
			above.lines_begin == above.lines_end && above.glyphs_begin == above.glyphs_end &&
			above.underlines_begin == above.underlines_end) {
		return;
	}
	ensure_top_item_();
	render_flat_(top_item_, p_list, above);
}

void HudOverlay::ensure_top_item_() {
	if (top_item_.is_valid()) return;
	RenderingServer *rs = RenderingServer::get_singleton();
	top_item_ = rs->canvas_item_create();
	rs->canvas_item_set_parent(top_item_, get_canvas_item());
	// Above the big-map sandwich (draw indices 5..8).
	rs->canvas_item_set_draw_index(top_item_, 9);
}

void HudOverlay::render_flat_(const RID &p_item, const HudDrawList &p_list, const FlatRange &p_range) {
	RenderingServer *rs = RenderingServer::get_singleton();
	// Kind-grouped submission preserves the compiler's per-kind insertion
	// order and keeps every glyph above the quads (retail draws its text
	// elements over the bars/frames the same walk emitted).
	for (size_t i = p_range.quads_begin; i < p_range.quads_end; ++i) {
		const opennova::hud::HudQuad &quad = p_list.quads[i];
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0);
		const Color color = opennova::color_from_argb(quad.color);
		if (!quad.filled) {
			PackedVector2Array outline;
			outline.push_back(Vector2(quad.x0, quad.y0));
			outline.push_back(Vector2(quad.x1, quad.y0));
			outline.push_back(Vector2(quad.x1, quad.y1));
			outline.push_back(Vector2(quad.x0, quad.y1));
			outline.push_back(Vector2(quad.x0, quad.y0));
			PackedColorArray outline_color;
			outline_color.push_back(color);
			rs->canvas_item_add_polyline(p_item, outline, outline_color, -1.0f);
			continue;
		}
		Ref<Texture2D> tex;
		if (quad.texture >= 0 && quad.texture < kTextureSlots) {
			tex = textures_[static_cast<size_t>(quad.texture)];
		}
		if (quad.additive) {
			// Per-command blend modes do not exist on a CanvasItem: additive
			// rows ride the child item carrying the add material.
			ensure_additive_item_();
			if (tex.is_valid()) {
				rs->canvas_item_add_texture_rect(additive_item_, rect, tex->get_rid(),
						false, color);
			} else {
				rs->canvas_item_add_rect(additive_item_, rect, color);
			}
			continue;
		}
		if (tex.is_valid()) {
			if (quad.u0 != 0.0f || quad.v0 != 0.0f || quad.u1 != 1.0f || quad.v1 != 1.0f) {
				const Vector2 tex_size = tex->get_size();
				rs->canvas_item_add_texture_rect_region(p_item, rect, tex->get_rid(),
						Rect2(quad.u0 * tex_size.x, quad.v0 * tex_size.y,
								(quad.u1 - quad.u0) * tex_size.x,
								(quad.v1 - quad.v0) * tex_size.y),
						color);
			} else {
				rs->canvas_item_add_texture_rect(p_item, rect, tex->get_rid(), false, color);
			}
		} else {
			rs->canvas_item_add_rect(p_item, rect, color);
		}
	}
	// Consecutive texture runs preserve primitive order and per-vertex color.
	// Device submission follows the font batch witness in docs/fonts/fnt-re.md;
	// the compiler still owns all crosshair and glyph geometry.
	for (size_t first = p_range.tris_begin; first < p_range.tris_end;) {
		const int texture = p_list.tris[first].texture;
		size_t end = first + 1;
		while (end < p_range.tris_end && p_list.tris[end].texture == texture) ++end;
		Ref<Texture2D> tex;
		if (texture >= 0 && texture < kTextureSlots) tex = textures_[static_cast<size_t>(texture)];
		const int count = static_cast<int>((end - first) * 3);
		PackedVector2Array points, uvs;
		PackedColorArray colors;
		PackedInt32Array indices;
		points.resize(count);
		uvs.resize(count);
		colors.resize(count);
		indices.resize(count);
		Vector2 *point = points.ptrw(), *uv = uvs.ptrw();
		Color *color = colors.ptrw();
		int32_t *index = indices.ptrw();
		int vertex_index = 0;
		for (size_t i = first; i < end; ++i) {
			const auto &tri = p_list.tris[i];
			const Color modulation = opennova::color_from_argb(tri.color);
			for (const auto *vertex : { &tri.a, &tri.b, &tri.c }) {
				point[vertex_index] = Vector2(vertex->x, vertex->y);
				uv[vertex_index] = Vector2(vertex->u, vertex->v);
				color[vertex_index] = modulation * opennova::color_from_argb(vertex->color);
				index[vertex_index] = vertex_index;
				++vertex_index;
			}
		}
		rs->canvas_item_add_triangle_array(p_item, indices, points, colors, uvs,
				PackedInt32Array(), PackedFloat32Array(), tex.is_valid() ? tex->get_rid() : RID());
		first = end;
	}
	for (size_t i = p_range.lines_begin; i < p_range.lines_end; ++i) {
		const opennova::hud::HudLine &line = p_list.lines[i];
		rs->canvas_item_add_line(p_item, Vector2(line.x0, line.y0), Vector2(line.x1, line.y1),
				opennova::color_from_argb(line.color), line.width);
	}
	// Keep the existing kind order, page order, italic corners, half-pixel
	// offsets and underline layer. Never sort text by texture across runs.
	for (size_t first = p_range.glyphs_begin; first < p_range.glyphs_end;) {
		const uint32_t page = p_list.glyphs[first].page;
		size_t end = first + 1;
		while (end < p_range.glyphs_end && p_list.glyphs[end].page == page) ++end;
		if (page >= page_textures_.size() || page_textures_[page].is_null()) {
			first = end;
			continue;
		}
		const int count = static_cast<int>(end - first);
		PackedVector2Array points, uvs;
		PackedColorArray colors;
		PackedInt32Array indices;
		points.resize(count * 4);
		uvs.resize(count * 4);
		colors.resize(count * 4);
		indices.resize(count * 6);
		Vector2 *point = points.ptrw(), *uv = uvs.ptrw();
		Color *color = colors.ptrw();
		int32_t *index = indices.ptrw();
		for (int i = 0; i < count; ++i) {
			const auto &glyph = p_list.glyphs[first + static_cast<size_t>(i)];
			const int base = i * 4;
			point[base] = Vector2(glyph.x_top_left, glyph.y_top);
			point[base + 1] = Vector2(glyph.x_top_right, glyph.y_top);
			point[base + 2] = Vector2(glyph.x_bottom_right, glyph.y_bottom);
			point[base + 3] = Vector2(glyph.x_bottom_left, glyph.y_bottom);
			uv[base] = Vector2(glyph.u0, glyph.v0);
			uv[base + 1] = Vector2(glyph.u1, glyph.v0);
			uv[base + 2] = Vector2(glyph.u1, glyph.v1);
			uv[base + 3] = Vector2(glyph.u0, glyph.v1);
			const Color modulation = opennova::color_from_argb(glyph.color);
			for (int corner = 0; corner < 4; ++corner) color[base + corner] = modulation;
			static constexpr int corners[] = {0, 1, 2, 0, 2, 3};
			for (int corner = 0; corner < 6; ++corner) index[i * 6 + corner] = base + corners[corner];
		}
		rs->canvas_item_add_triangle_array(p_item, indices, points, colors, uvs,
				PackedInt32Array(), PackedFloat32Array(), page_textures_[page]->get_rid());
		first = end;
	}
	for (size_t i = p_range.underlines_begin; i < p_range.underlines_end; ++i) {
		const opennova::hud::GameFontUnderline &underline = p_list.underlines[i];
		rs->canvas_item_add_line(p_item, Vector2(underline.x0, underline.y),
				Vector2(underline.x1, underline.y), opennova::color_from_argb(underline.color),
				1.0f);
	}
}

void HudOverlay::render_map_(const opennova::hud::HudMapPass &p_map,
		const std::vector<opennova::hud::GameFontQuad> &p_map_glyphs,
		bool p_big) {
	if (!p_map.visible) return;
	ensure_map_materials_();
	const RID additive = additive_material_->get_rid();
	const RID water = minimap_water_material_->get_rid();
	if (p_big) {
		// The whole big-map sandwich sits ABOVE the flat HUD and the corner
		// map: retail draws the M map after the full overlay pass, so bars,
		// chat, and the corner spinmap all disappear under it (only the
		// objectives-family legs draw later — that residual is ledgered on
		// D-HUD-21; witness at hud_frame.h HudDrawList::big_map).
		big_map_.ensure(get_canvas_item(), 5, false, additive, water);
		big_map_.render(p_map, p_map_glyphs, map_pass_textures());
		return;
	}
	// The WHOLE corner-map set draws BEHIND the parent's own commands:
	// retail pushes the map before the friendly-tag and console-message
	// passes, so those overlays paint OVER the corner map (witness at
	// hud_frame.cpp element ordering).
	corner_map_.ensure(get_canvas_item(), 0, true, additive, water);
	corner_map_.render(p_map, p_map_glyphs, map_pass_textures());
}
