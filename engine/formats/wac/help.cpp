#include <formats/wac/help.h>
#include <formats/wac/command.h>

#include <array>
#include <cstring>
#include <fstream>
#include <ostream>

namespace opennova::wac {
namespace {

// The original hashes six padded name bytes, then the last two nonzero name
// bytes. These IDs belong to the event XML consumer, not WAC bytecode opcodes.
// [orig: WacScript_DumpActionDefsToXML @0x4F0560]
uint32_t event_id(const char *name) {
    std::array<uint8_t, 19> padded{};
    const size_t size = std::strlen(name);
    for (size_t i = 0; i < size && i < padded.size(); ++i)
        padded[i] = static_cast<uint8_t>(name[i]);
    uint32_t hash = 0;
    for (int i = 5; i >= 0; --i) hash = (hash << 4) ^ (padded[size_t(i)] & 31u);
    hash <<= 6;
    int last = 18;
    while (last >= 0 && padded[size_t(last)] == 0) --last;
    if (last >= 0) {
        hash ^= padded[size_t(last)] & 31u;
        if (last > 0) hash ^= 2u * (padded[size_t(last - 1)] & 31u);
        hash ^= 8u * (uint32_t(last) & 7u);
    }
    return hash;
}

// Text lists use a bit-mask test and retain registry order and parameter slots.
// [orig: WacScript_DumpActionDefsToFile @0x4F0400]
void write_text_definitions(std::ostream &out, uint8_t mask) {
    for (int i = 0; i < wac_command_count(); ++i) {
        const CommandDef &command = wac_commands()[i];
        if ((command.flags & mask) == 0) continue;
        out << "//   " << command_signature(command) << "\n";
    }
}

// XML uses exact flags-without-condition-bit equality. Replicated actions
// therefore appear in the text list but not in this XML's action category.
// [orig: WacScript_DumpActionDefsToXML @0x4F0560]
void write_xml_definitions(std::ostream &out, uint8_t type) {
    const char *tag = type == 0 ? "CONDITION" : "ACTION";
    for (int i = 0; i < wac_command_count(); ++i) {
        const CommandDef &command = wac_commands()[i];
        if ((command.flags & 0xFE) != type) continue;
        out << "  <" << tag << " id=\"" << event_id(command.name) << "\">"
            << "<NAME>" << command.name << "</NAME><FORMAT>" << command.name;
        for (int param = 0; param < 4; ++param) {
            if (command.params[param] == ParamType::Null) continue;
            out << (param == 0 ? " " : ", ") << "%" << param + 1;
        }
        out << "</FORMAT><WAC>" << command.name << "(";
        for (int param = 0; param < 4; ++param) {
            if (command.params[param] == ParamType::Null) continue;
            if (param != 0) out << ",";
            out << param_type_name(command.params[param]);
        }
        out << ")</WAC></" << tag << ">\n";
    }
}

} // namespace

std::string command_signature(const CommandDef &command) {
    std::string text = command.name;
    text += " (";
    for (int param = 0; param < 4; ++param) {
        if (command.params[param] == ParamType::Null) continue;
        if (param != 0) text += ", ";
        text += param_type_name(command.params[param]);
    }
    text += ")";
    return text;
}

// File I/O follows the original open/close ordering. Failure after opening is
// not separately reported by retail's handler; only each fopen result gates it.
// [orig: WacCmd_Help @0x4F6DE0]
HelpExportResult export_help() {
    HelpExportResult result;
    {
        std::ofstream help("help.wac", std::ios::out | std::ios::trunc);
        if (!help.is_open()) return result;
        help << "// WAC Quick Reference Help File\n"
             << "\n"
             << "// WAC Flow Control (basic statements)\n"
             << "//  IF triggers THEN actions END\n"
             << "//  IF [ifname] triggers THEN actions END (optional named if)\n"
             << "//  IF triggers THEN actions ELSE actions END\n"
             << "//  IF triggers THEN actions ELSEIF triggers THEN actions END\n"
             << "//  PLOOP actions(player) END\n"
             << "//  GLOOP group actions(item) END\n"
             << "//  DOSEQ actions NEXT actions [NEXT actions..] END\n"
             << "//  DORND actions NEXT actions [NEXT actions..] END\n"
             << "//  IF triggers ENTER actions END (triggers on first true)\n"
             << "//  IF triggers LEAVE actions END (triggers after last true)\n"
             << "\n"
             << "// WAC Compiler Commands\n"
             << "//  VAR variablename (declares a number variable, shows up on debug screen)\n"
             << "//  CHEAT cheatname (declares a server cheat variable, shows up on debug screen)\n"
             << "//  RUN filename (this includes the text from a file, good from command line)\n"
             << "\n"
             << "// WAC Logic and Math (tests for inside IF statement)\n"
             << "//  Arithmatics ()+-*/%^\n"
             << "//  Booleans AND OR NOT < > <= >= == !=\n"
             << "//  Example: IF V1<12 THEN\n"
             << "//  Example: IF V1<12 AND V2<12 THEN\n"
             << "//  Note: this compiles to IF (V1<12) AND (V2<12) THEN\n"
             << "\n"
             << "// WAC Assignment Var = Value\n"
             << "//  Example: V1 = 12\n"
             << "//  Example: V2 = V2+V3*4\n"
             << "//  Note: This compiles to V2+(V3*4)\n"
             << "\n"
             << "// WAC Triggers (Things to do after IF statement)\n\n";
        write_text_definitions(help, 1);
        help << "\n// WAC Actions (Things to do after THEN/ENTER/LEAVE statement)\n\n";
        write_text_definitions(help, 2);
        help << "\n// WAC Debug Commands (This also contains old stuff)\n\n";
        write_text_definitions(help, 4);
        result.help_written = true;
    }
    {
        std::ofstream events("events.xml", std::ios::out | std::ios::trunc);
        if (!events.is_open()) return result;
        events << "<!-- TYPE id's must be unique for type level blocks -->\n"
               << "<!-- id's must be unique for condition/action level blocks -->\n"
               << "<EVENTS_RSRC>\n <CONDITION_TYPE id=\"0\">\n <NAME>Triggers</NAME>\n";
        write_xml_definitions(events, 0);
        events << " </CONDITION_TYPE>\n <ACTION_TYPE id=\"0\">\n <NAME>Events</NAME>\n";
        write_xml_definitions(events, 2);
        events << " </ACTION_TYPE>\n <ACTION_TYPE id=\"1\">\n <NAME>Debug</NAME>\n";
        write_xml_definitions(events, 4);
        events << " </ACTION_TYPE>\n</EVENTS_RSRC>\n";
        result.events_written = true;
    }
    return result;
}

} // namespace opennova::wac
