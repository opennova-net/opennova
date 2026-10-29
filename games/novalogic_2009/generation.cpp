#include "generation.h"

#include <net/protocol/napi_transport.h>

namespace opennova::games {

namespace {

Generation make() {
	Generation g;
	// The NAPI library's version string and company, the same in both images
	// (net/protocol/napi_transport.h cites each).
	g.nvs = kNapiNpVersion;
	g.co = kNovaLogicCompany;
	// The protocol version a host checks (NCC007 on a mismatch).
	// [orig: CNapiNetwork_Init @0x4ca7db, the literal @0x7ca6b0]
	// [orig: @0x4ca94b (dfx2.exe)]
	g.pv2 = "16";
	// The joiner's CU values, its net config's defaults, and the join check's build gates on the
	// same constants. [orig: NapiNetConfig_Init @0x4c3570 - VN @0x4c3622, BN @0x4c3629, MBN
	//  @0x4c3630, SOPD @0x4c3637; CNapiServerInfo_SerializeToSession @0x4c3650 - VN @0x4c36ed,
	//  BN @0x4c372c, MBN @0x4c37ac, SOPD @0x4c37ed; Server_ValidatePlayerJoinRequest @0x512100 -
	//  BN 1 @0x5121b5 (code 2 @0x5121df), VN 2 @0x5121f3 (3), MBN 0x131D112 @0x51221a (4), SOPD
	//  180 @0x512286 (8)]
	// DFX2's are JO's: the same net config defaults and serializer, and the rewritten join check
	// keeps the build gates. [orig: NapiNetConfig_Init @0x4c3650 (dfx2.exe) - VN @0x4c3702, BN
	//  @0x4c3709, MBN @0x4c3710, SOPD @0x4c3717; CNapiServerInfo_SerializeToSession @0x4c3730
	//  (dfx2.exe); Server_ValidatePlayerJoinRequest
	//  @0x50f1d0 (dfx2.exe) - BN 1 @0x50f2cb (code 2), VN 2 @0x50f2ef (3), MBN 0x131D112
	//  @0x50f316 (4), SOPD 180 @0x50f382 (8)]
	g.join_build = JoinBuild{ 1, 2, 20042002, 180 };
	return g;
}

} // namespace

const Generation &novalogic_2009() {
	static const Generation generation = make();
	return generation;
}

} // namespace opennova::games
