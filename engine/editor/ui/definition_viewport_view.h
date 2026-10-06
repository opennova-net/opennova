#pragma once

#include <editor/ui/viewport_view.h>

namespace opennova::editor {

// The definition viewport's view (ADR 0046 DI-21; preview/definition_viewport.h): its toolbar (an item's State,
// Alive, Destroying, Husk or Husk final; Enemy; Occupied where its particle slot waits for a driver; a person's
// SSN; a weapon's Third or First person; Run or Pause of the preview clock, Replay, Grid, Mute, Frame), a line
// naming what it draws with a Go to of the model (and of the item an ammo's round becomes), the picture's notes,
// the effects it spawns (each with a Go to of the definition the game spawns) and, while it dies, the death in
// order, each leg lit as the clock passes it; then the canvas filling the rest, which orbits, pans and dollies
// the camera (preview/orbit_canvas). Every change a SetViewport, every jump a Go to.
class DefinitionViewportView final : public ViewportView {
public:
	DefinitionViewportView();

protected:
	void draw_ready(Workspace &workspace, const ViewportModel &model, ViewportContext &context) override;
	void draw_empty(Workspace &workspace, const ViewportModel *model, const std::string &path) override;
};

} // namespace opennova::editor
