class_name EnvRenderCamera
extends RefCounted

# The one camera lookup the environment nodes (sky / water / celestial) share:
# the editor's 3D viewport camera when running inside the editor (these nodes
# are sanctioned editor-aware — they render live in the Terrain/Object
# workspaces), else the node's own scene viewport camera.


static func find(node: Node) -> Camera3D:
	if Engine.is_editor_hint():
		var editor_interface = Engine.get_singleton("EditorInterface")
		if editor_interface:
			var viewport = editor_interface.get_editor_viewport_3d(0)
			if viewport:
				return viewport.get_camera_3d()
	var viewport := node.get_viewport()
	return viewport.get_camera_3d() if viewport else null
