// The def catalogs' records in a modder's words (catalog_labels.h).
#include <editor/documents/catalog_labels.h>

#include <editor/documents/def_catalog_document.h>
#include <editor/graph/reference_kinds.h>

namespace opennova::editor {

using namespace def;

std::string catalog_record_label(const Document &document, const NodeAddress &address, const NameSource *names) {
	const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document);
	const void *native = catalog ? catalog->record(address) : nullptr;
	if (!native) return std::string();
	switch (CatalogKind(address.kind)) {
	case CatalogKind::Weapon: {
		const auto &weapon = *static_cast<const DefWeaponDef *>(native);
		std::string shown = wepdes_text(names, weapon.weapon_name);
		if (shown.empty()) shown = wepdes_text(names, weapon.loadout_menu_textid);
		return shown;
	}
	case CatalogKind::Ammo: return wepdes_text(names, static_cast<const DefAmmoDef *>(native)->name);
	case CatalogKind::Attachment: {
		// The child item the gun is, on the point of the vehicle's model it sits on [orig:
		// Mission_LoadBMSFile @ 0x40FD49..0x40FD96, the addeweap children spawned in two walks].
		const auto &gun = *static_cast<const DefItemEmplacementAttachment *>(native);
		const std::string id = std::to_string(gun.item_id);
		const GraphSymbol *item = names ? names->symbol(ReferenceKind::Item, id) : nullptr;
		const std::string what = item ? symbol_words(*item) : "Item " + id;
		return gun.userpoint[0] ? what + " on " + gun.userpoint : what;
	}
	case CatalogKind::PowerupAmmo: {
		// -1 fills the class, any other count is added [orig: PowerupAction_Pickup @ 0x4428A0].
		const auto &line = *static_cast<const DefPowerupAmmo *>(native);
		if (line.count == -1) return std::string("Fills ") + line.class_name;
		return std::to_string(line.count) + (line.count == 1 ? " round of " : " rounds of ") + line.class_name;
	}
	// The blocks the powerup runs when it is taken and when it comes back [orig: PowerUpDef_RegisterNewEntry
	// @ 0x442C00].
	case CatalogKind::Pickup: return "When taken";
	case CatalogKind::Respawn: return "When it comes back";
	default: return std::string();
	}
}

} // namespace opennova::editor
