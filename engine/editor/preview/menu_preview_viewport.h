#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::menu {
class MenuFrameCompiler;
struct MenuFrameState;
} // namespace opennova::menu

namespace opennova::editor {

enum class MenuPreviewStatus : uint8_t;
struct MenuPreviewOptions;

// The only seam between the engine-owned menu pane and a rendering device (ADR 0046 d11,
// the GameViewport pattern): the shell renders the previewed screen through the runtime's
// own MenuFrame into an offscreen viewport and draws it into the pane; engine-only runs
// leave it null. Picking, outlines and drags read the configured compiler, so they are
// the game's own hit test and rects. Declared apart from the pane (S13 V4), so the shell's
// device names no window header; S13 V5's viewport seam replaces it.
class MenuPreviewViewport {
public:
	virtual ~MenuPreviewViewport() = default;
	// What the device shows, and why not (`detail`: the status's detail).
	virtual MenuPreviewStatus status(std::string *detail) const = 0;
	// The files the screen names that the project does not have, each once.
	virtual const std::vector<std::string> &missing() const = 0;
	// The files the screen names that the project has but that did not load (a font or
	// string table that does not parse, a texture that does not decode), each once.
	virtual const std::vector<std::string> &unreadable() const = 0;
	// Size the offscreen viewport to the device size and draw its texture as the current
	// ImGui item (it renders on the frames it is drawn).
	virtual void draw(int device_width, int device_height) = 0;
	// How it draws the screen (every window shown, one window held in a state): they apply
	// after every configure, and a change configures again.
	virtual void set_options(const MenuPreviewOptions &options) = 0;
	virtual const MenuPreviewOptions &options() const = 0;
	// The configured compiler and its frame state; null unless ready.
	virtual const menu::MenuFrameCompiler *compiler() const = 0;
	virtual const menu::MenuFrameState *frame_state() const = 0;
	// The document revision the picture shows: an index maps to a record only while it is
	// the document's own.
	virtual uint64_t shown_revision() const = 0;
};

} // namespace opennova::editor
