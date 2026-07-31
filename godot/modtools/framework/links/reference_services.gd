class_name ReferenceServices
extends RefCounted
## The reference-strip service bundle riding the shell's reference index: the
## three Callables ReferenceStrip needs, as one typed record (ADR 0017). Built
## in exactly one place — from_shell — which replaces the near-duplicate
## inline builders the workspaces and the Mns inspector used to carry. Null
## (from from_shell on a headless/partial shell) means no services: adopters
## skip the strip.

## func(name: String) -> Array[ReferenceEdge].
var referrers: Callable = Callable()
## func() -> bool — whether the whole-root graph is already built (querying
## then is free, so the strip skips its "Find uses" button).
var is_ready: Callable = Callable()
## func(kind: String, path: String) — open a source file in its workspace.
var jump: Callable = Callable()


static func make(p_referrers: Callable, p_is_ready: Callable, p_jump: Callable) -> ReferenceServices:
	var services := ReferenceServices.new()
	services.referrers = p_referrers
	services.is_ready = p_is_ready
	services.jump = p_jump
	return services


## The editor trio over a shell's reference index; null when the shell (or its
## index/jump surface) is missing — headless tests, runtime owners. Referrer
## edges carry VFS-logical source names, so the jump resolves them through the
## shell root first (ReferenceStrip.resolve_source_path).
static func from_shell(shell: Object) -> ReferenceServices:
	if shell == null or not shell.has_method("get_reference_index") \
			or not shell.has_method("open_in_workspace"):
		return null
	return make(
		func(name: String) -> Array[ReferenceEdge]:
			return ReferenceEdge.from_dict_rows(shell.get_reference_index().referrers_of(name)),
		func() -> bool:
			return shell.get_reference_index().is_built(),
		func(kind: String, path: String) -> void:
			shell.open_in_workspace(kind, ReferenceStrip.resolve_source_path(shell, path)))
