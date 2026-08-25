class_name McpToolContext
extends RefCounted

## Per-call state shared by the runtime MCP handlers.

var tree: SceneTree = null
var args: Dictionary = {}
var cancelled := false
var logs: Array[String] = []
var images: Array = []
var log_sink: Callable = Callable()


func log(value: Variant) -> void:
	var text := str(value)
	logs.append(text)
	if log_sink.is_valid():
		log_sink.call(text)


func image(value: Variant) -> void:
	images.append(value)


func frames(count: int) -> void:
	var scene_tree := main_tree()
	if scene_tree == null:
		return
	for _i in range(count):
		await scene_tree.process_frame


func main_tree() -> SceneTree:
	if tree != null:
		return tree
	var loop := Engine.get_main_loop()
	return loop if loop is SceneTree else null
