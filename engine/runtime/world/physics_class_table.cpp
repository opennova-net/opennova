// The physics callback table's rows and the whole-name lookup (physics_class_table.h).
#include <runtime/world/physics_class_table.h>

#include <base/io/strutil.h>

namespace opennova::world {

namespace {

// The row names in table order, spelled as the table stores them.
// [orig: g_EntityClassPhysicsTable @0x82abc8]
constexpr const char *kRowNames[kPhysicsClassRowCount] = { "null", "envs", "ewep", "ele0",
	"door", "towr", "genx", "org0", "org1", "org2", "upfx", "nade", "rock", "schl", "clym",
	"arti", "squib", "CHel", "cveh", "ctank", "cbike", "cbot", "catv", "cpln", "ctrn", "chld",
	"aflr", "gflr", "rokt", "stng", "hlfr", "jvln", "arty", "psec" };

static_assert(static_cast<size_t>(PhysicsClass::Psec) + 1 == kPhysicsClassRowCount,
		"one enumerator per table row");

} // namespace

const char *physics_class_row_name(PhysicsClass row) {
	const size_t index = static_cast<size_t>(row);
	return index < kPhysicsClassRowCount ? kRowNames[index] : kRowNames[0];
}

// [orig: EntityDef_LookupPhysicsCallback @0x4a9240: stricmp over every row
//  (@0x4a9262), row 0 when none matches (@0x4a9272)]
PhysicsClass physics_class_from_move_function(std::string_view move_function) {
	for (size_t index = 0; index < kPhysicsClassRowCount; ++index)
		if (strutil::iequals(move_function, kRowNames[index]))
			return static_cast<PhysicsClass>(index);
	return PhysicsClass::Null;
}

bool physics_class_keeps_height(PhysicsClass row) {
	switch (row) {
	case PhysicsClass::Null:
	case PhysicsClass::Envs:
	case PhysicsClass::Ewep:
	case PhysicsClass::Door:
	case PhysicsClass::Genx:
	case PhysicsClass::Upfx:
	case PhysicsClass::Org0:
	case PhysicsClass::Chld:
		return true;
	default:
		return false;
	}
}

} // namespace opennova::world
