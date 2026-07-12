class_name ParticleBlueprintScreen
extends Control

## UE-Cascade-style blueprint surface for the particle workspace: a GraphEdit
## node canvas on the left (effects, particles, curve tables as nodes; edges are
## real .ptl references) and the live 3D ParticlePreview on the right. Selecting
## a node drives the model selection (and the inspector lane + preview); dragging
## an edge edits a reference:
##   effect  → particle   = membership in the effect's pdefs
##   particle → particle   = the particle's child_id spawn chain
## Curve tables appear as standalone selectable nodes (edited in the inspector).
##
## The workspace keeps driving the inner preview directly via get_preview(), so
## existing selection/preview plumbing is unchanged; this screen only adds the
## graph and the split layout.

const ParticlePreviewScript = preload("res://modtools/particle/particle_preview.gd")

# Slot port "types" — gate which ports can be wired together interactively.
const T_MEMBER := 0  # effect output  <-> particle input
const T_SPAWN := 1   # particle output <-> child-particle input

var _editor: ParticleEditor
var _workspace
var _graph: GraphEdit
var _preview: ParticlePreview

# name(StringName) -> {kind: String, obj: Object}
var _nodes: Dictionary = {}
# obj -> name(String)
var _obj_to_name: Dictionary = {}
var _node_counter := 0
var _syncing := false


func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	var split := HSplitContainer.new()
	split.set_anchors_preset(Control.PRESET_FULL_RECT)
	split.split_offset = 520
	add_child(split)

	_graph = GraphEdit.new()
	_graph.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_graph.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_graph.minimap_enabled = false
	_graph.right_disconnects = true
	_graph.show_arrange_button = false
	split.add_child(_graph)

	_preview = ParticlePreviewScript.new()
	_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_preview.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_preview.custom_minimum_size = Vector2(360, 0)
	split.add_child(_preview)

	_graph.connection_request.connect(_on_connection_request)
	_graph.disconnection_request.connect(_on_disconnection_request)
	_graph.node_selected.connect(_on_node_selected)
	_graph.delete_nodes_request.connect(_on_delete_nodes_request)


func get_preview() -> ParticlePreview:
	return _preview


func get_viewport_camera() -> Camera3D:
	return _preview.get_preview_camera() if _preview != null else null


func set_workspace(value) -> void:
	_workspace = value


func set_particle_editor(value: ParticleEditor) -> void:
	if _editor != null:
		if _editor.document_changed.is_connected(_rebuild_graph):
			_editor.document_changed.disconnect(_rebuild_graph)
		if _editor.selection_changed.is_connected(_highlight_selected):
			_editor.selection_changed.disconnect(_highlight_selected)
		if _editor.presentation_changed.is_connected(_refresh_presentations):
			_editor.presentation_changed.disconnect(_refresh_presentations)
	_editor = value
	if _editor != null:
		_editor.document_changed.connect(_rebuild_graph)
		_editor.selection_changed.connect(_highlight_selected)
		_editor.presentation_changed.connect(_refresh_presentations)
	_rebuild_graph()


# --- Graph build -------------------------------------------------------------

func _rebuild_graph() -> void:
	if _graph == null:
		return
	_graph.clear_connections()
	for child in _graph.get_children():
		if child is GraphNode:
			_graph.remove_child(child)
			child.queue_free()
	_nodes.clear()
	_obj_to_name.clear()
	_node_counter = 0
	if _editor == null or _editor.particle_file == null:
		return

	var file := _editor.particle_file
	var row := 0
	for effect in file.effects:
		if effect != null:
			_add_effect_node(effect, Vector2(40, 40 + row * 120))
			row += 1
	row = 0
	for particle in file.particles:
		if particle != null:
			_add_particle_node(particle, Vector2(330, 40 + row * 120))
			row += 1
	row = 0
	for table in file.tables:
		if table != null:
			_add_table_node(table, Vector2(640, 40 + row * 120))
			row += 1

	_rebuild_connections()
	_highlight_selected()


func _add_effect_node(effect, pos: Vector2) -> void:
	var node := GraphNode.new()
	var node_name := "n%d" % _node_counter
	_node_counter += 1
	node.name = node_name
	node.title = "▣ %s" % effect.id
	node.position_offset = pos
	var label := Label.new()
	label.text = "%d particle(s)" % effect.pdefs.size()
	node.add_child(label)
	# Effects only own membership edges; child_id never targets an effect.
	node.set_slot(0, false, T_SPAWN, Color(0.9, 0.7, 0.3), true, T_MEMBER, Color(0.4, 0.7, 0.95))
	_graph.add_child(node)
	_register(node_name, "effect", effect)


func _add_particle_node(particle, pos: Vector2) -> void:
	var node := GraphNode.new()
	var node_name := "n%d" % _node_counter
	_node_counter += 1
	node.name = node_name
	node.title = "● %s" % particle.id
	node.position_offset = pos
	var label := Label.new()
	label.text = _particle_summary(particle)
	node.add_child(label)
	# Row 0: left input (member-of an effect) + right output (spawns a child particle).
	node.set_slot(0, true, T_MEMBER, Color(0.4, 0.7, 0.95), true, T_SPAWN, Color(0.9, 0.7, 0.3))
	var child_label := Label.new()
	child_label.text = "child spawn target"
	node.add_child(child_label)
	# Membership and child chains terminate on separate typed inputs.
	node.set_slot(1, true, T_SPAWN, Color(0.9, 0.7, 0.3), false, T_SPAWN, Color(0.9, 0.7, 0.3))
	_graph.add_child(node)
	_register(node_name, "particle", particle)


func _add_table_node(table, pos: Vector2) -> void:
	var node := GraphNode.new()
	var node_name := "n%d" % _node_counter
	_node_counter += 1
	node.name = node_name
	node.title = "∿ %s" % table.id
	node.position_offset = pos
	var spark := _TableSpark.new()
	spark.table = table
	spark.custom_minimum_size = Vector2(120, 40)
	node.add_child(spark)
	_graph.add_child(node)
	_register(node_name, "table", table)


func _register(node_name: String, kind: String, obj) -> void:
	_nodes[node_name] = {"kind": kind, "obj": obj}
	_obj_to_name[obj] = node_name


func _rebuild_connections() -> void:
	var file := _editor.particle_file
	# Membership: effect output(0) -> particle input(0).
	for effect in file.effects:
		if effect == null:
			continue
		var effect_name: String = _obj_to_name.get(effect, "")
		if effect_name.is_empty():
			continue
		for pid in effect.pdefs:
			var particle := file.find_particle(pid)
			if particle != null and _obj_to_name.has(particle):
				_graph.connect_node(effect_name, 0, _obj_to_name[particle], 0)
	# Spawn chain: particle output(0) -> child-particle spawn input(1).
	for particle in file.particles:
		if particle == null or String(particle.child_id).is_empty():
			continue
		var particle_name: String = _obj_to_name.get(particle, "")
		if particle_name.is_empty():
			continue
		var child := file.find_particle(String(particle.child_id))
		if child != null and _obj_to_name.has(child):
			_graph.connect_node(particle_name, 0, _obj_to_name[child], 1)


func _refresh_presentations() -> void:
	if _graph == null or _editor == null or _editor.particle_file == null:
		return
	for node_name in _nodes:
		var node := _graph.get_node_or_null(NodePath(String(node_name))) as GraphNode
		var info: Dictionary = _nodes[node_name]
		if node == null or info.is_empty():
			continue
		match String(info.kind):
			"effect":
				var effect := info.obj as NovaParticleEffect
				node.title = "▣ %s" % effect.id
				var label := node.get_child(0) as Label
				if label != null:
					label.text = "%d particle(s)" % effect.pdefs.size()
			"particle":
				var particle := info.obj as NovaParticleDef
				node.title = "● %s" % particle.id
				var label := node.get_child(0) as Label
				if label != null:
					label.text = _particle_summary(particle)
			"table":
				var table := info.obj as NovaParticleTable
				node.title = "∿ %s" % table.id
				var spark := node.get_child(0) as Control
				if spark != null:
					spark.queue_redraw()
	_graph.clear_connections()
	_rebuild_connections()
	_highlight_selected()


func _particle_summary(particle) -> String:
	var layers := 0
	for layer in particle.get_graphics():
		if layer != null and layer.present:
			layers += 1
	return "%d layer(s) · rate %.0f" % [layers, particle.emit_rate]


# --- Interaction -------------------------------------------------------------

func _on_connection_request(from_node: StringName, from_port: int, to_node: StringName, to_port: int) -> void:
	var from_info: Dictionary = _nodes.get(from_node, {})
	var to_info: Dictionary = _nodes.get(to_node, {})
	if from_info.is_empty() or to_info.is_empty():
		return
	if from_info.kind == "effect" and to_info.kind == "particle":
		# Membership: add the particle id to the effect's pdefs.
		if _workspace != null:
			_workspace.effect_add_pdef(from_info.obj, to_info.obj.id)
		if not _graph.is_node_connected(from_node, from_port, to_node, to_port):
			_graph.connect_node(from_node, from_port, to_node, to_port)
	elif from_info.kind == "particle" and to_info.kind == "particle":
		# Spawn chain: a particle has at most one child particle, so replace any
		# existing spawn edge from this particle first.
		_clear_spawn_edges_from(from_node)
		if _workspace != null:
			_workspace.set_child_id(from_info.obj, to_info.obj.id)
		if not _graph.is_node_connected(from_node, from_port, to_node, to_port):
			_graph.connect_node(from_node, from_port, to_node, to_port)
	# Any other combination (table involvement, effect->effect, ...) is ignored.


func _on_disconnection_request(from_node: StringName, from_port: int, to_node: StringName, to_port: int) -> void:
	var from_info: Dictionary = _nodes.get(from_node, {})
	var to_info: Dictionary = _nodes.get(to_node, {})
	if from_info.is_empty() or to_info.is_empty():
		return
	if from_info.kind == "effect" and to_info.kind == "particle":
		if _workspace != null:
			_workspace.effect_remove_pdef(from_info.obj, to_info.obj.id)
	elif from_info.kind == "particle" and to_info.kind == "particle":
		if _workspace != null:
			_workspace.set_child_id(from_info.obj, "")
	if _graph.is_node_connected(from_node, from_port, to_node, to_port):
		_graph.disconnect_node(from_node, from_port, to_node, to_port)


func _clear_spawn_edges_from(node_name: StringName) -> void:
	for c in _graph.get_connection_list():
		if String(c.from_node) == String(node_name):
			var to_info: Dictionary = _nodes.get(c.to_node, {})
			if not to_info.is_empty() and to_info.kind == "particle":
				_graph.disconnect_node(c.from_node, c.from_port, c.to_node, c.to_port)


func _on_node_selected(node: Node) -> void:
	if _syncing or _workspace == null:
		return
	var info: Dictionary = _nodes.get(node.name, {})
	if info.is_empty():
		return
	match info.kind:
		"effect":
			_select_workflow(ParticleEditorWorkspace.Workflow.EFFECTS)
			_workspace.select_effect(info.obj)
		"particle":
			_select_workflow(ParticleEditorWorkspace.Workflow.PARTICLES)
			_workspace.select_particle(info.obj)
		"table":
			_select_workflow(ParticleEditorWorkspace.Workflow.TABLES)
			_workspace.select_table(info.obj)


func _on_delete_nodes_request(node_names: Array) -> void:
	# Removing one entry rebuilds/reindexes the graph synchronously. Resolve the
	# whole user selection first so later deletes cannot drift onto new n0/n1 rows.
	var selected: Array[Dictionary] = []
	for n in node_names:
		var info: Dictionary = _nodes.get(n, {})
		if not info.is_empty():
			selected.append({"kind": String(info.kind), "obj": info.obj})
	if _workspace == null:
		return
	for info in selected:
		match info.kind:
			"effect":
				_workspace.remove_effect(info.obj)
			"particle":
				_workspace.remove_particle(info.obj)
			"table":
				_workspace.remove_table(info.obj)


func _select_workflow(workflow_id: int) -> void:
	if _workspace == null:
		return
	# The workspace re-syncs the shell rail + inspector through its seam.
	_workspace.select_workflow(workflow_id)


func _highlight_selected() -> void:
	if _editor == null or _graph == null:
		return
	_syncing = true
	var selected_obj: Object = _editor.current_particle
	if _workspace != null and _workspace.has_method("get_active_workflow_id"):
		match int(_workspace.get_active_workflow_id()):
			ParticleEditorWorkspace.Workflow.EFFECTS:
				selected_obj = _editor.current_effect
			ParticleEditorWorkspace.Workflow.TABLES:
				selected_obj = _editor.current_table
			_:
				selected_obj = _editor.current_particle
	for node_name in _nodes:
		var node := _graph.get_node_or_null(NodePath(String(node_name))) as GraphNode
		if node == null:
			continue
		var info: Dictionary = _nodes[node_name]
		var is_sel: bool = info.obj == selected_obj
		node.set_selected(is_sel)
	_syncing = false


# --- Tiny inline sparkline for table nodes -----------------------------------

class _TableSpark extends Control:
	var table

	func _ready() -> void:
		queue_redraw()

	func _draw() -> void:
		var w := size.x
		var h := size.y
		draw_rect(Rect2(Vector2.ZERO, size), Color(0.10, 0.12, 0.14), true)
		if table == null:
			return
		var data: PackedByteArray = table.get_data()
		if data.size() < 2:
			return
		var prev := Vector2(0.0, h - float(data[0]) / 255.0 * h)
		for i in range(1, data.size()):
			var x := float(i) / float(data.size() - 1) * w
			var y := h - float(data[i]) / 255.0 * h
			var pt := Vector2(x, y)
			draw_line(prev, pt, Color(0.85, 0.95, 0.45), 1.0)
			prev = pt
