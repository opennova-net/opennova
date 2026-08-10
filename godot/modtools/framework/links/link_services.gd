class_name LinkServices
extends RefCounted

## The typed resource-reference service trio (ADR 0017) every link widget
## consumes: resolve a (kind, name) to its status record, open the kind
## picker, and jump to the owning workspace. Built from the one shell
## contract; adopters with custom targets assign the Callables directly.

var resolve := Callable()
var pick := Callable()
var jump := Callable()


static func from_shell(shell: WorkspaceShell) -> LinkServices:
	var out := LinkServices.new()
	out.resolve = func(kind: String, name: String) -> Dictionary:
		return shell.get_reference_index().resolve(kind, name)
	out.pick = func(kind: String, title: String, on_pick: Callable) -> void:
		shell.open_kind_picker(kind, title, on_pick)
	out.jump = func(kind: String, path: String) -> void:
		shell.open_in_workspace(ResourceKinds.jump_kind(kind), path)
	return out
