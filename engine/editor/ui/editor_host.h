#pragma once

#include <editor/session/editor_request.h>
#include <editor/session/session_view.h>

namespace opennova::editor {

// What every editor window sees of the world (ADR 0046 d10): the session's view to
// read and a sink for the typed requests it raises. The composition (EditorWindows)
// implements it; a test can implement it with a seeded view and a captured queue.
class EditorHost {
public:
	virtual ~EditorHost() = default;
	virtual const SessionView &view() const = 0;
	virtual void request(EditorRequest request) = 0;
};

} // namespace opennova::editor
