#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/viewport_device.h>
#include <editor/preview/viewport_kinds.h>

namespace opennova::editor {

class Viewports;
struct SessionView;

// The most devices a Shell keeps at once (ADR 0046 S13 V5, the maintainer's call).
inline constexpr size_t kViewportDeviceCapacity = 4;

// A Shell's devices (ADR 0046 S13 V5): at most kViewportDeviceCapacity, one per (document, kind)
// so a document may own several viewports (a mission's 3D view and its map), the least recently
// used given up for a new one, never one used in the same round (a sync and the asks since the one
// before: five viewports drawn at once keep four devices and the fifth waits, rather than thrashing
// every frame). Each is attached to its viewport while it lives: given up, its viewport keeps its
// state (its camera, its options), and the next device made for it builds its picture again. The
// Shell (EditorApp) makes one with its factory, the devices its kind's table makes
// (godot/src/authoring/viewport_devices.cpp); a test makes one with fakes.
class ViewportDeviceCache : public ViewportDeviceSource {
public:
	using Factory = std::function<std::unique_ptr<ViewportDevice>(ViewportKind kind)>;
	explicit ViewportDeviceCache(Factory make, size_t capacity = kViewportDeviceCapacity);
	~ViewportDeviceCache() override;
	ViewportDeviceCache(const ViewportDeviceCache &) = delete;
	ViewportDeviceCache &operator=(const ViewportDeviceCache &) = delete;

	// Which Preview targets each sync gives a device: every Preview-role kind's and the active
	// document's Main-role viewport (true, the default: a headless Shell, a test, where no window
	// draws to ask, given a device for what the workspace would show: S13 V10's script device of the
	// active text document among them), or only the one of the kind the Preview window shows (false,
	// the workspace: DocumentsView::preview_shown, so a kind it does not show holds no device until a
	// view asks for one as it draws, a Main viewport's on show).
	void set_pin_all_targets(bool all) { pin_all_ = all; }
	// One pump (the Shell's, after the session's poll): a device whose viewport went (its document
	// closed) or is not the one it was attached to (the document closed and opened again between two
	// syncs: a viewport made again, at the defaults, that no device is attached to) dropped; each
	// Preview target it pins (set_pin_all_targets) and each viewport a view asked for since (device)
	// given a device, made where it has none and attached
	// to its viewport, the least recently used not used this round given up past the capacity
	// (detached; none such: the device waits); then every viewport with a device follows the view, and
	// each device takes what its viewport asks (a rebuild, an update, a clear) and reports what its
	// picture read and placed, its size and its build.
	void sync(Viewports &viewports, const SessionView &view);
	// Every frame, after the clock ran (Viewports::advance): what the clock drives in each device.
	void tick(const Viewports &viewports);
	// Every frame after the tick (S13 V6): the devices' builds in flight a unit further, the most
	// recently used device's first: one unit a frame in all whatever the budget (the first build
	// that has one), then units while `more`, asked after each, says the frame's budget lasts (the
	// Shell's: the milliseconds it gives the builds, shared by every build; a test's, a count), and
	// what each build came to given to its viewport (Viewports::device_build).
	void step(Viewports &viewports, const std::function<bool()> &more);
	// The device drawing the viewport of `kind` over `path`, now the most recently used and kept
	// through the next sync; null while none is made, which the next sync makes.
	ViewportDevice *device(const std::string &path, ViewportKind kind) override;
	// How many devices it holds, and the one of (path, kind) (null: none), its recency untouched.
	size_t size() const { return slots_.size(); }
	ViewportDevice *held(const std::string &path, ViewportKind kind) const;

private:
	struct Slot {
		std::string path;
		ViewportKind kind = ViewportKind::kCount;
		std::unique_ptr<ViewportDevice> device;
		uint64_t used = 0; // the recency stamp
		uint64_t round = 0; // the last round (sync) it was used in or asked for
	};
	Slot *slot_(const std::string &path, ViewportKind kind);
	// The device of (path, kind), made and attached when there is none, the least recently used
	// given up first when full.
	void use_(Viewports &viewports, const std::string &path, ViewportKind kind);

	Factory make_;
	size_t capacity_;
	std::vector<Slot> slots_;
	struct Want {
		std::string path;
		ViewportKind kind = ViewportKind::kCount;
	};
	std::vector<Want> wanted_; // asked for by a canvas since the last sync
	uint64_t clock_ = 0; // the recency stamp
	uint64_t round_ = 0; // the syncs so far: the round a device used now belongs to
	bool pin_all_ = true;
};

} // namespace opennova::editor
