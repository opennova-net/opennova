// The §5.10b class-tag dispatch table (docs/net/novaworld-net-re.md §5.10b).
// Seeded from the item's *_function class-tag in items.def at load time
// [orig: ItemDef+356].
#include "npwire/entity_class.h"

#include <io/strutil.h>

namespace opennova {

namespace {

bool tag_is(const char *tag, std::string_view key) {
	return strutil::iequals(tag, key);
}

} // namespace

// §5.10b class dispatch — case-INSENSITIVE match: the shipped items.def
// authors the same classes in both cases (`CHel` beside `chel`, `ENVS`/`GNRC`
// beside lowercase 4-char tags) and retail resolves them onto one class row
// each (envs→EnvSoundEmitter per the §5.10b census), so exact-case matching
// would fail-closed on witnessed null-callback items. Direct witnesses:
// plyr→Player; org0/org1→Infantry; cveh/CHel/cbot/cpln/ctrn→Vehicle;
// rokt/stng/hlfr/jvln/arty→Guided. Known null callbacks map to a zero-body
// tag==1 header; unwitnessed tags default Unknown.
EntityClass class_from_tag(const char *tag) {
	if (!tag || tag[0] == '\0') return EntityClass::Unknown;
	if (tag_is(tag, "plyr")) return EntityClass::Player;
	if (tag_is(tag, "org0") || tag_is(tag, "org1"))
		return EntityClass::Infantry;
	if (tag_is(tag, "cveh") || tag_is(tag, "chel") ||
	    tag_is(tag, "cbot") || tag_is(tag, "cpln") || tag_is(tag, "ctrn"))
		return EntityClass::Vehicle;
	if (tag_is(tag, "rokt") || tag_is(tag, "stng") || tag_is(tag, "hlfr") ||
	    tag_is(tag, "jvln") || tag_is(tag, "arty") || tag_is(tag, "arti"))
		return EntityClass::Guided;
	if (tag_is(tag, "null") || tag_is(tag, "brrl") || tag_is(tag, "envs") ||
	    tag_is(tag, "ewep") || tag_is(tag, "ele0") || tag_is(tag, "gnrc") ||
	    tag_is(tag, "gnrl") || tag_is(tag, "gnl2") ||
	    tag_is(tag, "flag") || tag_is(tag, "squib") || tag_is(tag, "nade") ||
	    tag_is(tag, "schl") || tag_is(tag, "clym") || tag_is(tag, "vmne") ||
	    tag_is(tag, "lndm") || tag_is(tag, "bldg") || tag_is(tag, "bld2") ||
	    tag_is(tag, "cran") || tag_is(tag, "door") || tag_is(tag, "target") ||
	    tag_is(tag, "emit") || tag_is(tag, "towr") || tag_is(tag, "tree") ||
	    tag_is(tag, "palm") || tag_is(tag, "psec") || tag_is(tag, "pwrp") ||
	    tag_is(tag, "aflr") || tag_is(tag, "gflr"))
		return EntityClass::NoNetworkCallback;
	return EntityClass::Unknown;
}

} // namespace opennova
