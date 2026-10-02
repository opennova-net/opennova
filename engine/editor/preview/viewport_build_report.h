#pragma once

#include <cstdint>
#include <string>

#include <editor/session/operation_progress.h>

namespace opennova::editor {

// Where a device's build of its viewport's picture stands (ADR 0046 S13 V6), as the device says
// after each pump and after each frame's steps. A Rebuild is a build of the viewport's newest
// generation (ViewportModel::builds() as the device takes it), which a device either makes whole as
// it takes it (a menu's screen configured again over the textures its frame keeps) or builds over
// the Shell's frames a unit at a time (a model's textures, then its meshes, then its scene and its
// pose; a menu's textures not decoded yet, then its screen), drawing the last picture it built
// meanwhile: loading. A newer Rebuild drops the one in flight and begins anew; a build that fails
// keeps the last picture, `message` saying why (the protocol's: no Shell device's build fails yet).
// A device that never builds over frames (a test's, a Control's) reports none of this: its picture
// is the newest it was asked for.
struct ViewportBuildReport {
	uint64_t generation = 0; // the generation it builds or last built (0: none asked yet)
	bool loading = false; // that build runs over the frames, the last picture drawn meanwhile
	bool failed = false; // that build failed: the last picture kept
	std::string message; // why it failed
	// Its units, A1's operation progress (operation_progress.h), of this generation's build alone
	// (a newer generation's begins again at 0): done of total steps, the label what the build works
	// on (a model's "textures", "meshes", "scene", "pose"; a menu's "textures", "configure").
	OperationProgress progress;
	// What it cost on the Shell's frames: the frames its units ran on, the longest frame's units, the
	// longest unit and all of its units, in microseconds (a build made whole as it was taken is one
	// frame and one unit).
	uint32_t frames = 0;
	int64_t frame_us = 0;
	int64_t unit_us = 0;
	int64_t total_us = 0;
	// The same as the envelope reads it (its status, reason, message and progress: whether a build
	// runs and how far, whether it failed and why): what moves the view's Viewports concern when it
	// does not. A build made whole as it was taken moves nothing (the picture was made again, as
	// before S13 V6, which `builds` says).
	bool reads_same(const ViewportBuildReport &other) const {
		if (loading != other.loading || failed != other.failed) return false;
		if (failed && message != other.message) return false;
		return !loading ||
				(generation == other.generation && progress.done == other.progress.done &&
						progress.total == other.progress.total && progress.label == other.progress.label);
	}
};

} // namespace opennova::editor
