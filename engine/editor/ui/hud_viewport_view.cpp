#include <editor/ui/hud_viewport_view.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include <imgui.h>

#include <editor/documents/document_types.h>
#include <editor/preview/hud_canvas.h>
#include <editor/preview/hud_viewport.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/workspace.h>
#include <runtime/hud/hud_texture_names.h>

namespace opennova::editor {

namespace {

constexpr const char *kViewNames[] = { "Normal view", "Binoculars", "Night vision" };
constexpr const char *kViewTips[] = {
	"The HUD over the plain first-person view.",
	"The HUD under the binoculars: the game's binocular mask, reticle and range digits over it, and what the HUD "
	"hides while they are up.",
	"The HUD under the night-vision goggles: the game's goggle mask and gain scale over it (the green image the "
	"game renders under them is the 3D view's, which the preview has none of).",
};
constexpr const char *kDetailNames[] = { "Detail 0: all", "Detail 1", "Detail 2", "Detail 3: hidden" };

std::string screen_words(int width, int height) {
	std::string out = std::to_string(width) + " x " + std::to_string(height);
	if (width == 1024 && height == 768) out += " (the design size)";
	return out;
}

std::string stance_words(const HudViewport &model, int id) {
	const std::string &name = model.stance_names()[size_t(std::clamp(id, 0, 5))];
	return "Stance " + std::to_string(id) + (name.empty() ? std::string() : ": " + name);
}

std::string weapon_words(const HudViewport &model, const std::string &weapon) {
	if (weapon.empty())
		return model.weapons().empty() ? std::string("First weapon (weapon.def has none)")
		                               : "First weapon: " + model.weapons().front().name;
	if (weapon == "NONE") return "No weapon";
	return weapon;
}

float combo_width(float widest_text) {
	return widest_text + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// A combo of `count` choices on `row`, the chosen one's words shown; true with `chosen` set when a choice
// is made.
template <typename Words>
bool choice(ui_kit::WrapRow &row, const char *id, int count, int current, float width, Words &&words, int &chosen,
		const char *tip) {
	row.next(ui_kit::field_width(width, id));
	ImGui::SetNextItemWidth(width);
	bool made = false;
	if (ImGui::BeginCombo(id, words(current).c_str())) {
		for (int i = 0; i < count; ++i)
			if (ImGui::Selectable(words(i).c_str(), i == current)) {
				chosen = i;
				made = true;
			}
		ImGui::EndCombo();
	}
	ui_kit::tooltip(tip);
	return made;
}

// Go to a texture an element draws: its file opened where the editor edits its kind, else shown in Files.
void go_to_texture(Workspace &workspace, const SessionView &view, const std::string &path) {
	ReferenceTarget target;
	target.file = path;
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(path) : nullptr;
	target.editable = entry && is_editable_kind(entry->kind);
	window_requests::go_to(workspace, target);
}

// Go to a line of the HUD layout that places an element.
void go_to_line(Workspace &workspace, const HudViewport &model, const HudPreviewElement::Line &line) {
	ReferenceTarget target;
	target.file = model.path();
	target.locator = TextDocument::locator(line.line, line.column);
	target.editable = true;
	window_requests::go_to(workspace, target);
}

} // namespace

HudViewportView::HudViewportView() : ViewportView(ViewportKind::Hud) {}

void HudViewportView::draw_ready(Workspace &workspace, const ViewportModel &viewport, ViewportContext &context) {
	const auto &model = static_cast<const HudViewport &>(viewport);
	const SessionView &view = workspace.view();
	HudViewportOptions options = model.options();
	ui_kit::WrapRow row;
	// The screen.
	{
		int current = -1;
		for (size_t i = 0; i < std::size(kHudScreenSizes); ++i)
			if (kHudScreenSizes[i].width == options.width && kHudScreenSizes[i].height == options.height) current = int(i);
		const std::string shown = screen_words(options.width, options.height);
		const float width = combo_width(ui_kit::text_width("1600 x 1200 (the design size)"));
		row.next(ui_kit::field_width(width, "##screen"));
		ImGui::SetNextItemWidth(width);
		if (ImGui::BeginCombo("##screen", shown.c_str())) {
			for (size_t i = 0; i < std::size(kHudScreenSizes); ++i)
				if (ImGui::Selectable(screen_words(kHudScreenSizes[i].width, kHudScreenSizes[i].height).c_str(), int(i) == current)) {
					options.width = kHudScreenSizes[i].width;
					options.height = kHudScreenSizes[i].height;
				}
			ImGui::EndCombo();
		}
		ui_kit::tooltip("The screen the HUD is drawn at. hudpos.def's positions are in 1024 x 768 and scale to the "
		                "screen; the HUD font is FONTHUD1_HI above 640 pixels wide and FONTHUD1_LO at 640 and below, "
		                "and the label fonts change at 640 and 800.");
	}
	// The stance.
	{
		int chosen = options.stance;
		const float width = combo_width(ui_kit::text_width("Stance 0: PARACHUTE"));
		if (choice(row, "##stance", 6, options.stance, width, [&](int i) { return stance_words(model, i); }, chosen,
		           "The stance the HUD shows: its HUDSTANCE icon, which cross-fades from the last one over ALPHAFADE's time."))
			options.stance = chosen;
	}
	// The weapon.
	{
		const std::vector<HudPreviewWeapon> &weapons = model.weapons();
		const int count = int(weapons.size()) + 2;
		auto token = [&](int i) { return i == 0 ? std::string() : i == 1 ? std::string("NONE") : weapons[size_t(i - 2)].name; };
		int current = 0;
		for (int i = 0; i < count; ++i)
			if (token(i) == options.weapon) current = i;
		int chosen = current;
		const float width = combo_width(ui_kit::text_width("First weapon: WWWWWWWWWWWW"));
		if (choice(row, "##weapon", count, current, width, [&](int i) { return weapon_words(model, token(i)); }, chosen,
		           "The weapon the player holds: its name, ammo count, clip and round art, icon and crosshair, as weapon.def "
		           "gives them."))
			options.weapon = token(chosen);
	}
	// The clip and the reserve.
	{
		// Room for four digits beside the field's step buttons.
		const float width = ui_kit::text_width("9999") + 2.0f * ImGui::GetFrameHeight() +
		                    4.0f * ImGui::GetStyle().FramePadding.x + 2.0f * ImGui::GetStyle().ItemInnerSpacing.x;
		row.next(ui_kit::field_width(width, "Clip"));
		ImGui::SetNextItemWidth(width);
		int clip = options.clip;
		if (ImGui::InputInt("Clip", &clip, 1, 10)) options.clip = std::clamp(clip, -1, 9999);
		ui_kit::tooltip("The rounds in the clip; -1 a full clip (the weapon's clipsize).");
		row.next(ui_kit::field_width(width, "Reserve"));
		ImGui::SetNextItemWidth(width);
		int reserve = options.reserve;
		if (ImGui::InputInt("Reserve", &reserve, 1, 10)) options.reserve = std::clamp(reserve, 0, 9999);
		ui_kit::tooltip("The rounds the player carries beside the clip.");
	}
	// The health and a hit.
	{
		const float width = ImGui::GetFontSize() * 7.0f;
		row.next(ui_kit::field_width(width, "Health"));
		ImGui::SetNextItemWidth(width);
		int health = options.health;
		if (ImGui::SliderInt("Health", &health, 0, 100, "%d%%")) options.health = health;
		ui_kit::tooltip("The player's health: the health bar's fill and its colour band (tagcolor_good, _middle, _bad).");
		bool hit = options.damage > 0;
		row.next(ui_kit::checkbox_width("Hit"));
		if (ImGui::Checkbox("Hit", &hit)) options.damage = hit ? kHudDamageMost : 0;
		ui_kit::tooltip("The red damage vignette (vignette.tga) at its strongest, as a hit leaves it.");
	}
	// The view.
	{
		int chosen = int(options.view);
		const float width = combo_width(ui_kit::text_width(kViewNames[1]));
		if (choice(row, "##view", 3, int(options.view), width, [](int i) { return std::string(kViewNames[i]); }, chosen,
		           kViewTips[size_t(options.view)]))
			options.view = HudPreviewView(chosen);
	}
	// The detail level and the crosshair.
	{
		int chosen = options.detail;
		const float width = combo_width(ui_kit::text_width(kDetailNames[3]));
		if (choice(row, "##detail", 4, options.detail, width, [](int i) { return std::string(kDetailNames[i]); }, chosen,
		           "The HUD detail level F6 steps: each HUDDECLUT row of hudpos.def says which levels show its group; "
		           "level 3 hides the HUD."))
			options.detail = chosen;
		const float slider = ImGui::GetFontSize() * 6.0f;
		row.next(ui_kit::field_width(slider, "Crosshair"));
		ImGui::SetNextItemWidth(slider);
		int style = options.crosshair;
		char label[24];
		std::snprintf(label, sizeof(label), "cross%02d", style + 1);
		if (ImGui::SliderInt("Crosshair", &style, opennova::hud::kHudCrosshairStyleMin,
		                     opennova::hud::kHudCrosshairStyleMax, label))
			options.crosshair = style;
		ui_kit::tooltip("The player's crosshair style, cross01.tga to cross25.tga.");
	}
	if (options != model.options()) workspace.request(request::set_viewport(model.path(), hud_options_change(options)));

	// The element picked: its lines and its textures, each a Go to.
	if (const HudPreviewElement *picked = model.picked()) {
		ui_kit::WrapRow strip;
		const std::string words = std::string("Picked: ") + hud_element_words(picked->element).words;
		strip.next(ui_kit::text_width(words.c_str()));
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(words.c_str());
		for (size_t i = 0; i < picked->lines.size(); ++i) {
			const HudPreviewElement::Line &line = picked->lines[i];
			const std::string label = line.key + " (line " + std::to_string(line.line) + ")##line" + std::to_string(i);
			if (ui_kit::tool(strip, label.c_str(), true, "Go to its line in the layout: " + line.text, true))
				go_to_line(workspace, model, line);
		}
		for (size_t i = 0; i < picked->textures.size(); ++i) {
			const HudPreviewElement::Art &art = picked->textures[i];
			const std::string label = art.name + "##art" + std::to_string(i);
			if (ui_kit::tool(strip, label.c_str(), !art.path.empty(),
			                 art.path.empty() ? "The project has no " + art.name + ": the game draws nothing in its place."
			                                  : "Go to the texture it draws: " + art.path,
			                 true))
				go_to_texture(workspace, view, art.path);
		}
	} else {
		ImGui::TextDisabled("Point at an element to name it; click to pick it; drag it to move it; double click to go "
		                    "to its line.");
	}

	canvas(workspace, viewport, context, std::max(48.0f, ImGui::GetContentRegionAvail().y), [&](const CanvasInput &in) {
		// A double click goes to the line that places the element under it.
		if (!in.double_clicked || !in.hovered) return;
		float x = 0.0f, y = 0.0f;
		HudCanvas::screen_point(in.mouse.x, in.mouse.y, in.width, in.height, options.width, options.height, x, y);
		const HudPreviewElement *element = model.element_at(x, y);
		if (element && !element->lines.empty()) go_to_line(workspace, model, element->lines.front());
	});
}

} // namespace opennova::editor
