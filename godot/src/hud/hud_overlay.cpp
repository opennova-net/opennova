#include "hud_overlay.h"

#include "nova_hud_pos.h"
#include "resource_index/nova_resource_root.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/rect2.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace godot;

namespace {

using opennova::hud::HudDrawList;
using opennova::hud::HudLayout;
using opennova::hud::HudPosRecord;
using opennova::hud::HudRectRecord;

Color argb_to_color(uint32_t argb) {
	return Color(((argb >> 16) & 0xFFu) / 255.0f, ((argb >> 8) & 0xFFu) / 255.0f,
			(argb & 0xFFu) / 255.0f, ((argb >> 24) & 0xFFu) / 255.0f);
}

uint32_t color_to_argb(const Color &c) {
	const auto channel = [](float v) {
		const int b = static_cast<int>(v * 255.0f + 0.5f);
		return static_cast<uint32_t>(std::clamp(b, 0, 255));
	};
	return (channel(c.a) << 24) | (channel(c.r) << 16) | (channel(c.g) << 8) |
			channel(c.b);
}

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

void HudOverlay::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure", "hudpos", "root"), &HudOverlay::configure);
	ClassDB::bind_method(D_METHOD("is_configured"), &HudOverlay::is_configured);
	ClassDB::bind_method(D_METHOD("set_crosshair_style", "style"), &HudOverlay::set_crosshair_style);
	ClassDB::bind_method(D_METHOD("get_crosshair_style"), &HudOverlay::get_crosshair_style);
	ClassDB::bind_method(D_METHOD("set_weapon", "weapon_name", "display_name", "round_type",
								  "clipsize", "rounds_per_icon", "clipgfx_texture", "clipgfx_offset",
								  "rndgfx_texture", "rndgfx_offset", "rndgfx_step"),
			&HudOverlay::set_weapon);
	ClassDB::bind_method(D_METHOD("clear_weapon"), &HudOverlay::clear_weapon);
	ClassDB::bind_method(D_METHOD("push_message", "text"), &HudOverlay::push_message);
	ClassDB::bind_method(D_METHOD("set_player_state", "ticks", "health_fraction", "stance", "fov_deg"),
			&HudOverlay::set_player_state);
	ClassDB::bind_method(D_METHOD("set_weapon_state", "active", "clip", "reserve", "heat",
								  "hud_spread_fp16", "aimed_shot_available",
								  "keep_crosshair_while_aimed", "windup_active", "windup_held_ticks"),
			&HudOverlay::set_weapon_state);
	ClassDB::bind_method(D_METHOD("set_view_state", "binoculars_view_active", "aim_screen"),
			&HudOverlay::set_view_state);
	ClassDB::bind_method(D_METHOD("set_objective_line", "text"), &HudOverlay::set_objective_line);
	ClassDB::bind_method(D_METHOD("set_objectives_header", "text"), &HudOverlay::set_objectives_header);
	ClassDB::bind_method(D_METHOD("set_waypoint", "name", "distance_m"), &HudOverlay::set_waypoint);
	ClassDB::bind_method(D_METHOD("clear_waypoint"), &HudOverlay::clear_waypoint);
	ClassDB::bind_method(D_METHOD("set_objectives", "texts", "done"), &HudOverlay::set_objectives);
	ClassDB::bind_method(D_METHOD("set_attach_labels", "screens", "texts", "nearest"),
			&HudOverlay::set_attach_labels);
	ClassDB::bind_method(D_METHOD("set_friendly_tags", "screens", "dists_q16",
								  "names", "entity_ids", "health_ratios_fp16", "flags"),
			&HudOverlay::set_friendly_tags);
	ClassDB::bind_method(D_METHOD("set_friendly_tag_mode", "mode"),
			&HudOverlay::set_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("get_friendly_tag_mode"),
			&HudOverlay::get_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("set_friendly_tag_env", "fog_dist_q16", "speaking_level"),
			&HudOverlay::set_friendly_tag_env);
	ClassDB::bind_method(D_METHOD("set_hud_color_index", "index"),
			&HudOverlay::set_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_hud_color_index"),
			&HudOverlay::get_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_draw_list_stats"), &HudOverlay::get_draw_list_stats);

	BIND_CONSTANT(MIN_CROSSHAIR_STYLE);
	BIND_CONSTANT(MAX_CROSSHAIR_STYLE);
}

HudOverlay::HudOverlay() {
	// Default view state matches the presenter's pre-first-update defaults.
	state_.health_fraction = 1.0f;
	state_.fov_deg = 80.0f;
}

HudOverlay::~HudOverlay() {
	if (additive_item_.is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs != nullptr) {
			rs->free_rid(additive_item_);
		}
		additive_item_ = RID();
	}
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
	label_font_valid_ = load_fnt_(String(choice.normal_fnt), label_font_,
			opennova::hud::kHudFontSlotLabel);
	label_font_bold_valid_ =
			load_fnt_(String(choice.bold_fnt), label_font_bold_,
					opennova::hud::kHudFontSlotLabelBold);
	label_tier_ = choice.tier;
	compiler_.configure_label_fonts(
			label_font_valid_ ? &label_font_ : nullptr,
			label_font_bold_valid_ ? &label_font_bold_ : nullptr,
			choice.scale);
}

Ref<Texture2D> HudOverlay::load_hud_texture_(const String &p_name) const {
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
	compiler_.configure_label_fonts(nullptr, nullptr, 1.0f);
	configured_ = false;
	if (p_hudpos.is_null() || !p_hudpos->is_loaded()) {
		queue_redraw();
		return;
	}

	layout_.ammo_count = pos_record4(p_hudpos->get_ammo_count_pos());
	layout_.weapon_name = pos_record4(p_hudpos->get_weapon_name_pos());
	layout_.game_info = pos_record4(p_hudpos->get_game_info_pos());
	layout_.wpd_info = pos_record4(p_hudpos->get_wpd_info_pos());
	layout_.chat_text = pos_record2(p_hudpos->get_chat_text_pos());
	layout_.clip_pos = pos_record2(p_hudpos->get_clip_pos());
	layout_.stance_pos = pos_record2(p_hudpos->get_stance_pos());
	layout_.health_rect = rect_record(p_hudpos->get_health_rect());
	layout_.heat_rect = rect_record(p_hudpos->get_heat_rect());
	layout_.power_rect = rect_record(p_hudpos->get_powerbar_rect());

	const Dictionary colors = p_hudpos->get_colors();
	const auto color_of = [&colors](const char *key, uint32_t fallback) {
		if (!colors.has(key)) {
			return fallback;
		}
		return color_to_argb(colors[key]);
	};
	layout_.health_border = color_of("health_border", layout_.health_border);
	layout_.tag_good = color_of("tagcolor_good", layout_.tag_good);
	layout_.tag_middle = color_of("tagcolor_middle", layout_.tag_middle);
	layout_.tag_bad = color_of("tagcolor_bad", layout_.tag_bad);
	layout_.hud_text = color_of("hud_textcolor", layout_.hud_text);
	layout_.weapon_text = color_of("weapon_textcolor", layout_.weapon_text);
	layout_.stance_tint = color_of("stanceicon_color", layout_.stance_tint);
	layout_.heat_border = color_of("heat_border", layout_.heat_border);
	layout_.stance_bad = color_of("stancecolor_bad", layout_.stance_bad);

	const Vector3 fade = p_hudpos->get_alpha_fade();
	layout_.alpha_fade_base = fade.x;
	layout_.alpha_fade_max = fade.y;
	layout_.alpha_fade_seconds = fade.z;
	const int chline = p_hudpos->get_hud_chline();
	layout_.chat_lines = chline > 0 ? chline : 8;

	// Static HUD frame background: the first authored HUDGFX record, like the
	// ported shell drew.
	const Array frames = p_hudpos->get_static_frames();
	if (frames.size() > 0) {
		const Dictionary frame = frames[0];
		const Vector2i pos = frame.get("pos", Vector2i());
		layout_.frame_pos = pos_record2(pos);
		const Ref<Texture2D> tex = load_hud_texture_(frame.get("texture", String()));
		textures_[opennova::hud::kHudTexFrame] = tex;
		layout_.frame_texture_valid = tex.is_valid();
		layout_.frame_tex_w = tex.is_valid() ? tex->get_width() : 0;
		layout_.frame_tex_h = tex.is_valid() ? tex->get_height() : 0;
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

void HudOverlay::set_objective_line(const String &p_text) {
	state_.objective_text = p_text.utf8().get_data();
	queue_redraw();
}

void HudOverlay::set_objectives_header(const String &p_text) {
	state_.objectives_header = p_text.utf8().get_data();
}

void HudOverlay::set_waypoint(const String &p_name, int p_distance_m) {
	state_.waypoint.present = true;
	state_.waypoint.name = p_name.utf8().get_data();
	state_.waypoint.distance_m = p_distance_m;
	queue_redraw();
}

void HudOverlay::clear_waypoint() {
	state_.waypoint = opennova::hud::HudWaypointState{};
	queue_redraw();
}

void HudOverlay::set_objectives(const PackedStringArray &p_texts,
		const PackedByteArray &p_done) {
	state_.objectives.clear();
	state_.objectives.reserve(static_cast<size_t>(p_texts.size()));
	for (int64_t i = 0; i < p_texts.size(); ++i) {
		opennova::hud::HudObjectiveRow row;
		row.text = p_texts[i].utf8().get_data();
		row.done = i < p_done.size() && p_done[i] != 0;
		state_.objectives.push_back(row);
	}
	queue_redraw();
}

void HudOverlay::set_attach_labels(const PackedVector2Array &p_screens,
		const PackedStringArray &p_texts, const PackedByteArray &p_nearest) {
	state_.attach_labels.clear();
	const int64_t count = std::min(p_screens.size(), p_texts.size());
	state_.attach_labels.reserve(static_cast<size_t>(count));
	for (int64_t i = 0; i < count; ++i) {
		opennova::hud::HudAttachLabel label;
		label.screen_x = p_screens[i].x;
		label.screen_y = p_screens[i].y;
		label.text = p_texts[i].utf8().get_data();
		label.nearest = i < p_nearest.size() && p_nearest[i] != 0;
		state_.attach_labels.push_back(label);
	}
	queue_redraw();
}

void HudOverlay::set_friendly_tags(const PackedVector2Array &p_screens,
		const PackedInt32Array &p_dists_q16, const PackedStringArray &p_names,
		const PackedInt32Array &p_entity_ids,
		const PackedInt32Array &p_health_ratios_fp16,
		const PackedInt32Array &p_flags) {
	state_.friendly_tags.clear();
	const int64_t count = std::min(p_screens.size(), p_dists_q16.size());
	state_.friendly_tags.reserve(static_cast<size_t>(count));
	for (int64_t i = 0; i < count; ++i) {
		opennova::hud::HudFriendlyTag tag;
		tag.screen_x = p_screens[i].x;
		tag.screen_y = p_screens[i].y;
		tag.dist_q16 = p_dists_q16[i];
		if (i < p_names.size()) tag.name = p_names[i].utf8().get_data();
		if (i < p_entity_ids.size())
			tag.entity_id = static_cast<uint16_t>(p_entity_ids[i]);
		if (i < p_health_ratios_fp16.size())
			tag.health_ratio_fp16 = p_health_ratios_fp16[i];
		const int32_t flags = i < p_flags.size() ? p_flags[i] : 0;
		tag.medic = (flags & 1) != 0;
		tag.speaking = (flags & 2) != 0;
		tag.player = (flags & 4) != 0;
		state_.friendly_tags.push_back(tag);
	}
	queue_redraw();
}

void HudOverlay::set_friendly_tag_mode(int p_mode) {
	state_.friendly_tag_mode =
			CLAMP(p_mode, 0, opennova::hud::kFriendlyTagModeCount - 1);
	queue_redraw();
}

int HudOverlay::get_friendly_tag_mode() const {
	return state_.friendly_tag_mode;
}

void HudOverlay::set_hud_color_index(int p_index) {
	// 0..5 like the retail cycle [orig: input action case 10 @0x49afc7 wraps
	// past 5 to 0; config token "hud_color_index" default 2 @0x54d2a6].
	state_.hud_color_index = CLAMP(p_index, 0, 5);
	queue_redraw();
}

int HudOverlay::get_hud_color_index() const {
	return state_.hud_color_index;
}

void HudOverlay::set_friendly_tag_env(int p_fog_dist_q16,
		int p_speaking_level255) {
	state_.fog_dist_q16 = p_fog_dist_q16 > 0 ? p_fog_dist_q16 : INT32_MAX;
	state_.speaking_level255 = p_speaking_level255;
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

Dictionary HudOverlay::get_draw_list_stats() {
	Dictionary out;
	int64_t quads_filled = 0;
	int64_t quads_wire = 0;
	int64_t quads_textured = 0;
	int64_t quads_additive = 0;
	int64_t tris = 0;
	int64_t lines = 0;
	int64_t glyphs = 0;
	int64_t underlines = 0;
	int64_t elements = 0;
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
	}
	out["quads"] = quads_filled + quads_wire;
	out["quads_filled"] = quads_filled;
	out["quads_wire"] = quads_wire;
	out["quads_textured"] = quads_textured;
	out["quads_additive"] = quads_additive;
	out["tris"] = tris;
	out["lines"] = lines;
	out["glyphs"] = glyphs;
	out["underlines"] = underlines;
	out["elements_drawn"] = elements;
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
	if (!configured_) {
		return;
	}
	const Vector2 surface = draw_surface_();
	// Retail re-inits the overlay fonts on resolution change; the lazy tier
	// check is that re-init (the policy lives in hud_label_font_choice).
	ensure_label_fonts_(surface.x);
	render_list_(compiler_.compile(state_, surface.x, surface.y));
}

void HudOverlay::render_list_(const HudDrawList &p_list) {
	RenderingServer *rs = RenderingServer::get_singleton();
	// Kind-grouped submission preserves the compiler's per-kind insertion
	// order and keeps every glyph above the quads (retail draws its text
	// elements over the bars/frames the same walk emitted).
	for (const opennova::hud::HudQuad &quad : p_list.quads) {
		const Rect2 rect(quad.x0, quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0);
		const Color color = argb_to_color(quad.color);
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
		colors.push_back(argb_to_color(tri.color));
		draw_polygon(points, colors, uvs, tex);
	}
	for (const opennova::hud::HudLine &line : p_list.lines) {
		draw_line(Vector2(line.x0, line.y0), Vector2(line.x1, line.y1),
				argb_to_color(line.color), line.width);
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
		colors.push_back(argb_to_color(glyph.color));
		draw_polygon(points, colors, uvs, page);
	}
	for (const opennova::hud::GameFontUnderline &underline : p_list.underlines) {
		draw_line(Vector2(underline.x0, underline.y), Vector2(underline.x1, underline.y),
				argb_to_color(underline.color), 1.0f);
	}
}
