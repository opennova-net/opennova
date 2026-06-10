class_name MissionEntityFields
extends RefCounted

# Single ordered description of the per-entity AI / waypoint SPIN fields in the inspector's Behavior
# section. _build_behavior_section iterates this instead of hand-writing one call per row, so the
# field set + ranges live in one place. Each `property` must match the entity dictionary key (and the
# name->member table in libs/mission set_entity_property_int); a `{section: ...}` entry emits a
# heading; an optional `tip` sets the row's tooltip.
#
# The waypoint-path picker, the name1 / name2 / ai_flags text fields, and the top-of-panel team /
# group rows stay in code: they are pickers / text / custom-formatted, not plain spins.
const SPIN_FIELDS := [
	{"property": "wp_number", "label": "WP number", "min": 0.0, "max": 255.0},
	{"section": "Combat"},
	{"property": "perception", "label": "Perception", "min": -1000000.0, "max": 1000000.0},
	{"property": "accuracy", "label": "Accuracy", "min": -32768.0, "max": 32767.0},
	{"property": "alert_state", "label": "Alert state", "min": 0.0, "max": 255.0},
	{"property": "min_engagement_distance", "label": "Min combat range", "min": -1000000.0, "max": 1000000.0},
	{"property": "max_engagement_distance", "label": "Max combat range", "min": -1000000.0, "max": 1000000.0},
	{"property": "max_attack_distance", "label": "Max attack range", "min": -1000000.0, "max": 1000000.0},
	{"section": "Spawning"},
	{"property": "spawn_count", "label": "Spawn count", "min": -32768.0, "max": 32767.0},
	{"property": "max_simultaneous", "label": "No more than", "min": 0.0, "max": 255.0,
		"tip": "Max copies kept (no_more_than, byte 74). Pairs with the RemoveIfMoreThan AI flag."},
	{"property": "no_less_than", "label": "No less than", "min": 0.0, "max": 255.0,
		"tip": "Min copies kept (no_less_than, byte 75). Pairs with the RemoveIfLessThan AI flag."},
	{"section": "Identity"},
	{"property": "map_symbol", "label": "Map symbol", "min": 0.0, "max": 255.0,
		"tip": "Tactical-map icon index (byte 81)."},
]
