class_name JoinExpansionPlan
extends RefCounted

## What a LAN joiner must do to its mounted resource root once the host's authoritative
## expansion arrives in S2C 0x7B (net-re §5.32 field 7).
##
## The joiner used to mount from the LOCAL persisted expansion setting and never look at
## the host's, while still echoing the host's ServerHello.SUS2 in its C2S JOIN — so both
## host gates passed while the two sides held different data. That matters because the ADM
## weapon index space is expansion-scoped: weapon.def rows are assigned to ADM slots in pure
## file order [orig: WeaponDefs_ParseLineCallback @0x5436e1 -> AdmDef_FindFreeSlot @0x53FC50],
## and the shipped JO base and jox01 files diverge at the seventh `weapon` row. Every wire ADM
## index from there on then names a different weapon on each side (D-NET-178).
##
## Retail decides and executes the same thing one step earlier, off the session record the
## browser already holds: it copies that record's expansion over the pending name and switches
## THE ONE global mount in place before it connects [orig: UI_JoinSelectedSession @ 0x5699d0
## (expansion copy @ 0x569afa, switch @ 0x569b02, connect @ 0x569ded) -> Expansion_SwitchTo
## @ 0x5688c0 -> PFF_CloseAllOpenArchives @ 0x4a4380 / PFF_OpenAllArchives @ 0x4a4310]. A LAN
## joiner has no such record, so we run it on S2C 0x7B instead; the switch is sticky either way.
##
## This is the pure decision: it reads no files and mounts nothing, so both directions of the
## mismatch (expansion host vs base mount, base host vs expansion mount) are directly testable.
## `GameWorld` executes it.

## Nothing to do: the mounted root already holds the host's expansion.
const ACTION_KEEP := 0
## Re-point the mounted root at `expansion` (which may be "" for base game), in place — retail
## switches the one global mount, and there is no second mount object to swap in.
const ACTION_REMOUNT := 1
## The host's expansion is not installed here; `error` says so and the join must abort.
const ACTION_FAIL := 2

var action := ACTION_KEEP
## The expansion to mount, in its ON-DISK spelling when the install has one ("" = base game).
var expansion := ""
## Populated only for ACTION_FAIL: the join-failure reason shown to the player.
var error := ""


## Decide from the host's authoritative expansion, the expansion the root has ACTUALLY
## mounted (never the one it was asked for — see NovaResourceRoot::get_expansion), and the
## expansions installed under this resource directory (NovaResourceRoot.list_expansions).
## All three comparisons are case-insensitive: the wire carries the host's spelling and the
## installed set carries the local filesystem's.
static func decide(host_expansion: String, mounted_expansion: String,
		installed: PackedStringArray) -> JoinExpansionPlan:
	var plan := JoinExpansionPlan.new()
	var host := host_expansion.strip_edges()
	var mounted := mounted_expansion.strip_edges()
	if host.to_lower() == mounted.to_lower():
		plan.action = ACTION_KEEP
		plan.expansion = mounted
		return plan
	# A BASE-GAME host while an expansion is mounted locally is exactly as corrupting as the
	# reverse, and base game is always mountable — no installed-set lookup applies.
	if host.is_empty():
		plan.action = ACTION_REMOUNT
		plan.expansion = ""
		return plan
	for name in installed:
		if String(name).strip_edges().to_lower() == host.to_lower():
			plan.action = ACTION_REMOUNT
			plan.expansion = String(name)
			return plan
	plan.action = ACTION_FAIL
	plan.error = "join: host runs expansion '%s' which is not installed (installed: %s)" % [
		host, describe_installed(installed)]
	return plan


## The installed set as it appears in a failure reason. Kept next to the reason it feeds so
## the "nothing installed" wording has one home.
static func describe_installed(installed: PackedStringArray) -> String:
	if installed.is_empty():
		return "none — base game only"
	return ", ".join(installed)
