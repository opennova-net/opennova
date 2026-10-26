#pragma once

// The content store's view as the view JSON's content section (ADR 0069; view/content_view.h): what
// the Content window draws and the editor's MCP reads, the account without its session.

#include <base/io/json.h>

namespace opennova::editor {

struct SessionView;

io::JsonValue content_section(const SessionView &view);

} // namespace opennova::editor
