// The §5.10b class-tag dispatch table (docs/net/novaworld-net-re.md §5.10b).
// Seeded from the item's *_function class-tag in items.def at load time
// [orig: ItemDef+356].
#include "npwire/entity_class.h"

#include <cstring>

namespace opennova {

// §5.10b class dispatch — exact 4-char match (the table is case-sensitive).
// Direct witnesses: plyr→Player; org0/org1→Infantry; cveh/CHel/cbot/cpln/ctrn→
// Vehicle; rokt/stng/hlfr/jvln/arty→Guided. Known null callbacks map to a
// zero-body tag==1 header; unwitnessed tags default Unknown.
EntityClass class_from_tag(const char *tag) {
	if (!tag || tag[0] == '\0') return EntityClass::Unknown;
	if (std::strcmp(tag, "plyr") == 0) return EntityClass::Player;
	if (std::strcmp(tag, "org0") == 0 || std::strcmp(tag, "org1") == 0)
		return EntityClass::Infantry;
	if (std::strcmp(tag, "cveh") == 0 || std::strcmp(tag, "chel") == 0 ||
	    std::strcmp(tag, "CHel") == 0 || std::strcmp(tag, "cbot") == 0 ||
	    std::strcmp(tag, "cpln") == 0 || std::strcmp(tag, "ctrn") == 0)
		return EntityClass::Vehicle;
	if (std::strcmp(tag, "rokt") == 0 || std::strcmp(tag, "stng") == 0 ||
	    std::strcmp(tag, "hlfr") == 0 || std::strcmp(tag, "jvln") == 0 ||
	    std::strcmp(tag, "arty") == 0 || std::strcmp(tag, "arti") == 0)
		return EntityClass::Guided;
	if (std::strcmp(tag, "null") == 0 || std::strcmp(tag, "brrl") == 0 ||
	    std::strcmp(tag, "envs") == 0 || std::strcmp(tag, "ewep") == 0 ||
	    std::strcmp(tag, "ele0") == 0 || std::strcmp(tag, "gnrc") == 0 ||
	    std::strcmp(tag, "gnrl") == 0 || std::strcmp(tag, "gnl2") == 0 ||
	    std::strcmp(tag, "flag") == 0 || std::strcmp(tag, "squib") == 0 ||
	    std::strcmp(tag, "nade") == 0 || std::strcmp(tag, "schl") == 0 ||
	    std::strcmp(tag, "clym") == 0 || std::strcmp(tag, "vmne") == 0 ||
	    std::strcmp(tag, "lndm") == 0 || std::strcmp(tag, "bldg") == 0 ||
	    std::strcmp(tag, "bld2") == 0 || std::strcmp(tag, "cran") == 0 ||
	    std::strcmp(tag, "door") == 0 || std::strcmp(tag, "target") == 0 ||
	    std::strcmp(tag, "emit") == 0 || std::strcmp(tag, "towr") == 0 ||
	    std::strcmp(tag, "tree") == 0 || std::strcmp(tag, "palm") == 0 ||
	    std::strcmp(tag, "psec") == 0 || std::strcmp(tag, "pwrp") == 0 ||
	    std::strcmp(tag, "aflr") == 0 || std::strcmp(tag, "gflr") == 0)
		return EntityClass::NoNetworkCallback;
	return EntityClass::Unknown;
}

} // namespace opennova
