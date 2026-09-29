#pragma once

#include <cstdint>
#include <vector>

#include <editor/model/edit.h>
#include <editor/preview/model_overlay.h>
#include <formats/threedi/threedi_3di3.h>

namespace opennova::editor {

class ModelDocument;

// What a drag of a marker changes (ADR 0046 S10p5): its place, or its axis by the tip (a
// user point's Z axis, a spot light's).
enum class ModelHandle : uint8_t { Place, Axis };
// "place", "axis": the MCP's tokens, and back; false for another token.
const char *model_handle_token(ModelHandle handle);
bool model_handle_from_token(const char *token, ModelHandle &out);

// The grid a dragged place snaps to, metres in the file's axes (0: free).
inline constexpr float kModelHandleSnaps[] = {0.0f, 1.0f / 64.0f, 1.0f / 16.0f, 0.25f, 1.0f};

// The edits that put a marker's record where `to` lies in the preview's space: its place
// (snapped to `snap` metres on each of the file's axes, 0 free), or its axis turned to
// point from the marker at `to`. The part's pose at `time_ms` is undone first (the record
// is authored in the model's frame and the part carries it), and the file's axes are the
// schema's position and direction fields (threedi_build_to_mission). Each edit carries
// `gesture`, so a drag is one undo step. False for a marker with no record that moves (a
// pivot is geometry), an axis on an omni light, or a turn to a zero-length axis.
bool model_handle_edits(const ModelDocument &document, const threedi::Threedi3di3 &model, const ModelOverlay &overlay,
                        int lod, uint32_t time_ms, const int32_t bus[96], ModelHandle handle, const PreviewVec3 &to,
                        float snap, uint64_t gesture, std::vector<Edit> &out);

} // namespace opennova::editor
