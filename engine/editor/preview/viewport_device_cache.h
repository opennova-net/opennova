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
// used given up for a new one. Each is attached to its viewport while it lives: given up, its
// viewport keeps its state (its camera, its options), and the next device made for it builds its
// picture again. The Shell (EditorApp) makes one with its factory, the devices its kind's table
// makes (godot/src/authoring/viewport_devices.cpp); a test makes one with fakes.
class ViewportDeviceCache : public ViewportDeviceSource {
public:
	using Factory = std::function<std::unique_ptr<ViewportDevice>(ViewportKind kind)>;
	explicit ViewportDeviceCache(Factory make, size_t capacity = kViewportDeviceCapacity);
	~ViewportDeviceCache() override;
	ViewportDeviceCache(const ViewportDeviceCache &) = delete;
	ViewportDeviceCache &operator=(const ViewportDeviceCache &) = delete;

	// One pump (the Shell's, after the session's poll): each viewport the Preview window follows
	// (view.documents.previews) and each a canvas asked for since (device) given a device, made
	// where it has none and attached to its viewport, the least recently used given up past the
	// capacity (detached); a device whose viewport went (its document closed) dropped; then every
	// viewport with a device follows the view, and each device takes what its viewport asks (a
	// rebuild, an update, a clear) and reports what its picture read and placed.
	void sync(Viewports &viewports, const SessionView &view);
	// Every frame, after the clock ran (Viewports::advance): what the clock drives in each device.
	void tick(const Viewports &viewports);
	// The device drawing the viewport of `kind` over `path`, now the most recently used; null while
	// none is made, which the next sync makes.
	ViewportDevice *device(const std::string &path, ViewportKind kind) override;
	// How many devices it holds, and the one of (path, kind) (null: none), its recency untouched.
	size_t size() const { return slots_.size(); }
	ViewportDevice *held(const std::string &path, ViewportKind kind) const;

private:
	struct Slot {
		std::string path;
		ViewportKind kind = ViewportKind::kCount;
		std::unique_ptr<ViewportDevice> device;
		uint64_t used = 0;
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
};

} // namespace opennova::editor
