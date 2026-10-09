class_name RuntimeFixture
extends RefCounted

## Synthetic, test-only boot data. Materialized outside the Godot resource tree;
## never exported or used as a runtime default. Per-format fixtures supply fonts,
## terrain, dialog and images; native document writers supply the mission/text.

static var _directory := ""

const TEXT_FILES := {
	"items.def": """begin "Player #1, Single player"
  id 105310
  type person
  graphic US01
  graphicenemy Indo01
  sid player1_sp
  anim_def US01
  hp 150
  sound_profile SP_JO_SP_PlayerM1
  sound_profileFemale SP_JO_SP_PlayerF1
  ai_function plyr
  disk_function PLAYER
  move_function org2
end

begin "Null"
  id 100000
  type marker
end

begin "start, player"
  id 106001
  type marker
end

begin "start, dmatch"
  id 106002
  type marker
end

begin "start, Blue Team"
  id 106003
  type marker
end

begin "start, Red Team"
  id 106004
  type marker
end

begin "waypoint"
  id 106005
  type marker
end
""",
	"weapon.def": """ammoclass_max_carry CLASS_556MM		1000

weapon "WPN_M4AUTO"
	category 1
	rank     0
	statid   100
	clipsize    30
	maxclips    7
	startrounds 210
	ammoclass   CLASS_556MM 1
	round_type  AM_556MM
	flags       auto

	ANIMADM	AKM_1ST
	GFX1	AKM_1st
	GFX1A	ARMSG
	pos		10.0		0.0		-201.0			0.0		0.0		1.0
	TPOS	-28.046		21.531		-187.857		0.0		0.0		0.0

	ACTION	"IDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"EMPTYIDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"FIRE"
	DELAYEND	5
	ANIM		ANIM_WPN_FIRE
	FUNCTION	WPN_STD_FIRE
	END

	ACTION	"RECOIL"
	DELAYEND	0
	ANIM		ANIM_WPN_RECOIL
	FUNCTION	WPN_STD_RECOIL
	END

	ACTION	"RELOAD"
	DELAYSTART	196
	DELAYEND	auto
	ANIM		ANIM_WPN_RELOAD
	FUNCTION	WPN_STD_RELOAD
	END

	ACTION	"EMPTY"
	DELAYSTART	0
	DELAYEND	auto
	ANIM		ANIM_WPN_EMPTY
	FUNCTION	WPN_STD_EMPTY
	END

	ACTION	"SWITCHTO"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHTO
	END

	ACTION	"SWITCHFROM"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHFROM
	FUNCTION	WPN_STD_SWITCHFROM
	END

	ACTION	"SWITCHRANK"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHRANK
	END
end

weapon "WPN_AK47AUTO"
	category 1
	rank     0
	statid   101
	clipsize    30
	maxclips    7
	startrounds 210
	ammoclass   CLASS_556MM 1
	round_type  AM_556MM
	flags       auto

	ANIMADM	AKM_1ST
	GFX1	AKM_1st
	GFX1A	ARMSG
	pos		10.0		0.0		-201.0			0.0		0.0		1.0
	TPOS	-28.046		21.531		-187.857		0.0		0.0		0.0

	ACTION	"IDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"EMPTYIDLE"
	DELAYEND	auto
	ANIM		ANIM_WPN_IDLE
	FUNCTION	WPN_STD_IDLE
	END

	ACTION	"FIRE"
	DELAYEND	5
	ANIM		ANIM_WPN_FIRE
	FUNCTION	WPN_STD_FIRE
	END

	ACTION	"RECOIL"
	DELAYEND	0
	ANIM		ANIM_WPN_RECOIL
	FUNCTION	WPN_STD_RECOIL
	END

	ACTION	"RELOAD"
	DELAYSTART	196
	DELAYEND	auto
	ANIM		ANIM_WPN_RELOAD
	FUNCTION	WPN_STD_RELOAD
	END

	ACTION	"EMPTY"
	DELAYSTART	0
	DELAYEND	auto
	ANIM		ANIM_WPN_EMPTY
	FUNCTION	WPN_STD_EMPTY
	END

	ACTION	"SWITCHTO"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHTO
	END

	ACTION	"SWITCHFROM"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHFROM
	FUNCTION	WPN_STD_SWITCHFROM
	END

	ACTION	"SWITCHRANK"
	DELAYSTART	1
	DELAYEND	1
	ANIM		ANIM_WPN_SWITCHRANK
	FUNCTION	WPN_STD_SWITCHRANK
	END
end
""",
	"ammo.def": """ammo AT_NULL
	velocity            0
	max_age             0
	drag                1
	min_damage          0
	max_damage          0
end

ammo AM_556MM
	velocity            3000
	max_age             300
	drag                1
	weight_in_grains    62
	min_damage          25
	max_damage          40
	penetration_impact  100
end
""",
	"main.mnu": """<!-- Minimal main.mnu for fixtures/minimal/ — the boot entry screen. The engine
     loads main.mnu and selects the "Startup" node [orig: Menu_InitShellResources @ 0x552651
     -> CUIScene_SelectNodeByName @ 0x63b6b0]. Authored from scratch (no retail
     asset); parses through libs/mnu. Just the two paths a minimal host+join
     needs: LAN multiplayer and exit. -->
<SCREEN>
	<NAME>STARTUP</NAME>
	<MUSICVAR>1</MUSICVAR>
	<WINDOW type="window" name="MAIN">
		<APPEARANCE type="custom" state="default"></APPEARANCE>
		<POSITION>
			<LEFT>0</LEFT>
			<TOP>75</TOP>
			<RIGHT>800</RIGHT>
			<BOTTOM>525</BOTTOM>
		</POSITION>
		<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>
		<CURSOR>
			<FILE>newarow1.tga</FILE>
			<FLAGS>STANDARD_TRANSPARENT</FLAGS>
		</CURSOR>
		<WINDOW type="window" name="BUTTONS">
			<FONT>
				<NAME>%DEF_FONTNAME_LG%</NAME>
				<DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG>
				<MOUSEOVER_FG>%DEF_TEXT_MOUSEOVER_FG%</MOUSEOVER_FG>
				<SELECTED_FG>%DEF_TEXT_SELECTED_FG%</SELECTED_FG>
				<DISABLED_FG>%DEF_TEXT_DISABLED_FG%</DISABLED_FG>
			</FONT>
			<POSITION>
				<LEFT>0</LEFT>
				<TOP>453</TOP>
			</POSITION>
			<WINDOW type="button" name="SINGLE_PLAYER">
				<ACTION type="screen" file="sp.mnu">SINGLE_PLAYER</ACTION>
				<APPEARANCE state="default"></APPEARANCE>
				<APPEARANCE state="mouseover"></APPEARANCE>
				<APPEARANCE state="selected"></APPEARANCE>
				<APPEARANCE state="disabled"></APPEARANCE>
				<POSITION>
					<TOP>0</TOP>
					<LEFT>40</LEFT>
					<RIGHT>200</RIGHT>
				</POSITION>
				<STRING type="id" justify="LEFT">MM_Singleplayer</STRING>
			</WINDOW>
			<WINDOW type="button" name="LAN_MULTI_PLAYER">
				<ACTION type="screen" file="mp.mnu">LAN_MULTI_PLAYER</ACTION>
				<APPEARANCE state="default"></APPEARANCE>
				<APPEARANCE state="mouseover"></APPEARANCE>
				<APPEARANCE state="selected"></APPEARANCE>
				<APPEARANCE state="disabled"></APPEARANCE>
				<POSITION>
					<TOP>0</TOP>
					<LEFT>240</LEFT>
					<RIGHT>520</RIGHT>
				</POSITION>
				<STRING type="id" justify="CENTER">MM_LANMultiplayer</STRING>
			</WINDOW>
			<WINDOW type="button" name="EXIT">
				<ACTION type="exit">EXIT</ACTION>
				<APPEARANCE state="default"></APPEARANCE>
				<APPEARANCE state="mouseover"></APPEARANCE>
				<APPEARANCE state="selected"></APPEARANCE>
				<APPEARANCE state="disabled"></APPEARANCE>
				<POSITION>
					<TOP>0</TOP>
					<LEFT>660</LEFT>
					<RIGHT>760</RIGHT>
				</POSITION>
				<STRING type="id" justify="CENTER">MM_Exit</STRING>
			</WINDOW>
		</WINDOW>
	</WINDOW>
</SCREEN>
""",
	"mp.mnu": """<!-- Minimal mp.mnu for fixtures/minimal/ — the LAN multiplayer host/join screen
     [orig: reached from main.mnu @ 0x5588fa]. Authored from scratch (no retail
     asset); parses through libs/mnu. Host creates a match on the custom map;
     Join finds it on the LAN. Back returns to Startup. -->
<SCREEN>
	<NAME>LAN_MULTI_PLAYER</NAME>
	<WINDOW type="window" name="MAIN">
		<APPEARANCE type="custom" state="default"></APPEARANCE>
		<POSITION>
			<LEFT>0</LEFT>
			<TOP>75</TOP>
			<RIGHT>800</RIGHT>
			<BOTTOM>525</BOTTOM>
		</POSITION>
		<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>
		<CURSOR>
			<FILE>newarow1.tga</FILE>
			<FLAGS>STANDARD_TRANSPARENT</FLAGS>
		</CURSOR>
		<WINDOW type="window" name="BUTTONS">
			<POSITION>
				<LEFT>0</LEFT>
				<TOP>453</TOP>
			</POSITION>
			<WINDOW type="button" name="HOST">
				<ACTION type="command">HOST_GAME</ACTION>
				<APPEARANCE state="default"></APPEARANCE>
				<POSITION>
					<TOP>0</TOP>
					<LEFT>150</LEFT>
					<RIGHT>290</RIGHT>
				</POSITION>
				<STRING type="id" justify="CENTER">MP_Host</STRING>
			</WINDOW>
			<WINDOW type="button" name="JOIN">
				<ACTION type="command">JOIN_GAME</ACTION>
				<APPEARANCE state="default"></APPEARANCE>
				<POSITION>
					<TOP>0</TOP>
					<LEFT>310</LEFT>
					<RIGHT>450</RIGHT>
				</POSITION>
				<STRING type="id" justify="CENTER">MP_Join</STRING>
			</WINDOW>
			<WINDOW type="button" name="BACK">
				<ACTION type="screen" file="main.mnu">STARTUP</ACTION>
				<APPEARANCE state="default"></APPEARANCE>
				<POSITION>
					<TOP>0</TOP>
					<LEFT>620</LEFT>
					<RIGHT>760</RIGHT>
				</POSITION>
				<STRING type="id" justify="CENTER">MP_Back</STRING>
			</WINDOW>
		</WINDOW>
	</WINDOW>
</SCREEN>
""",
	"sp.mnu": """<!-- Minimal sp.mnu for fixtures/minimal/ — the single-player mission screen.
     main.mnu's SINGLE_PLAYER button opens this by file+node name. The control
     NAMES are retail's own (IA_LIST / BRIEFING / ACCEPT / BACK) because the
     engine drives them by name, not by position: the populate handler fills
     IA_LIST from the mission catalog and enables ACCEPT on selection
     [orig: SinglePlayer_PopulateMissionList @ 0x561840,
     SinglePlayer_MissionListEventHandler @ 0x561ed0,
     SinglePlayer_RefreshAcceptOnActivate @ 0x561a20]. Rows are every .bms the
     mount can see whose game-type code word lands in the waypoint/Co-op family;
     a .bms with no attrib mode bits set is Single Player, which is what
     minimal_map_gen authors. Authored from scratch (no retail asset). -->
<SCREEN>
	<NAME>SINGLE_PLAYER</NAME>
	<MUSICVAR>1</MUSICVAR>
	<WINDOW type="window" name="MAIN">
		<APPEARANCE type="custom" state="default"></APPEARANCE>
		<POSITION>
			<LEFT>0</LEFT>
			<TOP>75</TOP>
			<RIGHT>800</RIGHT>
			<BOTTOM>525</BOTTOM>
		</POSITION>
		<TEXT_RSRC>menutxt.BIN</TEXT_RSRC>
		<CURSOR>
			<FILE>newarow1.tga</FILE>
			<FLAGS>STANDARD_TRANSPARENT</FLAGS>
		</CURSOR>
		<FONT>
			<NAME>%DEF_FONTNAME%</NAME>
			<DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG>
			<MOUSEOVER_FG>%DEF_TEXT_MOUSEOVER_FG%</MOUSEOVER_FG>
			<SELECTED_FG>%DEF_TEXT_SELECTED_FG%</SELECTED_FG>
			<DISABLED_FG>%DEF_TEXT_DISABLED_FG%</DISABLED_FG>
		</FONT>
		<WINDOW type="static" name="CA_LABEL">
			<APPEARANCE state="default"></APPEARANCE>
			<POSITION>
				<LEFT>40</LEFT>
				<TOP>42</TOP>
			</POSITION>
			<STRING type="ID" justify="LEFT">SP_Campaigns</STRING>
		</WINDOW>
		<WINDOW type="list" name="IA_LIST">
			<APPEARANCE state="default"></APPEARANCE>
			<POSITION>
				<LEFT>40</LEFT>
				<TOP>73</TOP>
				<RIGHT>380</RIGHT>
				<BOTTOM>395</BOTTOM>
			</POSITION>
			<ITEMS justify="LEFT" vjustify="CENTER">
				<APPEARANCE type="color" state="selected">%ITEM_SELECTED_BG%</APPEARANCE>
			</ITEMS>
			<MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
		</WINDOW>
		<WINDOW type="static" name="BRIEFING_LABEL">
			<APPEARANCE state="default"></APPEARANCE>
			<POSITION>
				<LEFT>429</LEFT>
				<TOP>42</TOP>
			</POSITION>
			<STRING type="ID" justify="LEFT">SP_MissionDesc</STRING>
		</WINDOW>
		<WINDOW type="multiline_edit" name="BRIEFING" READONLY>
			<APPEARANCE state="default"></APPEARANCE>
			<POSITION>
				<LEFT>429</LEFT>
				<TOP>73</TOP>
				<RIGHT>760</RIGHT>
				<BOTTOM>395</BOTTOM>
			</POSITION>
		</WINDOW>
		<WINDOW type="button" name="BACK">
			<ACTION type="screen" file="main.mnu">STARTUP</ACTION>
			<APPEARANCE state="default"></APPEARANCE>
			<APPEARANCE state="mouseover"></APPEARANCE>
			<APPEARANCE state="selected"></APPEARANCE>
			<APPEARANCE state="disabled"></APPEARANCE>
			<FONT>
				<NAME>%DEF_FONTNAME_LG%</NAME>
				<DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG>
				<MOUSEOVER_FG>%DEF_TEXT_MOUSEOVER_FG%</MOUSEOVER_FG>
				<SELECTED_FG>%DEF_TEXT_SELECTED_FG%</SELECTED_FG>
				<DISABLED_FG>%DEF_TEXT_DISABLED_FG%</DISABLED_FG>
			</FONT>
			<POSITION>
				<LEFT>40</LEFT>
				<TOP>425</TOP>
				<RIGHT>200</RIGHT>
			</POSITION>
			<STRING type="id" justify="LEFT">NAV_BACK</STRING>
		</WINDOW>
		<WINDOW type="button" name="ACCEPT">
			<HOTKEY VIRTUAL>VK_RETURN</HOTKEY>
			<APPEARANCE state="default"></APPEARANCE>
			<APPEARANCE state="mouseover"></APPEARANCE>
			<APPEARANCE state="selected"></APPEARANCE>
			<APPEARANCE state="disabled"></APPEARANCE>
			<FONT>
				<NAME>%DEF_FONTNAME_LG%</NAME>
				<DEFAULT_FG>%DEF_TEXT_FG%</DEFAULT_FG>
				<MOUSEOVER_FG>%DEF_TEXT_MOUSEOVER_FG%</MOUSEOVER_FG>
				<SELECTED_FG>%DEF_TEXT_SELECTED_FG%</SELECTED_FG>
				<DISABLED_FG>%DEF_TEXT_DISABLED_FG%</DISABLED_FG>
			</FONT>
			<POSITION>
				<LEFT>560</LEFT>
				<TOP>425</TOP>
				<RIGHT>760</RIGHT>
			</POSITION>
			<STRING type="id" justify="RIGHT">NAV_ACCEPT</STRING>
		</WINDOW>
	</WINDOW>
</SCREEN>
""",
	"menu_style.mns": """DEF_FONTNAME Arial16n.fnt
DEF_FONTNAME_LG Arial16b.fnt
IMPACT_FONTNAME Impac38b.fnt
DEF_TEXT_FG FFFFFFFF
DEF_TEXT_MOUSEOVER_FG FFFFC040
DEF_TEXT_SELECTED_FG FFFF8000
DEF_TEXT_DISABLED_FG FF545252
TRIM_COLOR FF808080
ITEM_SELECTED_BG 80FF8000
COLOR_BLACK FF000000
SEMIOPAQUE_BLACK 80000000
DEF_IMAGE_DEFAULT_BG FF000000
""",
	"mnml.trn": """terrain_name     "mnml"

water_height     0

polytrn_colormap         mnml_c.tga
polytrn_detailmap        mnml_dm.tga
polytrn_detailmap_c1     mnml_dc1.tga
polytrn_detailmap_c2     mnml_dc2.tga
polytrn_detailmap_c3     mnml_dc3.tga
polytrn_detailmapdist    mnml_dmd.tga
polytrn_polydata         mnml.cpt
polytrn_tilestrip        mnml_t.tga
polytrn_charmap          mnml_m.pcx
polytrn_foliagemap       mnml_f.pcx
polytrn_detailblendmap   mnml_d1.tga

polytrn_detaildensity		128
polytrn_detaildensity2		8
polytrn_sectorcount		8
polytrn_wrapx			0
polytrn_wrapy			0

polytrn_origin			-4	-4

polytrn_sectors			0	0	0	0	0	0	0	0
polytrn_sectors			0	0	0	0	0	0	0	0
polytrn_sectors			0	0	0	0	0	0	0	0
polytrn_sectors			0	0	0	1	3	0	0	0
polytrn_sectors			0	0	0	2	4	0	0	0
polytrn_sectors			0	0	0	0	0	0	0	0
polytrn_sectors			0	0	0	0	0	0	0	0
polytrn_sectors			0	0	0	0	0	0	0	0
""",
	"mnml.env": """enviro_name "mnml"

timeofday "Day"

envscale 1

iris_percent 15
iris_center 1

water_rgb 56,59,39

sky_map1 Cloud01.pcx
sky_map2 Cloud01b.pcx
sky_height 175
sky_speed 15

fog_level 1000
fog_type 2

cloud_rgb 128,128,128
terrain_rgb 255,255,255
vertex_rgb 128,128,128

lightning_rgb 85,85,90
sun_3di msun.3di
moon_3di fmoon4.3di
glare_3di mglare.3di
star_3di

ceiling_rgb 55,55,55
floor_rgb 25,25,25

curtime 1200

water_murk 0.8
advanced_clouds 1

tod_begin 0000
    sun_rgb 0,0,0
    moon_rgb 47,66,86
    sky_rgb 35,36,59
    ground_rgb 14,29,45
    skyfog_rgb 1,2,6
    fog_rgb 1,2,6
    skybase_rgb 35,36,59
    skybright_rgb 35,36,59
    skyhighlight_rgb 0,0,0
    cloudbase_rgb 35,36,59
    cloudhighlight_rgb 14,29,45
    cloudedge_rgb 14,29,45
tod_end

tod_begin 1200
    sun_rgb 170,170,167
    moon_rgb 0,0,0
    sky_rgb 84,88,89
    ground_rgb 49,55,46
    skyfog_rgb 77,91,138
    fog_rgb 77,91,138
    skybase_rgb 84,88,89
    skybright_rgb 84,88,89
    skyhighlight_rgb 170,170,167
    cloudbase_rgb 84,88,89
    cloudhighlight_rgb 49,55,46
    cloudedge_rgb 49,55,46
tod_end

tod_begin 2359
    sun_rgb 0,0,0
    moon_rgb 47,66,86
    sky_rgb 35,36,59
    ground_rgb 14,29,44
    skyfog_rgb 1,2,6
    fog_rgb 1,2,6
    skybase_rgb 35,36,59
    skybright_rgb 35,36,59
    skyhighlight_rgb 0,0,0
    cloudbase_rgb 35,36,59
    cloudhighlight_rgb 14,29,44
    cloudedge_rgb 14,29,44
tod_end
""",
}


static func directory() -> String:
	if not _directory.is_empty():
		return _directory
	_directory = ProjectSettings.globalize_path("res://../.godot-test-fixtures").path_join(
			"runtime_%d" % Time.get_ticks_usec())
	assert(DirAccess.make_dir_recursive_absolute(_directory) == OK)
	for name in TEXT_FILES:
		write_bytes(name, String(TEXT_FILES[name]).replace("\n", "\r\n").to_utf8_buffer())
	for name in ["Arial12b", "Arial14b", "Arial14n", "Arial16b", "Arial16n", "Impac22b", "Impac38b"]:
		copy_fixture("fnt/synth_1page.fnt", name + ".fnt")
	copy_fixture("terrain/tmap/Tmap.cpt", "mnml.cpt")
	copy_fixture("terrain/tmap/Tmap_m.pcx", "mnml_m.pcx")
	copy_fixture("terrain/tmap/Tmap_f.pcx", "mnml_f.pcx")
	copy_fixture("env/cloud01.pcx", "mnml.pcx")
	copy_fixture("dbf/synth_bank.dbf", "mnml.dbf")
	var bank := LwfData.new()
	bank.create_empty()
	assert(bank.save_file(_directory.path_join("mnml.lwf")) == OK)
	# All channels have the same value, so RGBA bytes are also BGRA TGA bytes.
	for name in ["mnml_c", "mnml_dm", "mnml_dc1", "mnml_dc2", "mnml_dc3", "mnml_dmd", "mnml_d1", "mnml_t", "newarow1"]:
		var size := 2048 if name == "mnml_c" or name == "mnml_d1" else 32
		var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
		image.fill(Color(0.5, 0.5, 0.5, 1.0))
		var header := PackedByteArray()
		header.resize(18)
		header[2] = 2 # Uncompressed true-color TGA.
		header.encode_u16(12, size)
		header.encode_u16(14, size)
		header[16] = 32
		header[17] = 0x28 # Top-origin, eight alpha bits.
		write_bytes(name + ".tga", header + image.get_data())
	for name in ["gameerr.bin", "vmacros.bin", "keyhelp.bin"]:
		write_bytes(name, RtxtStringFile.new().to_byte_array())
	copy_fixture("rtxt/synth_game.bin", "gametext.bin")
	var labels := RtxtStringFile.new()
	labels.add_section("Menu")
	for key in ["MM_Singleplayer", "MM_LANMultiplayer", "MM_Exit", "MP_Host", "MP_Join", "MP_Back", "SP_Campaigns", "SP_MissionDesc", "NAV_ACCEPT", "NAV_BACK"]:
		labels.add_entry(key, key, 0, Vector2i())
	write_bytes("menutxt.bin", labels.to_byte_array())
	var briefing := RtxtStringFile.new()
	briefing.add_section("Info")
	briefing.add_entry("TITLE", "Synthetic mission", 0, Vector2i())
	briefing.add_entry("BRIEFING", "Runtime integration fixture.", 0, Vector2i())
	write_bytes("mnml.bin", briefing.to_byte_array())
	var mission := MissionData.new()
	assert(mission.create_default() == OK)
	assert(mission.set_header_string("mission_name", "Synthetic mission"))
	assert(mission.set_header_string("terrain", "mnml"))
	assert(mission.set_header_string("environment", "mnml"))
	assert(mission.set_header_int("start_time", 12 * 256))
	assert(mission.set_header_int("minutes_per_day", 1440))
	for item_id in [106001, 106003, 106004]:
		assert(mission.add_entity(MissionData.KIND_MARKER, item_id,
				Vector3.ZERO, Vector3.ZERO) != null)
	assert(mission.save_as(_directory.path_join("mnml.bms")) == OK)
	return _directory


static func file(name: String) -> String:
	return directory().path_join(name)


static func write_bytes(name: String, bytes: PackedByteArray) -> void:
	var target := FileAccess.open(_directory.path_join(name), FileAccess.WRITE)
	assert(target != null)
	target.store_buffer(bytes)
	target.close()


static func copy_fixture(source: String, name: String) -> void:
	var bytes := FileAccess.get_file_as_bytes("res://../fixtures/" + source)
	assert(not bytes.is_empty(), "fixture is available: " + source)
	write_bytes(name, bytes)
