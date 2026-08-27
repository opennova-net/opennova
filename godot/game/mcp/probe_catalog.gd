class_name ProbeCatalog
extends RefCounted

## THE list of runtime probes (docs/mcp.md, ADR 0041): every game_probe entry
## is declared here with its script under res://probes/. The scripts are
## source-only (export_presets.cfg excludes probes/*), so a shipped build
## still lists the catalog but reports each probe unavailable; load_probe
## resolves them with load() at run time for that reason.


static func definitions() -> Array[ProbeDef]:
	return []


static func definition(name: String) -> ProbeDef:
	for def in definitions():
		if def.name == name:
			return def
	return null


## Whether the probe's script is present in this build.
static func is_available(def: ProbeDef) -> bool:
	return def != null and not def.script_path.is_empty() \
			and ResourceLoader.exists(def.script_path)


## A fresh probe instance, or null when the script is absent or is not a
## GameProbe.
static func load_probe(def: ProbeDef) -> GameProbe:
	if not is_available(def):
		return null
	var script := load(def.script_path) as GDScript
	if script == null:
		return null
	var instance: Variant = script.new()
	return instance as GameProbe
