#include "update_wizard.h"

#include "update_ini.h"

namespace opennova::update {

namespace {

constexpr std::string_view kHeader = "HEADER";

char ascii_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}
char ascii_upper_char(char c) {
	return (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
}

bool carries(const std::string &value, size_t max_chars) {
	if (value.size() > max_chars)
		return false;
	return value.find_first_of("\r\n;") == std::string::npos;
}

} // namespace

UpdateWizard read_update_wizard(std::string_view text) {
	const UpdateIni ini(text);
	auto get = [&](std::string_view key, size_t bytes) {
		return ini.value(kHeader, key, bytes).value_or(std::string());
	};
	UpdateWizard wizard;
	wizard.cmd = get("CMD", kWizardValueBytes);
	wizard.url = get("URL", kWizardValueBytes);
	wizard.url2 = get("URL2", kWizardValueBytes);
	wizard.url3 = get("URL3", kWizardValueBytes);
	wizard.dlg = get("DLG", kWizardValueBytes);
	wizard.product_login = get("PRODUCT_LOGIN", kWizardValueBytes);
	// DRIVE is a strncpy of one character into a one-byte field. [orig: Cupdate2Dlg_ReadWizard
	// @0x404e5a (UPDATE.EXE)]
	wizard.drive = get("DRIVE", 2);
	wizard.dir = get("DIR", kWizardValueBytes);
	wizard.local = get("LOCAL", kWizardValueBytes);
	wizard.territory = get("TERRITORY", kWizardValueBytes);
	return wizard;
}

std::string write_update_wizard(const UpdateWizard &wizard) {
	const struct {
		std::string_view key;
		const std::string &value;
		size_t max_chars;
	} rows[] = {
		{ "CMD", wizard.cmd, kWizardValueBytes - 1 },
		{ "URL", wizard.url, kWizardValueBytes - 1 },
		{ "URL2", wizard.url2, kWizardValueBytes - 1 },
		{ "URL3", wizard.url3, kWizardValueBytes - 1 },
		{ "DLG", wizard.dlg, kWizardValueBytes - 1 },
		{ "PRODUCT_LOGIN", wizard.product_login, kWizardValueBytes - 1 },
		{ "DRIVE", wizard.drive, 1 },
		{ "DIR", wizard.dir, kWizardValueBytes - 1 },
		{ "LOCAL", wizard.local, kWizardValueBytes - 1 },
		{ "TERRITORY", wizard.territory, kWizardValueBytes - 1 },
	};
	std::string out = "[HEADER]\r\n";
	for (const auto &row : rows) {
		if (row.value.empty())
			continue;
		// A key line is read through a 128-byte buffer. [orig: IniFile_GetValue @ 0x403b90
		// (UPDATE.EXE)]
		if (!carries(row.value, row.max_chars) ||
				row.key.size() + 1 + row.value.size() >= kIniKeyLineBytes)
			return {};
		out.append(row.key).append("=").append(row.value).append("\r\n");
	}
	return out;
}

Territory territory_from_cc_bin(const uint8_t *data, size_t size) {
	Territory territory;
	for (size_t i = 0; i < 2 && i < size; ++i)
		territory.code[i] = char(data[i]);
	for (size_t i = 2; i < 4 && i < size; ++i)
		territory.placeholder[i - 2] = char(data[i]);
	return territory;
}

std::string substitute_territory(std::string_view value, const Territory &territory) {
	std::string out(value);
	const std::string lower = { ascii_lower(territory.placeholder[0]),
		ascii_lower(territory.placeholder[1]) };
	size_t at = out.find(lower);
	if (at != std::string::npos) {
		out[at] = ascii_lower(territory.code[0]);
		out[at + 1] = ascii_lower(territory.code[1]);
		return out;
	}
	const std::string upper = { ascii_upper_char(territory.placeholder[0]),
		ascii_upper_char(territory.placeholder[1]) };
	at = out.find(upper);
	if (at != std::string::npos) {
		out[at] = ascii_upper_char(territory.code[0]);
		out[at + 1] = ascii_upper_char(territory.code[1]);
	}
	return out;
}

UpdateWizard with_territory(UpdateWizard wizard, const Territory &territory) {
	for (std::string *value : { &wizard.cmd, &wizard.url, &wizard.url2, &wizard.url3, &wizard.dlg,
				 &wizard.dir, &wizard.local, &wizard.territory, &wizard.product_login })
		*value = substitute_territory(*value, territory);
	return wizard;
}

} // namespace opennova::update
