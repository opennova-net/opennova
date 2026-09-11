#include "hud/hud_overlay.h"
#include "util/color_convert.h"
#include "hud/hud_draw_list_stats.h"
#include "hud/vehicle_hud_block.h"

#include "hud/hud_pos.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/simulation.h"
#include "terrain/terrain_data.h"
#include "util/axes.h"

#include <formats/def/def.h> // DefVehicleHudBlock (the VEHICLE_HUD block the panel feed reads)
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

HudPosRecord pos_record4(const Vector4i &v) {
	HudPosRecord r;
	r.x = v.x;
	r.y = v.y;
	r.hidden = v.z;
	r.align = v.w;
	r.present = true;
	return r;
}

HudPosRecord pos_record2(const Vector2i &v) {
	HudPosRecord r;
	r.x = v.x;
	r.y = v.y;
	r.present = true;
	return r;
}

HudRectRecord rect_record(const Rect2i &rect) {
	HudRectRecord r;
	r.x = static_cast<float>(rect.position.x);
	r.y = static_cast<float>(rect.position.y);
	r.w = static_cast<float>(rect.size.x);
	r.h = static_cast<float>(rect.size.y);
	r.present = true;
	return r;
}

} // namespace

int HudOverlay::hud_color_index_default() { return opennova::hud::kHudColorIndexDefault; }
int HudOverlay::clamp_hud_color_index(int p_index) { return opennova::hud::clamp_hud_color_index(p_index); }
int HudOverlay::next_hud_color_index(int p_index) { return opennova::hud::next_hud_color_index(p_index); }
int HudOverlay::hud_detail_level_default() { return opennova::hud::kHudDetailLevelDefault; }
int HudOverlay::hud_detail_level_blank() { return opennova::hud::kHudDetailLevelBlank; }
int HudOverlay::clamp_hud_detail_level(int p_level) { return opennova::hud::clamp_hud_detail_level(p_level); }
int HudOverlay::next_hud_detail_level(int p_level) { return opennova::hud::next_hud_detail_level(p_level); }
int HudOverlay::showhud_flags_default() { return static_cast<int>(opennova::hud::kShowHudFlagsDefault); }
int HudOverlay::next_showhud_flags(int p_flags) {
	return static_cast<int>(opennova::hud::next_showhud_flags(static_cast<uint32_t>(p_flags)));
}
int HudOverlay::sight_scale_index_default() { return opennova::hud::kSightScaleIndexDefault; }
int HudOverlay::next_sight_scale_index(int p_index) {
	return opennova::hud::next_sight_scale_index(p_index);
}
HudOverlay::FriendlyTagMode HudOverlay::friendly_tag_mode_default() {
	return static_cast<FriendlyTagMode>(opennova::hud::kFriendlyTagModeDefault);
}
HudOverlay::FriendlyTagMode HudOverlay::next_friendly_tag_mode(FriendlyTagMode p_mode) {
	return static_cast<FriendlyTagMode>(opennova::hud::next_friendly_tag_mode(
			static_cast<opennova::hud::FriendlyTagMode>(p_mode)));
}
float HudOverlay::friendly_tag_lift() { return opennova::hud::kFriendlyTagLiftUnits; }

void HudOverlay::_bind_methods() {
	BIND_ENUM_CONSTANT(SHOWHUD_FLAG_GUN);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_OFF);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_FAR_BRIEF);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_FULL);
	BIND_ENUM_CONSTANT(FRIENDLY_TAGS_BRIEF);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("hud_color_index_default"), &HudOverlay::hud_color_index_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("clamp_hud_color_index", "index"), &HudOverlay::clamp_hud_color_index);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_hud_color_index", "index"), &HudOverlay::next_hud_color_index);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("hud_detail_level_default"), &HudOverlay::hud_detail_level_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("hud_detail_level_blank"), &HudOverlay::hud_detail_level_blank);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("clamp_hud_detail_level", "level"), &HudOverlay::clamp_hud_detail_level);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_hud_detail_level", "level"), &HudOverlay::next_hud_detail_level);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("showhud_flags_default"), &HudOverlay::showhud_flags_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_showhud_flags", "flags"), &HudOverlay::next_showhud_flags);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("sight_scale_index_default"), &HudOverlay::sight_scale_index_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_sight_scale_index", "index"), &HudOverlay::next_sight_scale_index);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("friendly_tag_mode_default"), &HudOverlay::friendly_tag_mode_default);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("next_friendly_tag_mode", "mode"), &HudOverlay::next_friendly_tag_mode);
	ClassDB::bind_static_method("HudOverlay", D_METHOD("friendly_tag_lift"), &HudOverlay::friendly_tag_lift);
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
	ClassDB::bind_method(D_METHOD("set_weapon", "weapon_name", "display_name", "round_type",
								  "clipsize", "rounds_per_icon", "clipgfx_texture", "clipgfx_offset",
								  "rndgfx_texture", "rndgfx_offset", "rndgfx_step"),
			&HudOverlay::set_weapon);
	ClassDB::bind_method(D_METHOD("clear_weapon"), &HudOverlay::clear_weapon);
	ClassDB::bind_method(D_METHOD("push_message", "text"), &HudOverlay::push_message);
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
			D_METHOD("set_scoreboard", "shown", "game_type", "frame_counter", "strings", "sim"),
			&HudOverlay::set_scoreboard);
	ClassDB::bind_method(D_METHOD("set_vehicle_panel", "shown", "block", "stance", "sim"),
			&HudOverlay::set_vehicle_panel);
	ClassDB::bind_method(D_METHOD("push_chat_line", "text", "argb"),
			&HudOverlay::push_chat_line);
	ClassDB::bind_method(D_METHOD("set_end_round_statistics", "shown", "raised",
			"title", "labels", "values"),
			&HudOverlay::set_end_round_statistics);
	ClassDB::bind_method(D_METHOD("set_message_log_shown", "shown"),
			&HudOverlay::set_message_log_shown);
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
	ClassDB::bind_method(D_METHOD("set_friendly_tags", "shown", "camera_xform",
								  "camera_projection", "fog_distance_units", "sim"),
			&HudOverlay::set_friendly_tags);
	ClassDB::bind_method(D_METHOD("set_end_round_overlay", "shown", "top", "bottom",
								  "texts", "ys"),
			&HudOverlay::set_end_round_overlay);
	ClassDB::bind_method(D_METHOD("set_friendly_tag_mode", "mode"),
			&HudOverlay::set_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("get_friendly_tag_mode"),
			&HudOverlay::get_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("set_friendly_tag_env", "fog_distance_units", "speaking_level"),
			&HudOverlay::set_friendly_tag_env);
	ClassDB::bind_method(D_METHOD("set_hud_color_index", "index"),
			&HudOverlay::set_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_hud_color_index"),
			&HudOverlay::get_hud_color_index);
	ClassDB::bind_method(D_METHOD("set_hud_detail_level", "level"),
			&HudOverlay::set_hud_detail_level);
	ClassDB::bind_method(D_METHOD("get_hud_detail_level"),
			&HudOverlay::get_hud_detail_level);
	ClassDB::bind_method(D_METHOD("set_showhud_flags", "flags"),
			&HudOverlay::set_showhud_flags);
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
		if (map_base_item_.is_valid()) rs->free_rid(map_base_item_);
		if (map_add_item_.is_valid()) rs->free_rid(map_add_item_);
		if (map_water_item_.is_valid()) rs->free_rid(map_water_item_);
		if (map_top_item_.is_valid()) rs->free_rid(map_top_item_);
		if (big_map_base_item_.is_valid()) rs->free_rid(big_map_base_item_);
		if (big_map_add_item_.is_valid()) rs->free_rid(big_map_add_item_);
		if (big_map_water_item_.is_valid()) rs->free_rid(big_map_water_item_);
		if (big_map_top_item_.is_valid()) rs->free_rid(big_map_top_item_);
	}
	additive_item_ = RID();
	map_base_item_ = RID();
	map_add_item_ = RID();
	map_water_item_ = RID();
	map_top_item_ = RID();
	big_map_base_item_ = RID();
	big_map_add_item_ = RID();
	big_map_water_item_ = RID();
	big_map_top_item_ = RID();
	map_water_sampling_configured_ = false;
	map_top_sampling_configured_ = false;
	big_map_water_sampling_configured_ = false;
	big_map_top_sampling_configured_ = false;
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
	compiler_.configure_label_fonts(
			label_font_valid_ ? &label_font_ : nullptr,
			label_font_bold_valid_ ? &label_font_bold_ : nullptr,
			label_font_large_valid_ ? &label_font_large_ : nullptr,
			choice.scale, choice.large_scale,
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

	layout_.ammo_count = pos_record4(p_hudpos->get_ammo_count_pos());
	layout_.weapon_name = pos_record4(p_hudpos->get_weapon_name_pos());
	layout_.game_info = pos_record4(p_hudpos->get_game_info_pos());
	layout_.wpd_info = pos_record4(p_hudpos->get_wpd_info_pos());
	layout_.chat_text = pos_record2(p_hudpos->get_chat_text_pos());
	// The chat box's coordinate rows (the chat wrap width `x2 - (x1 - 4)`)
	// are NOT the HUDCHATTEXT anchor: retail's g_hudChatBoxCoords are written
	// by a separate hud.def `chat_message x1 y1 x2 y2` / `sys_message` parser
	// [orig: File_ParseASCIIFile("hud.def", cb, 0x2A5A8EAD) @0x5be210..0x5be228,
	// the callback @0x5bb7a0, stores @0x5bb7d1/@0x5bb7ed/@0x5bb825/@0x5bb841],
	// and JO:CA ships no hud.def — the rows stay 0, the width is 4, and the
	// wrapper returns 1 at the first character, so a retail chat line never
	// wraps. chat_box_present stays false for the same result; a hud.def-equipped
	// title needs a formats/def reader (the file is SCR-encoded, key 0x2A5A8EAD).
	layout_.sys_text = pos_record2(p_hudpos->get_sys_text_pos());
	// LFP_FLAGS — the AAS zone status panel's anchor (retail g_hudZonePanelX/Y).
	{
		const Vector2i lfp = p_hudpos->get_lfp_flags();
		layout_.lfp_anchor_x = lfp.x;
		layout_.lfp_anchor_y = lfp.y;
		layout_.lfp_anchor_present = true;
	}
	// HUDVEHSTANCEPOS — the vehicle panel's base before the stance offset.
	veh_stance_pos_ = p_hudpos->get_veh_stance_pos();
	layout_.clip_pos = pos_record2(p_hudpos->get_clip_pos());
	layout_.stance_pos = pos_record2(p_hudpos->get_stance_pos());
	layout_.health_rect = rect_record(p_hudpos->get_health_rect());
	layout_.heat_rect = rect_record(p_hudpos->get_heat_rect());
	layout_.power_rect = rect_record(p_hudpos->get_powerbar_rect());
	const Rect2i spinmap = p_hudpos->get_spinmap_bounds();
	layout_.spinmap_rect = rect_record(spinmap);
	layout_.spinmap_rect.present = spinmap.size.x > 0 && spinmap.size.y > 0;
	layout_.spinmap_wp_dist_off = p_hudpos->get_spinmap_wp_dist_off();
	const Vector3i map_coords = p_hudpos->get_map_coords();
	layout_.map_coords_x = static_cast<float>(map_coords.x);
	layout_.map_coords_y = static_cast<float>(map_coords.y);
	layout_.map_coords_off = map_coords.z;

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

	const Dictionary colors = p_hudpos->get_colors();
	const auto color_of = [&colors](const char *key, uint32_t fallback) {
		if (!colors.has(key)) {
			return fallback;
		}
		return opennova::argb_from_color(colors[key]);
	};
	layout_.health_border = color_of("health_border", layout_.health_border);
	layout_.tag_good = color_of("tagcolor_good", layout_.tag_good);
	layout_.tag_middle = color_of("tagcolor_middle", layout_.tag_middle);
	layout_.tag_bad = color_of("tagcolor_bad", layout_.tag_bad);
	layout_.hud_text = color_of("hud_textcolor", layout_.hud_text);
	layout_.weapon_text = color_of("weapon_textcolor", layout_.weapon_text);
	layout_.stance_tint = color_of("stanceicon_color", layout_.stance_tint);
	layout_.heat_border = color_of("heat_border", layout_.heat_border);
	// The whole stance colour triple: the vehicle panel bands its seats with the
	// same three the stance bar reads [orig: the good/middle/bad arms of the
	// seat loop in HUD_DrawVehicleHealthBars @0x5a4fd0, see docs/interface/hud-re.md].
	layout_.stance_good = color_of("stancecolor_good", layout_.stance_good);
	layout_.stance_middle = color_of("stancecolor_middle", layout_.stance_middle);
	layout_.stance_bad = color_of("stancecolor_bad", layout_.stance_bad);

	const Vector3 fade = p_hudpos->get_alpha_fade();
	layout_.alpha_fade_base = fade.x;
	layout_.alpha_fade_max = fade.y;
	layout_.alpha_fade_seconds = fade.z;
	const int chline = p_hudpos->get_hud_chline();
	layout_.chat_lines = chline > 0 ? chline : 8;

	// Static HUD frame background. Retail keeps ONE static frame and the LAST
	// authored line wins, so the index comes from the engine-side policy rather
	// than being assumed here [orig: HUD_ParseHudposToken @0x59F370, see
	// engine/runtime/hud/hud_frame.h].
	const Array frames = p_hudpos->get_static_frames();
	const int frame_index =
			opennova::hud::hud_static_frame_index(static_cast<int>(frames.size()));
	if (frame_index >= 0) {
		const Dictionary frame = frames[frame_index];
		const Vector2i pos = frame.get("pos", Vector2i());
		layout_.frame_pos = pos_record2(pos);
		const Ref<Texture2D> tex = load_hud_texture_(frame.get("texture", String()));
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

	// HUDSTANCE's explicit id addresses the retail slot arrays; file order is
	// irrelevant and a later record for the same id replaces the earlier one.
	String stance_names[6];
	const Array stances = p_hudpos->get_stances();
	for (int64_t i = 0; i < stances.size(); ++i) {
		const Dictionary stance = stances[i];
		const int id = stance.get("id", -1);
		if (id < 0 || id >= 6) {
			continue;
		}
		stance_names[id] = String(stance.get("texture", String()));
		const Vector2i offset = stance.get("offset", Vector2i());
		layout_.stance_offset_x[static_cast<size_t>(id)] = offset.x;
		layout_.stance_offset_y[static_cast<size_t>(id)] = offset.y;
	}
	for (int i = 0; i < 6; ++i) {
		const Ref<Texture2D> tex = load_hud_texture_(stance_names[i]);
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
	// Slot kHudTexMapRadar stays empty: the radar-contacts sector-slice leg
	// is unported (D-HUD-21) and nothing emits its texture index yet.
	textures_[opennova::hud::kHudTexMapWpIndicator] =
			load_hud_texture_("WPIndctr.tga");

	// The HUD font named by hudpos (hi first, lo fallback), parsed by the
	// engine fnt lib; page bitmaps become one texture each for glyph quads.
	String font_name = p_hudpos->get_font_hi();
	if (font_name.is_empty()) {
		font_name = p_hudpos->get_font_lo();
	}
	font_valid_ = load_fnt_(font_name, font_, opennova::hud::kHudFontSlotHud);

	configured_ = true;
	compiler_.configure(layout_, font_valid_ ? &font_ : nullptr);
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
		const String &p_round_type, int p_clipsize, int p_rounds_per_icon,
		const String &p_clipgfx_texture, const Vector2i &p_clipgfx_offset,
		const String &p_rndgfx_texture, const Vector2i &p_rndgfx_offset,
		const Vector2i &p_rndgfx_step) {
	(void)p_weapon_name; // identity lives presenter-side; the state is the HUD slice
	opennova::hud::HudWeaponState &wep = state_.weapon;
	const bool was_active = wep.active;
	wep = opennova::hud::HudWeaponState{};
	wep.active = was_active;
	wep.display_name = p_display_name.utf8().get_data();
	wep.round_type = p_round_type.utf8().get_data();
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

void HudOverlay::push_message(const String &p_text) {
	compiler_.push_message(p_text.utf8().get_data(), state_.ticks);
	queue_redraw();
}

void HudOverlay::push_feed_line(const String &p_text, int64_t p_argb) {
	// The SYSTEM feed sink (kills, joins, system lines) — the packed ARGB is
	// stored raw and drawn as stored [orig: the stored-color read @0x59ae97].
	compiler_.push_feed_line(p_text.utf8().get_data(),
			static_cast<uint32_t>(p_argb), state_.ticks);
	queue_redraw();
}

void HudOverlay::set_player_state(int p_ticks, float p_health_fraction, int p_stance,
		float p_fov_deg) {
	state_.ticks = p_ticks;
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

void HudOverlay::set_view_state(bool p_binoculars_view_active, const Vector2 &p_aim_screen) {
	state_.binoculars_view_active = p_binoculars_view_active;
	state_.aim_valid = p_aim_screen.is_finite();
	state_.aim_screen_x = state_.aim_valid ? p_aim_screen.x : 0.0f;
	state_.aim_screen_y = state_.aim_valid ? p_aim_screen.y : 0.0f;
	queue_redraw();
}

void HudOverlay::set_objectives_header(const String &p_text) {
	state_.objectives_header = p_text.utf8().get_data();
}

void HudOverlay::set_scoreboard(bool p_shown, int64_t p_game_type, int p_frame_counter,
		const Dictionary &p_strings, const Ref<Simulation> &p_sim) {
	opennova::hud::HudScoreboardState &sb = state_.scoreboard;
	sb.shown = p_shown;
	sb.game_type = static_cast<uint32_t>(p_game_type);
	// The 4-team page clock: the shell's 62 Hz HUD tick, the same fold (and the
	// same frame-rate caveat) as the LFP panel's blink counter below.
	sb.frame_counter = p_frame_counter;
	sb.title = String(p_strings.get("title", "")).utf8().get_data();
	sb.server_name = String(p_strings.get("server", "")).utf8().get_data();
	sb.mission_title = String(p_strings.get("mission", "")).utf8().get_data();
	sb.game_type_label = String(p_strings.get("game_type", "")).utf8().get_data();
	sb.players_line = String(p_strings.get("players", "")).utf8().get_data();
	sb.spectators_line = String(p_strings.get("spectators", "")).utf8().get_data();
	sb.footer = String(p_strings.get("footer", "")).utf8().get_data();
	// Rows come straight from the netsim projection — no script-side
	// Dictionary round-trip to drop fields or lose the score sign.
	if (p_shown && p_sim.is_valid()) {
		p_sim->fill_scoreboard_rows(sb.rows);
		sb.team_count = p_sim->scoreboard_team_count();
	} else {
		sb.rows.clear();
		sb.team_count = 0;
	}
	queue_redraw();
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
		line.text = p_texts[i].utf8().get_data();
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
	st.title = p_title.utf8().get_data();
	for (int64_t i = 0; i < 4; ++i) {
		st.labels[i] = i < p_labels.size()
				? std::string(p_labels[i].utf8().get_data()) : std::string();
		st.values[i] = i < p_values.size()
				? std::string(p_values[i].utf8().get_data()) : std::string();
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
	vp.anchor_x = veh_stance_pos_.x;
	vp.anchor_y = veh_stance_pos_.y;
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
	compiler_.push_chat_line(p_text.utf8().get_data(),
			static_cast<uint32_t>(p_argb), state_.ticks);
	queue_redraw();
}

void HudOverlay::set_message_log_shown(bool p_shown) {
	state_.message_log_shown = p_shown;
	queue_redraw();
}

void HudOverlay::set_message_log_title(const String &p_title) {
	state_.message_log_title = p_title.utf8().get_data();
	queue_redraw();
}

void HudOverlay::set_lfp_panel(bool p_shown, int64_t p_game_type, int p_local_team,
		int p_frame_counter, const Dictionary &p_strings, const Ref<Simulation> &p_sim) {
	opennova::hud::HudLfpPanelState &lp = state_.lfp_panel;
	lp.local_team = p_local_team;
	// The blink clock the marker masks (`& 0x18`). The shell feeds the 62 Hz
	// HUD tick here; retail's g_hudFrameCounter increments once per MAIN FRAME
	// [orig: Game_ProcessMainFrame @0x5265d5 -> Game_TickHudFrameCounters
	// @0x434c23, see docs/interface/hud-re.md], so retail's blink is
	// frame-rate dependent and matches this fold only at 62 fps.
	lp.frame_counter = p_frame_counter;
	// The conquest arm is the other branch of the same drawer and is
	// unmodelled [orig: g_GameType == 0x50010 @0x5a24a1].
	lp.conquest_mode = static_cast<uint32_t>(p_game_type) ==
			opennova::game_type::kConquerAndControl;
	lp.under_attack_text =
			String(p_strings.get("under_attack", "")).utf8().get_data();
	lp.ready_text = String(p_strings.get("ready", "")).utf8().get_data();
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
	state_.waypoint.name = p_name.utf8().get_data();
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
			// ctrlx/drvrx share the Control label [orig: g_hudLabelTextControl @0x5a34db/0x5a34fb]
			return overlay_text(p_gametext, "STROVER_CONTROL", "!Control");
		case SeatType::Gunner: // UseGun [orig: def+0x3A0 else dword_2723868]
			if (p_attach_text_key.empty()) {
				return overlay_text(p_gametext, "STROVER_USEGUN", "!UseGun");
			}
			// the witnessed empty-label quirk (parse-miss stores "")
			return overlay_text(p_gametext, String::utf8(p_attach_text_key.c_str()), "");
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

// [orig: draw_vehicle_seat_and_armory_labels @0x5a3290 — the projection
//  Math_FixedPointTransformPoint22 + clip_point_to_frustum_and_project @0x5a3655]
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
				continue; // [orig: clip_point_to_frustum_and_project nonzero = clipped @0x5a3655]
			}
			opennova::hud::HudAttachLabel label;
			label.screen_x = screen.x;
			label.screen_y = screen.y;
			label.text = attach_label_text(p_gametext, l.type, l.attach_text_key).utf8().get_data();
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
	return String::utf8(state_.attach_labels[static_cast<size_t>(p_index)].text.c_str());
}

// [orig: HUD_DrawFriendlyTagsPass @0x5a4480 -> HUD_DrawEntityLabel @0x5a39b0 —
//  distance @0x5a3aba, projection Math_FixedPointTransformPoint22 +
//  clip_point_to_frustum_and_project @0x5a3b47, fog Env_FogDistCurrent
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
	// cycle/death-force policy.
	declutter_.set_level(CLAMP(p_level, 0, opennova::hud::kDeclutterLevelMax));
	apply_declutter_();
	queue_redraw();
}

int HudOverlay::get_hud_detail_level() const {
	return declutter_.level();
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
	int64_t map_backing_tris = 0;
	int64_t map_terrain_tris = 0;
	int64_t map_footprint_tris = 0;
	int64_t map_sprites = 0;
	int64_t map_lines_under = 0;
	int64_t map_lines = 0;
	int64_t map_labels = 0;
	bool big_map_visible = false;
	int64_t big_map_backing_tris = 0;
	int64_t big_map_terrain_tris = 0;
	int64_t big_map_footprint_tris = 0;
	int64_t big_map_sprites = 0;
	int64_t big_map_lines_under = 0;
	int64_t big_map_lines = 0;
	int64_t big_map_labels = 0;
	int64_t big_map_glyphs = 0;
	if (configured_) {
		const Vector2 surface = draw_surface_();
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
		map_backing_tris = static_cast<int64_t>(list.map.backing.size());
		map_terrain_tris = static_cast<int64_t>(list.map.terrain.size());
		map_footprint_tris = static_cast<int64_t>(list.map.overlays.size());
		map_sprites = static_cast<int64_t>(list.map.sprites.size());
		map_lines_under = static_cast<int64_t>(list.map.lines_under.size());
		map_lines = static_cast<int64_t>(list.map.lines.size());
		map_labels = static_cast<int64_t>(list.map.labels.size());
		big_map_visible = list.big_map.visible;
		big_map_backing_tris = static_cast<int64_t>(list.big_map.backing.size());
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
	out->set_map_backing_tris(map_backing_tris);
	out->set_map_terrain_tris(map_terrain_tris);
	out->set_map_footprint_tris(map_footprint_tris);
	out->set_map_sprites(map_sprites);
	out->set_map_lines_under(map_lines_under);
	out->set_map_lines(map_lines);
	out->set_map_labels(map_labels);
	out->set_big_map_visible(big_map_visible);
	out->set_big_map_backing_tris(big_map_backing_tris);
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
	out->set_map_texture_filter(map_top_sampling_configured_
			? static_cast<int64_t>(
					RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_map_texture_repeat(map_top_sampling_configured_
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	out->set_map_water_texture_filter(map_water_sampling_configured_
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_map_water_texture_repeat(map_water_sampling_configured_
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	out->set_big_map_texture_filter(big_map_top_sampling_configured_
			? static_cast<int64_t>(
					RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_big_map_texture_repeat(big_map_top_sampling_configured_
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DEFAULT));
	out->set_big_map_water_texture_filter(big_map_water_sampling_configured_
			? static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR)
			: static_cast<int64_t>(RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_DEFAULT));
	out->set_big_map_water_texture_repeat(big_map_water_sampling_configured_
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

void HudOverlay::ensure_map_items_() {
	if (map_base_item_.is_valid() && map_add_item_.is_valid() &&
			map_water_item_.is_valid() && map_top_item_.is_valid()) {
		return;
	}
	if (additive_material_.is_null()) {
		additive_material_.instantiate();
		additive_material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
	}
	ensure_minimap_water_material_();
	RenderingServer *rs = RenderingServer::get_singleton();
	// The retail map decal pipeline quadruples texture x diffuse (the
	// captures measure exactly 0x60 x 4 = 1.5058 x texture). A 1x canvas
	// cannot express >1 modulate, so the terrain draws twice — base + an
	// additive child-item pass — which saturates identically. Everything
	// the map draws ABOVE its terrain rides a third child so the sandwich
	// keeps retail's order. The WHOLE corner-map trio draws BEHIND the
	// parent's own commands: retail pushes the map before the friendly-tag
	// and console-message passes, so those overlays paint OVER the corner
	// map (witness at hud_frame.cpp element ordering and hud_minimap.cpp
	// kTerrainTint; hud-re.md carries the pass addresses). Per-command
	// blend modes do not exist on a CanvasItem.
	if (!map_base_item_.is_valid()) {
		map_base_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(map_base_item_, get_canvas_item());
		rs->canvas_item_set_draw_behind_parent(map_base_item_, true);
		rs->canvas_item_set_draw_index(map_base_item_, 0);
	}
	if (!map_add_item_.is_valid()) {
		map_add_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(map_add_item_, get_canvas_item());
		rs->canvas_item_set_material(map_add_item_,
				additive_material_->get_rid());
		rs->canvas_item_set_draw_behind_parent(map_add_item_, true);
		rs->canvas_item_set_draw_index(map_add_item_, 1);
	}
	if (!map_water_item_.is_valid()) {
		map_water_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(map_water_item_, get_canvas_item());
		rs->canvas_item_set_material(map_water_item_,
				minimap_water_material_->get_rid());
		rs->canvas_item_set_draw_behind_parent(map_water_item_, true);
		rs->canvas_item_set_draw_index(map_water_item_, 2);
		rs->canvas_item_set_default_texture_filter(map_water_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
		rs->canvas_item_set_default_texture_repeat(map_water_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		map_water_sampling_configured_ = true;
	}
	if (!map_top_item_.is_valid()) {
		map_top_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(map_top_item_, get_canvas_item());
		rs->canvas_item_set_draw_behind_parent(map_top_item_, true);
		rs->canvas_item_set_draw_index(map_top_item_, 3);
		rs->canvas_item_set_default_texture_filter(map_top_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
		rs->canvas_item_set_default_texture_repeat(map_top_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		map_top_sampling_configured_ = true;
	}
}

void HudOverlay::ensure_big_map_items_() {
	if (big_map_base_item_.is_valid() && big_map_add_item_.is_valid() &&
			big_map_water_item_.is_valid() && big_map_top_item_.is_valid()) {
		return;
	}
	if (additive_material_.is_null()) {
		additive_material_.instantiate();
		additive_material_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
	}
	ensure_minimap_water_material_();
	RenderingServer *rs = RenderingServer::get_singleton();
	// The whole big-map sandwich sits ABOVE the flat HUD and the corner
	// map: retail draws the M map after the full overlay pass, so bars,
	// chat, and the corner spinmap all disappear under it (only the
	// objectives-family legs draw later — that residual is ledgered on
	// D-HUD-21; witness at hud_frame.h HudDrawList::big_map).
	if (!big_map_base_item_.is_valid()) {
		big_map_base_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(big_map_base_item_, get_canvas_item());
		rs->canvas_item_set_draw_index(big_map_base_item_, 5);
	}
	if (!big_map_add_item_.is_valid()) {
		big_map_add_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(big_map_add_item_, get_canvas_item());
		rs->canvas_item_set_material(big_map_add_item_,
				additive_material_->get_rid());
		rs->canvas_item_set_draw_index(big_map_add_item_, 6);
	}
	if (!big_map_water_item_.is_valid()) {
		big_map_water_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(big_map_water_item_, get_canvas_item());
		rs->canvas_item_set_material(big_map_water_item_,
				minimap_water_material_->get_rid());
		rs->canvas_item_set_draw_index(big_map_water_item_, 7);
		rs->canvas_item_set_default_texture_filter(big_map_water_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
		rs->canvas_item_set_default_texture_repeat(big_map_water_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		big_map_water_sampling_configured_ = true;
	}
	if (!big_map_top_item_.is_valid()) {
		big_map_top_item_ = rs->canvas_item_create();
		rs->canvas_item_set_parent(big_map_top_item_, get_canvas_item());
		rs->canvas_item_set_draw_index(big_map_top_item_, 8);
		rs->canvas_item_set_default_texture_filter(big_map_top_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
		rs->canvas_item_set_default_texture_repeat(big_map_top_item_,
				RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
		big_map_top_sampling_configured_ = true;
	}
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
	if (map_base_item_.is_valid()) {
		rs->canvas_item_clear(map_base_item_);
	}
	if (map_add_item_.is_valid()) {
		rs->canvas_item_clear(map_add_item_);
	}
	if (map_water_item_.is_valid()) {
		rs->canvas_item_clear(map_water_item_);
	}
	if (map_top_item_.is_valid()) {
		rs->canvas_item_clear(map_top_item_);
	}
	if (big_map_base_item_.is_valid()) {
		rs->canvas_item_clear(big_map_base_item_);
	}
	if (big_map_add_item_.is_valid()) {
		rs->canvas_item_clear(big_map_add_item_);
	}
	if (big_map_water_item_.is_valid()) {
		rs->canvas_item_clear(big_map_water_item_);
	}
	if (big_map_top_item_.is_valid()) {
		rs->canvas_item_clear(big_map_top_item_);
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
	RenderingServer *rs = RenderingServer::get_singleton();
	render_map_(p_list.map, p_list.map_glyphs, false);
	// The M-cycle big map rides its OWN sandwich above the flat HUD and the
	// corner map — retail draws it as a second pass over the whole overlay
	// set. Each pass carries its own glyphs.
	render_map_(p_list.big_map, p_list.big_map_glyphs, true);
	// Kind-grouped submission preserves the compiler's per-kind insertion
	// order and keeps every glyph above the quads (retail draws its text
	// elements over the bars/frames the same walk emitted).
	for (const opennova::hud::HudQuad &quad : p_list.quads) {
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0);
		const Color color = opennova::color_from_argb(quad.color);
		if (!quad.filled) {
			draw_rect(rect, color, false, -1.0f);
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
				draw_texture_rect_region(tex, rect,
						Rect2(quad.u0 * tex_size.x, quad.v0 * tex_size.y,
								(quad.u1 - quad.u0) * tex_size.x,
								(quad.v1 - quad.v0) * tex_size.y),
						color);
			} else {
				draw_texture_rect(tex, rect, false, color);
			}
		} else {
			draw_rect(rect, color, true);
		}
	}
	for (const opennova::hud::HudTri &tri : p_list.tris) {
		Ref<Texture2D> tex;
		if (tri.texture >= 0 && tri.texture < kTextureSlots) {
			tex = textures_[static_cast<size_t>(tri.texture)];
		}
		PackedVector2Array points;
		points.resize(3);
		points.set(0, Vector2(tri.a.x, tri.a.y));
		points.set(1, Vector2(tri.b.x, tri.b.y));
		points.set(2, Vector2(tri.c.x, tri.c.y));
		PackedVector2Array uvs;
		uvs.resize(3);
		uvs.set(0, Vector2(tri.a.u, tri.a.v));
		uvs.set(1, Vector2(tri.b.u, tri.b.v));
		uvs.set(2, Vector2(tri.c.u, tri.c.v));
		PackedColorArray colors;
		colors.push_back(opennova::color_from_argb(tri.color));
		draw_polygon(points, colors, uvs, tex);
	}
	for (const opennova::hud::HudLine &line : p_list.lines) {
		draw_line(Vector2(line.x0, line.y0), Vector2(line.x1, line.y1),
				opennova::color_from_argb(line.color), line.width);
	}
	// Glyph quads carry explicit corner geometry (the italic pass shears the
	// top edge), so each renders as a polygon over its page texture, keeping
	// the engine's -0.5 vertex offsets as-is.
	for (const opennova::hud::GameFontQuad &glyph : p_list.glyphs) {
		if (glyph.page >= page_textures_.size()) {
			continue;
		}
		const Ref<Texture2D> page = page_textures_[glyph.page];
		if (page.is_null()) {
			continue;
		}
		PackedVector2Array points;
		points.resize(4);
		points.set(0, Vector2(glyph.x_top_left, glyph.y_top));
		points.set(1, Vector2(glyph.x_top_right, glyph.y_top));
		points.set(2, Vector2(glyph.x_bottom_right, glyph.y_bottom));
		points.set(3, Vector2(glyph.x_bottom_left, glyph.y_bottom));
		PackedVector2Array uvs;
		uvs.resize(4);
		uvs.set(0, Vector2(glyph.u0, glyph.v0));
		uvs.set(1, Vector2(glyph.u1, glyph.v0));
		uvs.set(2, Vector2(glyph.u1, glyph.v1));
		uvs.set(3, Vector2(glyph.u0, glyph.v1));
		PackedColorArray colors;
		colors.push_back(opennova::color_from_argb(glyph.color));
		draw_polygon(points, colors, uvs, page);
	}
	for (const opennova::hud::GameFontUnderline &underline : p_list.underlines) {
		draw_line(Vector2(underline.x0, underline.y), Vector2(underline.x1, underline.y),
				opennova::color_from_argb(underline.color), 1.0f);
	}
}

void HudOverlay::render_map_(const opennova::hud::HudMapPass &p_map,
		const std::vector<opennova::hud::GameFontQuad> &p_map_glyphs,
		bool p_big) {
	if (!p_map.visible) return;
	RID base_item, add_item, water_item, top_item;
	if (p_big) {
		ensure_big_map_items_();
		base_item = big_map_base_item_;
		add_item = big_map_add_item_;
		water_item = big_map_water_item_;
		top_item = big_map_top_item_;
	} else {
		ensure_map_items_();
		// The whole corner-map trio draws behind the parent's own commands
		// (retail pushes the map before the tag/message passes).
		base_item = map_base_item_;
		add_item = map_add_item_;
		water_item = map_water_item_;
		top_item = map_top_item_;
	}
	RenderingServer *rs = RenderingServer::get_singleton();

	// One triangle-array submission per (item, texture) group: the per-frame
	// map redraw must not request one RenderingServer polygon per triangle
	// (each polygon is its own GPU buffer request — the per-tri form measured
	// in whole milliseconds on missions with dense footprint sets).
	PackedVector2Array points, uvs;
	PackedColorArray colors;
	PackedInt32Array indices;
	const auto flush_tris = [&](const RID &item, const Ref<Texture2D> &tex) {
		if (indices.is_empty()) return;
		rs->canvas_item_add_triangle_array(item, indices, points, colors, uvs,
				PackedInt32Array(), PackedFloat32Array(),
				tex.is_valid() ? tex->get_rid() : RID());
		points.clear();
		uvs.clear();
		colors.clear();
		indices.clear();
	};
	const auto push_map_tri = [&](const opennova::hud::HudMapTri &tri) {
		const int base = static_cast<int>(points.size());
		points.push_back(Vector2(tri.a.x, tri.a.y));
		points.push_back(Vector2(tri.b.x, tri.b.y));
		points.push_back(Vector2(tri.c.x, tri.c.y));
		uvs.push_back(Vector2(tri.a.u, tri.a.v));
		uvs.push_back(Vector2(tri.b.u, tri.b.v));
		uvs.push_back(Vector2(tri.c.u, tri.c.v));
		const Color color = opennova::color_from_argb(tri.color);
		colors.push_back(color);
		colors.push_back(color);
		colors.push_back(color);
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
	};
	const auto push_quad = [&](const Vector2 *corner, const Vector2 *uv,
			uint32_t argb) {
		const int base = static_cast<int>(points.size());
		const Color color = opennova::color_from_argb(argb);
		for (int i = 0; i < 4; ++i) {
			points.push_back(corner[i]);
			uvs.push_back(uv[i]);
			colors.push_back(color);
		}
		indices.push_back(base);
		indices.push_back(base + 1);
		indices.push_back(base + 2);
		indices.push_back(base);
		indices.push_back(base + 2);
		indices.push_back(base + 3);
	};

	const Ref<Texture2D> empty_texture;
	for (const opennova::hud::HudMapTri &tri : p_map.backing)
		push_map_tri(tri);
	flush_tris(base_item, empty_texture);

	const Ref<Texture2D> terrain_texture =
			textures_[opennova::hud::kHudTexMapTerrain];
	for (const opennova::hud::HudMapTri &tri : p_map.terrain)
		push_map_tri(tri);
	if (!indices.is_empty()) {
		// Second (additive) half of the x4 output stage — see
		// ensure_map_items_. The additive item repeats the same arrays.
		rs->canvas_item_add_triangle_array(add_item, indices, points, colors,
				uvs, PackedInt32Array(), PackedFloat32Array(),
				terrain_texture.is_valid() ? terrain_texture->get_rid()
						: RID());
	}
	flush_tris(base_item, terrain_texture);

	// Retail's alpha-tested depthspin draw is opaque and follows the completed
	// terrain output. Canvas splits that output across base/additive items, so
	// the sampled height cutoff rides its own shader item after both; putting
	// it in either terrain item lets the later leg add terrain back over water.
	const Ref<Texture2D> water_texture =
			textures_[opennova::hud::kHudTexMapWater];
	for (const opennova::hud::HudMapTri &tri : p_map.terrain_water)
		push_map_tri(tri);
	flush_tris(water_item, water_texture);

	// The under-layer lines (the 300-wu grid rules) draw FIRST on the top
	// item: retail's grid branch runs before the marker walk, so the
	// buildings-first footprint fills paint OVER the rules (witness at
	// hud_minimap.h HudMapPass::lines_under).
	const auto submit_lines = [&](const std::vector<opennova::hud::HudMapLine>
			&lines) {
		if (lines.empty()) return;
		PackedVector2Array line_points;
		PackedColorArray line_colors;
		line_points.resize(static_cast<int64_t>(lines.size()) * 2);
		line_colors.resize(static_cast<int64_t>(lines.size()));
		int64_t li = 0;
		for (const opennova::hud::HudMapLine &line : lines) {
			line_points.set(li * 2, Vector2(line.x0, line.y0));
			line_points.set(li * 2 + 1, Vector2(line.x1, line.y1));
			line_colors.set(li, opennova::color_from_argb(line.color));
			++li;
		}
		rs->canvas_item_add_multiline(top_item, line_points, line_colors, 1.0f);
	};
	submit_lines(p_map.lines_under);

	// Footprint fills draw in the MARKER-WALK slot: retail's building fills
	// blend their ctx alpha over the terrain AFTER the decal's x4 output
	// stage completes, so here they must ride the TOP item — anything on
	// the base item gets the additive child's terrain resubmission summed
	// on top of it (that ordering mistake read near-white; the reference
	// capture measures retail's fill band at ~147..158 per channel over
	// ground, i.e. the 0xD0-alpha gray over the finished doubled terrain).
	// They paint over the grid rules and before the icon sprites, like
	// retail's grid-branch-then-buildings-first walk (witness at
	// world::minimap_footprint_fill_argb).
	for (const opennova::hud::HudMapTri &tri : p_map.overlays)
		push_map_tri(tri);
	flush_tris(top_item, empty_texture);

	// Sprites batch by consecutive texture slot (insertion order is the
	// compiler layer order, so only same-texture runs may merge).
	int run_texture_slot = -1;
	Ref<Texture2D> run_texture;
	for (const opennova::hud::HudMapSprite &sprite : p_map.sprites) {
		const int texture_slot = opennova::hud::kHudTexMapIcons + sprite.texture;
		if (texture_slot != run_texture_slot) {
			flush_tris(top_item, run_texture);
			run_texture_slot = texture_slot;
			run_texture = texture_slot >= 0 && texture_slot < kTextureSlots
					? textures_[static_cast<size_t>(texture_slot)]
					: Ref<Texture2D>();
		}
		const float c = std::cos(sprite.rotation_rad);
		const float s = std::sin(sprite.rotation_rad);
		auto corner = [&](float x, float y) {
			return Vector2(sprite.center_x + x * c - y * s,
					sprite.center_y + x * s + y * c);
		};
		const Vector2 corners[4] = {
			corner(-sprite.half_w, -sprite.half_h),
			corner(sprite.half_w, -sprite.half_h),
			corner(sprite.half_w, sprite.half_h),
			corner(-sprite.half_w, sprite.half_h),
		};
		const Vector2 quad_uvs[4] = {
			Vector2(sprite.u0, sprite.v0),
			Vector2(sprite.u1, sprite.v0),
			Vector2(sprite.u1, sprite.v1),
			Vector2(sprite.u0, sprite.v1),
		};
		push_quad(corners, quad_uvs, sprite.color);
	}
	flush_tris(top_item, run_texture);

	submit_lines(p_map.lines);

	// Map text: element_spinmap compiles per-pass GameFont glyph quads; they
	// render LAST on this pass top item, above its grid rules and markers.
	// Batched by consecutive font page.
	uint32_t run_page = 0xFFFFFFFFu;
	Ref<Texture2D> run_page_texture;
	for (const opennova::hud::GameFontQuad &glyph : p_map_glyphs) {
		if (glyph.page >= page_textures_.size()) continue;
		if (glyph.page != run_page) {
			flush_tris(top_item, run_page_texture);
			run_page = glyph.page;
			run_page_texture = page_textures_[glyph.page];
		}
		if (run_page_texture.is_null()) continue;
		const Vector2 corners[4] = {
			Vector2(glyph.x_top_left, glyph.y_top),
			Vector2(glyph.x_top_right, glyph.y_top),
			Vector2(glyph.x_bottom_right, glyph.y_bottom),
			Vector2(glyph.x_bottom_left, glyph.y_bottom),
		};
		const Vector2 quad_uvs[4] = {
			Vector2(glyph.u0, glyph.v0),
			Vector2(glyph.u1, glyph.v0),
			Vector2(glyph.u1, glyph.v1),
			Vector2(glyph.u0, glyph.v1),
		};
		push_quad(corners, quad_uvs, glyph.color);
	}
	flush_tris(top_item, run_page_texture);
}
