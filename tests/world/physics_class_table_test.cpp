// The physics callback table's whole-name lookup (world/physics_class_table.h):
// an items.def move_function binds the row whose name equals the whole token
// ignoring case, and anything else binds row 0, null.
// [orig: EntityDef_LookupPhysicsCallback @0x4a9240, stricmp @0x4a9262 over the
//  34 rows of g_EntityClassPhysicsTable @0x82abc8, row 0 @0x4a9272]
#include <runtime/world/physics_class_table.h>

#include <cstdio>
#include <cstring>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

int main() {
	// The table order and spellings, as the image stores them.
	const char *expected[kPhysicsClassRowCount] = { "null", "envs", "ewep", "ele0", "door",
		"towr", "genx", "org0", "org1", "org2", "upfx", "nade", "rock", "schl", "clym", "arti",
		"squib", "CHel", "cveh", "ctank", "cbike", "cbot", "catv", "cpln", "ctrn", "chld", "aflr",
		"gflr", "rokt", "stng", "hlfr", "jvln", "arty", "psec" };
	for (size_t i = 0; i < kPhysicsClassRowCount; ++i) {
		const PhysicsClass row = static_cast<PhysicsClass>(i);
		CHECK(std::strcmp(physics_class_row_name(row), expected[i]) == 0);
		// Every row binds itself by its own name.
		CHECK(physics_class_from_move_function(expected[i]) == row);
	}

	// The 5-character rows match whole, and case never matters.
	CHECK(physics_class_from_move_function("ctank") == PhysicsClass::Ctank);
	CHECK(physics_class_from_move_function("cbike") == PhysicsClass::Cbike);
	CHECK(physics_class_from_move_function("squib") == PhysicsClass::Squib);
	CHECK(physics_class_from_move_function("chel") == PhysicsClass::Chel);
	CHECK(physics_class_from_move_function("CHEL") == PhysicsClass::Chel);
	CHECK(physics_class_from_move_function("ENVS") == PhysicsClass::Envs);
	CHECK(physics_class_from_move_function("CTank") == PhysicsClass::Ctank);

	// A prefix, an extension, a non-row name or no name binds the null row.
	for (const char *other : { "ctan", "cbik", "squi", "towr2", "doorX", "cvehicle",
				 "CHelScout", "plyr", "vmne", "lndm", "file", "Null", "", " cveh" })
		CHECK(physics_class_from_move_function(other) == PhysicsClass::Null);

	if (failures == 0) std::printf("physics_class_table: OK\n");
	return failures == 0 ? 0 : 1;
}
