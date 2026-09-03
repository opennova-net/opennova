class_name WorldView
extends RefCounted

## The narrow, live view of the running mission the in-world screens (deploy,
## end of round) and the debug click picker read: the simulation and the
## mounted resource root, both re-resolved per call (each mission brings new
## ones). The base answers none (no world); GameWorld.world_view() serves the
## live one over itself; a test fakes it by overriding the two verbs (ADR 0043
## rule 11: a presenter depends on the narrowest surface it uses, never on a
## world double).


func sim() -> Simulation:
	return null


func resource_root() -> ResourceRoot:
	return null
