#include <runtime/inmatch/vehicle_menu_controller.h>

#include <base/io/strutil.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/menu/menu_runtime.h>

namespace opennova::inmatch {

namespace {

const char *const kList = "ITEM_LIST";

bool same_rows(const std::vector<VehicleMenuRow> &a, const std::vector<VehicleMenuRow> &b) {
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].text != b[i].text)
			return false;
	return true;
}

} // namespace

void VehicleMenuController::open(
		menu::MenuRuntime &menu, const std::string &, const std::string &) {
	const VehicleMenuSession session = host_ ? host_->session() : VehicleMenuSession();
	if (!session)
		return;
	// [orig: UI_OpenMenuScreen @0x54E520 — the load @0x54E57D (the init event, VehicleMenu_Open
	//  @0x564240, on the first parse), the select @0x54E599 (the activate event,
	//  VehicleMenu_OnActivate @0x563AD0)]
	session.runtime->vehicle_menu().open(*session.runtime, *session.world, session.items);
	shown_rows_.clear();
	shown_selection_ = -2;
	refresh(menu);
}

void VehicleMenuController::refresh(menu::MenuRuntime &menu) {
	const VehicleMenuSession session = host_ ? host_->session() : VehicleMenuSession();
	const int list = menu.widget_id(kList);
	if (!session || list < 0)
		return;
	const VehicleMenu &vehicles = session.runtime->vehicle_menu();
	const int selected = vehicles.selected_row();
	if (same_rows(vehicles.rows(), shown_rows_) && selected == shown_selection_)
		return;
	shown_rows_ = vehicles.rows();
	shown_selection_ = selected;
	// The group names are items.def bytes (the machine's code page), as the list holds them.
	std::vector<std::string> items;
	items.reserve(shown_rows_.size());
	for (const VehicleMenuRow &row : shown_rows_)
		items.push_back(row.text);
	menu.set_widget_items(list, items);
	// A selection outside the rows (none, or past a refill's end) shows no row.
	const int rows = static_cast<int>(shown_rows_.size());
	menu.select_row(list, selected >= 0 && selected < rows ? selected : -1, false);
}

void VehicleMenuController::on_event(menu::MenuRuntime &, const menu::MenuEvent &event) {
	if (event.kind == menu::MenuEvent::Kind::ValueChanged) {
		// [orig: UI_OnItemListSelectionChanged @0x563B30 (registered @0x564215, event mask 32)]
		if (!strutil::iequals(event.text, kList) || event.value < 0)
			return;
		const VehicleMenuSession session = host_ ? host_->session() : VehicleMenuSession();
		if (session)
			session.runtime->vehicle_menu().select_row(event.value);
		return;
	}
	if (event.kind != menu::MenuEvent::Kind::WidgetActivated)
		return;
	// [orig: VehicleMenu_OnButton @0x563B60 — ACCEPT user data 0 (@0x5641d9), CANCEL 1 (@0x5641f7)]
	if (strutil::iequals(event.text, "ACCEPT"))
		click(false);
	else if (strutil::iequals(event.text, "CANCEL"))
		click(true);
}

void VehicleMenuController::click(bool cancel) {
	const VehicleMenuSession session = host_ ? host_->session() : VehicleMenuSession();
	if (session)
		session.runtime->vehicle_menu().click(*session.runtime, *session.world, cancel);
	if (host_)
		host_->close();
}

std::string VehicleMenuController::preview_graphic() {
	const VehicleMenuSession session = host_ ? host_->session() : VehicleMenuSession();
	if (!session || session.items == nullptr)
		return std::string();
	return vehicle_menu_preview_graphic(
			*session.items, session.runtime->vehicle_menu().selection());
}

} // namespace opennova::inmatch
