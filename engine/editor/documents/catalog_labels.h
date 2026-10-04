#pragma once

#include <string>

#include <editor/documents/name_source.h>
#include <editor/model/document.h>

namespace opennova::editor {

// The def catalogs' records in a modder's words (ADR 0046, the UX round's plain-words lane; the
// catalog type's DocumentType::record_label): a weapon by the name the player sees for it, the text its
// id names in gametext.bin's WepDes section as the HUD shows it [orig: HUD_DrawWeaponAmmoAndName @
// 0x593b7f], else its loadout list's name [orig: PlayerInfo_PopulateWeaponSlotLists @ 0x560430]; an
// ammo by its round's name there [orig: PlayerInfo_PopulateAmmoComboBoxes @ 0x55def0]; a vehicle's
// mounted gun by its item's name and the point it sits on ("Hummer MG on ewep01"); a powerup's ammo
// line by what it gives, its action blocks by when they run. "" where the record's own name says it
// (an item's name is the words the catalog has for it), which record_display then shows.
std::string catalog_record_label(const Document &document, const NodeAddress &address, const NameSource *names);

} // namespace opennova::editor
