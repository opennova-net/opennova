#pragma once

#include <godot_cpp/classes/sub_viewport.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

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
//
// It configures as it takes the Rebuild when the frame keeps every texture the screen names (a
// configure again after an edit or an option, which the shipped menus measure well within the poll
// budget), and over the frames otherwise (S13 V6: a screen's first configure decodes its textures,
// the shipped options.mnu's beyond the poll budget): a unit a texture the frame does not keep,
// decoded ahead of the configure, then the configure, which finds them all.
class MenuViewportApplier final : public ViewportApplier {
public:
	explicit MenuViewportApplier(SubViewport &viewport);
	// The frame lets go of the screen it borrowed (it may outlive the applier until its SubViewport
	// is freed).
	~MenuViewportApplier() override;

	void rebuild(const opennova::editor::ViewportModel &model, const opennova::editor::SessionView &view,
			const opennova::editor::PreviewClock &clock) override;
	ApplierStep step(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			std::string &failure) override;
	bool building() const override { return build_ != nullptr; }
	opennova::editor::OperationProgress progress() const override;
	void update(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void clear() override;
	void apply(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock,
			opennova::editor::ViewportDeviceReport &report) override;
	// The menu's clock on the preview clock (menu_frame_clock): a focused edit box's caret, the frame's
	// time set (and the frame drawn again, never configured again) only as the caret's half of the
	// blink changes.
	void tick(const opennova::editor::ViewportModel &model, const opennova::editor::PreviewClock &clock) override;
	void resize(int width, int height) override;

	MenuFrame *frame() const { return frame_; }

private:
	// A configure over the frames: the files it reads through (held while it runs), the textures the
	// frame does not keep yet (decoded ahead, a unit each), then the configure of the screen the
	// viewport shows then (a newer Rebuild begins anew: the viewport's image is this one).
	struct Build {
		std::shared_ptr<const opennova::editor::ProjectAssetSource> assets;
		std::vector<std::string> textures;
		size_t next = 0;
		size_t units() const { return textures.size() + 1; }
	};
	// The screen the viewport shows configured on the frame over `assets`, its options held on it and
	// its clock the preview clock's at `clock` (S13 V8: a configure that ends frames after its Rebuild
	// draws the caret's half of the blink of now, not of 0).
	void configure_(const opennova::editor::ViewportModel &model,
			const std::shared_ptr<const opennova::editor::ProjectAssetSource> &assets,
			const opennova::editor::PreviewClock &clock);
	// The viewport's options on the configured screen's frame state.
	void apply_options_(const opennova::editor::ViewportModel &model);

	MenuFrame *frame_ = nullptr;
	uint64_t frame_id_ = 0; // the frame's instance, checked as the applier goes
	// The menu image and the files the frame reads through, held while the frame borrows them (until
	// its next configure, or its clear): the viewport may make its image again, or go, first.
	std::shared_ptr<const opennova::mnu::Document> image_;
	std::shared_ptr<const opennova::editor::ProjectAssetSource> assets_;
	std::unique_ptr<Build> build_; // the configure in flight (null: none)
	opennova::editor::OperationProgress done_; // the last configure's units, all done
};

} // namespace godot
