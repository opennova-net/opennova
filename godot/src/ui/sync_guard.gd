class_name SyncGuard
extends RefCounted
## Lives in engine/ui (not modtools): shell-neutral by contract, so both the
## game's debug overlay and editor inspectors can use it.
## Reentrancy guard for inspector UI<->model sync. While run(body) executes,
## active is true; field setters check active and no-op so programmatic control
## updates do not echo back into the model. Replaces the ad-hoc
## {"value": false} dict-box that lambdas used to mutate a captured flag.

var active := false


func run(body: Callable) -> void:
	active = true
	body.call()
	active = false
