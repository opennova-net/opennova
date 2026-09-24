// The Physics window (ADR 0042 d6): the engine contact-debug capture's
// control surface over the PhysicsSnapshot the embedder pushes — per-kind
// counts (held in ring / lifetime total) with draw-filter checkboxes, and
// Clear — leaving typed PhysicsRequests the embedder drains into the
// Simulation contact-debug seam.
//
// The window holds only the pushed value record — it never reaches into a
// live World. Visibility-armed: while hidden it drops its snapshot, and the
// embedder (gated on GameDevTools::needs_physics_snapshot) stops building new
// ones and disarms the contact capture, which records only while shown.
#pragma once

#include <runtime/devtools/collision_overlay.h>
#include <runtime/devtools/hitbox_overlay.h>
#include <runtime/devtools/imgui_pass.h>
#include <runtime/devtools/physics_request.h>
#include <runtime/devtools/physics_snapshot.h>

#include <array>
#include <cstdint>
#include <deque>
#include <string>

namespace opennova::devtools {

class PhysicsWindow : public Window {
public:
	// Seconds per pushed snapshot (the Rays window's cadence).
	static constexpr double kRefreshSeconds = 0.25;

	const char *title() const override { return "Physics"; }
	MenuGroup menu_group() const override { return MenuGroup::Sim; }
	InitialDockPlacement initial_dock_placement() const override {
		return InitialDockPlacement::Right;
	}
	void draw(ImGuiPass &pass, uint64_t frame_index) override;
	void on_visibility(bool visible) override;

	// The pushed record, by value; an invalid snapshot clears the page.
	void set_snapshot(const PhysicsSnapshot &snapshot);

	// The Game-view layers: the captured contacts (pushed per logic tick,
	// filtered by the kind checkboxes) and the hit meshes (the hitbox oracle,
	// refreshed at HitboxOverlayLayer::kRefreshHz). A layer on keeps the
	// contact capture armed with this window closed.
	ContactsOverlayLayer &contacts_layer() { return contacts_layer_; }
	HitboxOverlayLayer &hitbox_layer() { return hitbox_layer_; }
	const ContactsOverlayLayer &contacts_layer() const { return contacts_layer_; }
	const HitboxOverlayLayer &hitbox_layer() const { return hitbox_layer_; }
	void set_contacts_overlay(ContactsOverlayRecord record) { contacts_record_ = std::move(record); }
	void set_hitbox_overlay(HitboxOverlayRecord record) { hitbox_record_ = std::move(record); }
	const ContactsOverlayRecord &contacts_overlay() const { return contacts_record_; }
	const HitboxOverlayRecord &hitbox_overlay() const { return hitbox_record_; }

	// The typed request queue the embedder drains. enqueue_request is the one
	// path the drawn controls feed — and the headless test seam.
	void enqueue_request(const PhysicsRequest &request);
	bool take_request(PhysicsRequest &request);

	// The formatted kind rows, for tests and probes (the RaysWindow row-text
	// seam): "Name: held N / total M".
	int row_count() const;
	const char *row_text(int row) const;
	bool snapshot_valid() const { return snapshot_.valid; }

private:
	void format_rows();

	PhysicsSnapshot snapshot_{};
	std::array<std::string, kContactKindCount> rows_{};
	std::deque<PhysicsRequest> requests_;
	// The mask mirrored from every push (the checkboxes display authoritative
	// state; a click flips locally + queues the request, the next push confirms).
	uint32_t mask_edit_ = kContactKindMaskAll;
	// Declared ahead of the layers that read them.
	ContactsOverlayRecord contacts_record_{};
	HitboxOverlayRecord hitbox_record_{};
	ContactsOverlayLayer contacts_layer_{contacts_record_};
	HitboxOverlayLayer hitbox_layer_{hitbox_record_};
};

}  // namespace opennova::devtools
