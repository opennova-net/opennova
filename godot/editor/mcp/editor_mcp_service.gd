class_name EditorMcpService
extends McpEndpointService

## The OpenNova Editor's MCP endpoint (`opennova-editor`, ADR 0046 d10): the same
## loopback Streamable-HTTP transport the game runs (godot/game/mcp/), started by
## `--mcp-port <n>` on the editor's command line or by a test on port 0, over the
## editor's wire seam (EditorApp.request_json / query_json, ADR 0046 S13 A5). The editor loads
## this script BY PATH (res://editor/mcp/, which the game products never ship), so
## nothing in the shell names its class. The endpoint lives as long as the editor:
## `editor_request {kind: "quit"}` is the stop. McpEndpointService carries the hub, the
## server and the listen.

var tools: EditorMcpTools = null
var app: Node = null


## Start the endpoint on `port` (loopback only; 0 binds an ephemeral port for tests).
func setup(editor_app: Node, port: int) -> Error:
	if editor_app == null or not editor_app.has_method("query_json") or port < 0 or port > 65535:
		return ERR_INVALID_PARAMETER
	app = editor_app
	_open_endpoint("EditorMcpServer", "opennova-editor", "OpenNova Editor", (
			"Drive the OpenNova Editor: editor_state reads the view by section; editor_query asks "
			+ "what the editor holds (the files, a document and its records, the problems, the asset "
			+ "graph, a menu's tree, the output, the operation, the catalog of every request and "
			+ "query); editor_request raises any typed request by kind (open, edit records in the "
			+ "batch form, save, import, rename); editor_viewport reads a document's viewport (a "
			+ "menu's screen, a model) and drags or commands through it; editor_build packs the "
			+ "project; editor_play runs the game on the build and reports the game's own MCP port, "
			+ "which game_* tools then drive."))
	tools = EditorMcpTools.new(self, app)
	tools.register_all(server.registry)
	return _start_endpoint(port, "Editor MCP")
