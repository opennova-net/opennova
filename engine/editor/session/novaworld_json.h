#pragma once

// NovaWorld's view as the view JSON's novaworld section (ADR 0069; view/novaworld_view.h): what
// the NovaWorld window draws and the editor's MCP reads, the account without its session.

#include <base/io/json.h>

namespace opennova::editor {

struct SessionView;

io::JsonValue novaworld_section(const SessionView &view);

} // namespace opennova::editor
