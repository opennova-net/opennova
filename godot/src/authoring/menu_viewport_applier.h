#pragma once

#include <godot_cpp/classes/sub_viewport.hpp>

#include <memory>

#include <editor/assets/project_asset_source.h>

#include "authoring/viewport_applier.h"
#include "mnu/menu_frame.h"

namespace godot {

// A menu viewport's device work (ADR 0046 S6c, S9j, S13 V5): the runtime's own MenuFrame laid out
// across the device's SubViewport, configured with the screen the viewport shows (the menu the game
// would read were it saved now, MenuViewport::image and screen) and the Shell's %VAR% list, reading
// the project's files through the session's asset source (the open documents standing in for
// theirs), the viewport's options held on its frame state after each configure (apply_menu_options).
// The viewport's hit tests read its own headless compile; this one draws, and reports where it placed
// each widget beside it.
class MenuViewportApplier final : public ViewportApplier {
public:
	explicit MenuViewportApplier(SubViewport &viewport);

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view) override;
	void update(const opennova::editor::ViewportModel &model) override;
	void clear() override;
	void step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	void tick(const opennova::editor::ViewportModel &, const opennova::editor::PreviewClock &) override {}
	void resize(int width, int height) override;

	MenuFrame *frame() const { return frame_; }

private:
	// The viewport's options on the configured screen's frame state.
	void apply_options_(const opennova::editor::ViewportModel &model);

	MenuFrame *frame_ = nullptr;
	// The files the frame reads through (it borrows them until its next configure).
	std::shared_ptr<const opennova::editor::ProjectAssetSource> assets_;
};

} // namespace godot
