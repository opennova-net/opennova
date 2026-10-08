extends GameProbe

## Reads the mounted Play child's definitions through the runtime databases.
## It accepts expected values, never a path to a second resource root.

func run(ctx: ProbeContext) -> ProbeVerdict:
	for frame in range(600):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		if ctx.resource_root() != null:
			break
		await ctx.wait_frames(1)
	var root := ctx.resource_root()
	if root == null:
		return ProbeVerdict.failed("no mounted resource root")
	var items := ItemDatabase.new()
	var weapons := WeaponDatabase.new()
	var ammo := AmmoDatabase.new()
	if items.load_from_resource_root(root, "items.def") != OK \
			or weapons.load_from_resource_root(root, "weapon.def") != OK \
			or ammo.load_from_resource_root(root, "ammo.def") != OK:
		return ProbeVerdict.failed("the runtime could not load the built catalogs")
	var item_id := int(ctx.args["item_id"])
	var weapon_index := weapons.find_weapon(str(ctx.args["weapon_name"]))
	var ammo_index := ammo.find_ammo(str(ctx.args["ammo_name"]))
	if not items.has_item(item_id) or items.get_display_name(item_id) != str(ctx.args["item_name"]):
		return ProbeVerdict.failed("the built item is missing or differs")
	if weapon_index < 0 or weapons.get_weapon(weapon_index).get_clipsize() != int(ctx.args["weapon_clipsize"]):
		return ProbeVerdict.failed("the built weapon is missing or differs")
	if ammo_index < 0 or ammo.get_velocity(ammo_index) != int(ctx.args["ammo_velocity"]):
		return ProbeVerdict.failed("the built ammo is missing or differs")
	return ProbeVerdict.passed("the Play child loaded all three edited catalogs", {
		"root": root.get_root_dir(), "items": items.get_count(),
		"weapons": weapons.get_count(), "ammo": ammo.get_count(),
	})
