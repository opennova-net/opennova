extends GutTest

# Menu-RUNTIME semantics on MenuDriver (the binding over engine/runtime/menu/menu_runtime.cpp) — the
# carve-out coverage the deleted MnuMenu Control-tree tests pinned, now driven
# through the compiled surface and ported to retail's dispatch (the 2026-09-23
# grill): action dispatch (screen/pop/url/window/shell verbs, the activation's
# order), the per-screen MUSICVAR push, hotkey routing (the VK names, the label
# mnemonics of tabs, the edit focus, Enter's commit), the widget_value_changed
# relay kinds, combo popup lifecycle, edit focus/typing, checkbox/radio toggling,
# and sound-trigger edges. The engine owns the
# witnessed primitives (draw walk, pump, geometry, edit ops — pinned by ctest
# tests/menu/menu_frame_compiler_test.cpp); these tests pin the orchestration
# the driver performs around them.
#
# Frameless drivers exercise the pure store/dispatch seams; an attached
# 800x600 MenuFrame (design coords == local coords) drives the mouse paths.

const ACTIONS_XML := """
<SCREEN>
  <NAME>MAIN</NAME>
  <MUSICVAR>3</MUSICVAR>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="window" name="PANEL">
      <POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>200</RIGHT><BOTTOM>200</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="list" name="PICKLIST">
      <POSITION><LEFT>300</LEFT><TOP>10</TOP><RIGHT>400</RIGHT><BOTTOM>100</BOTTOM></POSITION>
      <ITEMS>
        <ITEM value="0">AAA</ITEM>
        <ITEM value="1">BBB</ITEM>
        <ITEM value="2">CCC</ITEM>
      </ITEMS>
      <LIST_BOX><MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT></LIST_BOX>
    </WINDOW>
    <WINDOW type="spinlist" name="SPIN">
      <POSITION><LEFT>300</LEFT><TOP>120</TOP><RIGHT>400</RIGHT><BOTTOM>140</BOTTOM></POSITION>
      <ITEMS>
        <ITEM value="0">LOW</ITEM>
        <ITEM value="1">MED</ITEM>
        <ITEM value="2">HIGH</ITEM>
      </ITEMS>
    </WINDOW>
  </WINDOW>
</SCREEN>
<SCREEN>
  <NAME>SUB</NAME>
  <WINDOW type="window" name="ROOT2">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="window" name="SUB_PANEL">
      <POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""

# The interactive board: hotkey targets, a combo with an authored popup
# (MIN_ITEM_HEIGHT keeps row geometry font-free), edits, toggles, a sound
# button, and a plain OTHER button for popup-dismissal checks.
const BOARD_XML := """
<SCREEN>
  <NAME>MAIN</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="window" name="HIDDEN_GROUP" HIDDEN>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>50</RIGHT><BOTTOM>50</BOTTOM></POSITION>
      <WINDOW type="button" name="HIDDEN_ESC">
        <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>40</RIGHT><BOTTOM>20</BOTTOM></POSITION>
        <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
      </WINDOW>
    </WINDOW>
    <WINDOW type="button" name="BACK">
      <POSITION><LEFT>600</LEFT><TOP>500</TOP><RIGHT>700</RIGHT><BOTTOM>530</BOTTOM></POSITION>
      <STRING>B{hot}ack</STRING>
      <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
      <HOTKEY>V</HOTKEY>
    </WINDOW>
    <WINDOW type="combobox" name="MODE">
      <POSITION><LEFT>200</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS>
        <ITEM value="0">ONE</ITEM>
        <ITEM value="1">TWO</ITEM>
      </ITEMS>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>80</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
      </LIST_BOX>
    </WINDOW>
    <WINDOW type="edit" name="NAME_EDIT">
      <POSITION><LEFT>10</LEFT><TOP>100</TOP><RIGHT>210</RIGHT><BOTTOM>120</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="edit" name="NUM_EDIT" NUMBER MINVAL="1" MAXVAL="99" MAXCHAR="3">
      <POSITION><LEFT>10</LEFT><TOP>140</TOP><RIGHT>210</RIGHT><BOTTOM>160</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="checkbox" name="CHK">
      <POSITION><LEFT>10</LEFT><TOP>200</TOP><RIGHT>110</RIGHT><BOTTOM>220</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="radio" name="R1">
      <GROUP>1</GROUP>
      <POSITION><LEFT>10</LEFT><TOP>240</TOP><RIGHT>110</RIGHT><BOTTOM>260</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="radio" name="R2">
      <GROUP>1</GROUP>
      <POSITION><LEFT>10</LEFT><TOP>280</TOP><RIGHT>110</RIGHT><BOTTOM>300</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="button" name="SND_BTN">
      <POSITION><LEFT>400</LEFT><TOP>200</TOP><RIGHT>500</RIGHT><BOTTOM>230</BOTTOM></POSITION>
      <SOUND state="selected" trigger="CLICK_SET">bank.lwf</SOUND>
    </WINDOW>
    <WINDOW type="button" name="OTHER">
      <POSITION><LEFT>500</LEFT><TOP>300</TOP><RIGHT>600</RIGHT><BOTTOM>330</BOTTOM></POSITION>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""

const ACTION_BUTTON_XML := """
<SCREEN>
  <NAME>MAIN</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="window" name="POPUP" HIDDEN>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="button" name="GO">
      <POSITION><LEFT>200</LEFT><TOP>200</TOP><RIGHT>300</RIGHT><BOTTOM>230</BOTTOM></POSITION>
      <HOTKEY VIRTUAL>VK_RETURN</HOTKEY>
      <ACTION type="window" state="SHOW">POPUP</ACTION>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""

const HIDDEN_ONLY_XML := """
<SCREEN>
  <NAME>S</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="window" name="HGROUP" HIDDEN>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>50</RIGHT><BOTTOM>50</BOTTOM></POSITION>
      <WINDOW type="button" name="HIDDEN_ESC">
        <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>40</RIGHT><BOTTOM>20</BOTTOM></POSITION>
        <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
      </WINDOW>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


func _doc(xml: String) -> MnuDocument:
	var doc := MnuDocument.new()
	assert_eq(doc.load_from_bytes(xml.to_utf8_buffer()), OK, "fixture parses")
	return doc


func _frameless_driver(xml: String, menu_file := "menu.mnu") -> MenuDriver:
	var driver := MenuDriver.new()
	assert_true(driver.open_document(_doc(xml), null, null, null, menu_file),
			"document opens frameless")
	return driver


func _framed_driver(xml: String, menu_file := "menu.mnu") -> MenuDriver:
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(_doc(xml), null, null, null, menu_file),
			"document opens on the frame")
	return driver


func _click(driver: MenuDriver, pos: Vector2) -> void:
	driver.process_mouse(pos, true)
	driver.process_mouse(pos, false)


func _click_widget(driver: MenuDriver, name: String) -> void:
	var rect := driver.widget_frame_rect(driver.widget_id(name))
	assert_gt(rect.size.x, 0.0, "%s has a solved rect to click" % name)
	_click(driver, rect.get_center())


func _key(keycode: Key, unicode := 0, pressed := true, echo := false) -> InputEventKey:
	var k := InputEventKey.new()
	k.keycode = keycode
	k.unicode = unicode
	k.pressed = pressed
	k.echo = echo
	return k


# --- (a) action dispatch --------------------------------------------------------


func test_attached_driver_releases_itself_and_its_document() -> void:
	var driver := _framed_driver(ACTIONS_XML)
	var driver_ref: WeakRef = weakref(driver)
	var document_ref: WeakRef = weakref(driver.document())
	driver.get_frame().configure(null, "", null, null, null)
	driver = null
	assert_null(driver_ref.get_ref(), "the frame's signal connections do not retain the driver")
	assert_null(document_ref.get_ref(), "closing the driver releases its document")


func test_screen_action_same_file_navigates_and_pushes_stack() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	# No FILE is no same-file jump (retail faults there; the editor refuses it).
	assert_false(driver.dispatch_action_row(MnuActionRow.make("screen", "SUB", "", false, "")),
			"a SCREEN row with no FILE does nothing")
	assert_eq(driver.get_current_screen(), "MAIN", "still MAIN")
	# Shipped same-file jumps spell their own filename, case-insensitively.
	assert_true(driver.dispatch_action_row(MnuActionRow.make("screen", "SUB", "", false, "MENU.MNU")),
			"own-filename screen action handled")
	assert_eq(driver.get_current_screen(), "SUB", "case-insensitive same-file jump navigated")
	assert_true(driver.pop_screen(), "pop returns")
	assert_eq(driver.get_current_screen(), "MAIN", "pop returned to MAIN")


func test_pop_past_root_emits_pop_requested() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	watch_signals(driver)
	assert_false(driver.pop_screen(), "pop past the file's own history is the shell's")
	assert_signal_emitted(driver, "pop_requested")
	assert_eq(driver.get_current_screen(), "MAIN", "screen unchanged")


func test_cross_file_screen_action_emits_menu_requested() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	watch_signals(driver)
	assert_true(driver.dispatch_action_row(MnuActionRow.make("screen", "LOBBY", "", false, "other.mnu")),
			"cross-file action consumed")
	assert_signal_emitted_with_parameters(driver, "menu_requested", ["other.mnu", "LOBBY"])
	assert_eq(driver.get_current_screen(), "MAIN", "cross-file jump does not navigate in-file")


func test_url_action_and_no_quit_token() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	watch_signals(driver)
	# QUIT is no ACTION type: code 0, nothing happens.
	assert_false(driver.dispatch_action_row(MnuActionRow.make("quit", "", "", false, "")),
			"quit is not an action")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("url", " www.novalogic.com\n", "", false, "")),
			"url consumed")
	assert_signal_emitted_with_parameters(driver, "url_requested", ["www.novalogic.com", false])
	assert_true(driver.dispatch_action_row(
			MnuActionRow.make("url", "www.x.com/?external_browser=1", "", false, "")))
	assert_signal_emitted_with_parameters(driver, "url_requested",
			["www.x.com/?external_browser=1", true], 1)


func test_window_actions_show_hide_enable_disable_toggle() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	var panel := driver.widget_id("PANEL")
	assert_true(driver.is_widget_shown(panel), "panel starts shown")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "hide", false, "")))
	assert_false(driver.is_widget_shown(panel), "hide hides")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "show", false, "")))
	assert_true(driver.is_widget_shown(panel), "show shows")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "hide", true, "")))
	assert_false(driver.is_widget_shown(panel), "hide+TOGGLE flips shown -> hidden")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "show", true, "")))
	assert_true(driver.is_widget_shown(panel), "show+TOGGLE flips hidden -> shown")
	assert_false(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "toggle", false, "")),
			"TOGGLE is no STATE (it is the flag)")
	assert_true(driver.is_widget_shown(panel), "an unknown STATE changes nothing")

	assert_false(driver.is_widget_disabled(panel), "panel starts enabled")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "disable", false, "")))
	assert_true(driver.is_widget_disabled(panel), "disable disables")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "enable", false, "")))
	assert_false(driver.is_widget_disabled(panel), "enable enables")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "disable", true, "")))
	assert_true(driver.is_widget_disabled(panel), "disable+TOGGLE flips enabled -> disabled")
	assert_true(driver.dispatch_action_row(MnuActionRow.make("window", "PANEL", "enable", true, "")))
	assert_false(driver.is_widget_disabled(panel), "enable+TOGGLE flips disabled -> enabled")


func test_window_action_rejects_off_screen_target() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	assert_eq(driver.get_current_screen(), "MAIN")
	assert_false(driver.dispatch_action_row(MnuActionRow.make("window", "SUB_PANEL", "hide", false, "")),
			"a WINDOW action only reaches the current screen's widgets")
	assert_false(driver.dispatch_action_row(MnuActionRow.make("window", "NO_SUCH", "hide", false, "")),
			"an unknown target is rejected")


func test_service_verbs_are_not_driver_actions() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	watch_signals(driver)
	var action := MnuActionRow.make("lan_search", "SERVER_ROWS", "", false, "")
	assert_false(driver.dispatch_action_row(action),
			"a service verb (menu-re.md: LAN_SEARCH) is not handled by the driver")
	assert_eq(driver.get_current_screen(), "MAIN", "the driver invents no LAN effects")
	assert_signal_not_emitted(driver, "screen_changed")
	assert_signal_not_emitted(driver, "menu_requested")


# Retail's order (menu-re.md, "Activation and the ACTION walk"): the ACTION rows
# run first, then the control callbacks (widget_activated), so an observer reads
# the screen the rows left.
func test_widget_activated_follows_action_dispatch() -> void:
	var driver := _framed_driver(ACTION_BUTTON_XML)
	var order: Array = []
	var popup_shown_at_emit: Array = []
	driver.widget_activated.connect(
			func(_id: int, name: String) -> void:
				order.append("activated:" + name)
				popup_shown_at_emit.append(
						driver.is_widget_shown(driver.widget_id("POPUP"))))
	assert_true(driver.handle_key_input(_key(KEY_ENTER)), "GO consumed")
	assert_eq(order, ["activated:GO"], "the activation observer ran once")
	assert_eq(popup_shown_at_emit, [true],
			"the observer runs after the ACTION list showed the popup")


# An observer that synchronously swaps the document (the game.mnu CONFIRM_YES ->
# teardown -> main.mnu chain) leaves the new document untouched: the rows ran on
# the old one before the callbacks.
func test_document_swap_during_activation_blocks_stale_dispatch() -> void:
	var driver := _framed_driver(ACTION_BUTTON_XML)
	# The swapped-in document authors the same hidden POPUP the stale GO
	# ACTION would show: it stays hidden only if that ACTION list never ran.
	driver.widget_activated.connect(
			func(_id: int, _name: String) -> void:
				assert_true(driver.open_document(
						_doc(ACTION_BUTTON_XML), null, null, null, "other.mnu"),
						"observer swaps the document mid-activation"))
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_ENTER)), "GO consumed")
	assert_signal_emitted(driver, "widget_activated")
	assert_eq(driver.get_menu_file(), "other.mnu", "the new document is live")
	assert_false(driver.is_widget_shown(driver.widget_id("POPUP")),
			"the new document is untouched by the stale ACTION list")


# --- (b) screen_changed + the MUSICVAR push -------------------------------------


const MUS_SCRIPT_FIXTURE := "res://../fixtures/mus/synth_gamemus.bin"
const MUSIC_VAR_INDEX := 4


func test_screen_changed_and_music_var_push_on_every_show() -> void:
	# A real director over the synthetic MUS script: the push lands in the
	# music VM's variable, read back through get_var.
	var script := MusicScript.new()
	assert_eq(script.load_from_path(MUS_SCRIPT_FIXTURE), OK, "the MUS fixture loads")
	var director := MusicDirector.new()
	add_child_autofree(director)
	director.load_mus_script(script)
	director.auto_start = false
	director.start()
	var driver := MenuDriver.new()
	driver.set_music_director(director)
	driver.set_music_var_index(MUSIC_VAR_INDEX)
	watch_signals(driver)
	assert_true(driver.open_document(_doc(ACTIONS_XML), null, null, null, "menu.mnu"))
	assert_signal_emitted_with_parameters(driver, "screen_changed", ["MAIN"])
	assert_eq(director.get_var(MUSIC_VAR_INDEX), 3, "the authored MUSICVAR is pushed on show")

	assert_true(driver.show_screen("SUB"))
	# SUB authors no MUSICVAR: the push still fires with zero [orig:
	# UI_DispatchScreenEvent @ 0x54e6a0 -> AudioVM_SetVariable @ 0x54eff4].
	assert_eq(director.get_var(MUSIC_VAR_INDEX), 0, "a no-MUSICVAR screen resets to 0")
	assert_signal_emit_count(driver, "screen_changed", 2)

	director.set_var(MUSIC_VAR_INDEX, 7)
	assert_true(driver.show_screen("SUB"))
	assert_eq(director.get_var(MUSIC_VAR_INDEX), 0, "repeated screen events repeat the push")
	assert_signal_emit_count(driver, "screen_changed", 3)


# --- (c) hotkeys through handle_key_input ---------------------------------------


func test_esc_hotkey_activates_authored_widget() -> void:
	var driver := _framed_driver(BOARD_XML)
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_ESCAPE)), "ESC consumed")
	# The hidden subtree's VK_ESCAPE prunes; the shown BACK activates.
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("BACK"), "BACK"])


func test_hidden_subtree_hotkey_never_fires() -> void:
	var driver := _framed_driver(HIDDEN_ONLY_XML)
	watch_signals(driver)
	assert_false(driver.handle_key_input(_key(KEY_ESCAPE)),
			"a hidden-subtree hotkey neither fires nor consumes")
	assert_signal_not_emitted(driver, "widget_activated")


func test_disabled_hotkey_row_is_skipped() -> void:
	# A row whose widget is not visible in the hierarchy (disabled here) is
	# skipped and the scan goes on [orig: UI_DispatchKeyboardEventToChildren
	# @ 0x63ad10]; with no other ESC row the key goes unanswered.
	var driver := _framed_driver(BOARD_XML)
	driver.set_widget_disabled(driver.widget_id("BACK"), true)
	watch_signals(driver)
	assert_false(driver.handle_key_input(_key(KEY_ESCAPE)),
			"a disabled row neither fires nor consumes")
	assert_signal_not_emitted(driver, "widget_activated")


func test_character_hotkey_fires_case_insensitively() -> void:
	var driver := _framed_driver(BOARD_XML)
	watch_signals(driver)
	# BACK authors <HOTKEY>V</HOTKEY>; a lowercase 'v' keystroke matches.
	assert_true(driver.handle_key_input(_key(KEY_V, "v".unicode_at(0))),
			"lowercase character hotkey consumed")
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("BACK"), "BACK"])


func test_label_marker_hotkey_fires_and_stays_out_of_display_text() -> void:
	var driver := _framed_driver(BOARD_XML)
	var back := driver.widget_id("BACK")
	assert_eq(driver.get_widget_text(back), "Back",
			"the {hot} marker is not part of the displayed label")
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_A, "a".unicode_at(0))),
			"a lowercase label mnemonic is consumed")
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[back, "BACK"])


const KEYS_XML := """
<SCREEN>
  <NAME>MAIN</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="radio" name="TAB_VIDEO">
      <GROUP>1</GROUP>
      <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <STRING>{hot}Video</STRING>
    </WINDOW>
    <WINDOW type="radio" name="TAB_AUDIO" CHECKED>
      <GROUP>1</GROUP>
      <POSITION><LEFT>100</LEFT><TOP>0</TOP><RIGHT>200</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <STRING>{hot}Audio</STRING>
    </WINDOW>
    <WINDOW type="edit" name="PLAYERNAME">
      <POSITION><LEFT>10</LEFT><TOP>100</TOP><RIGHT>210</RIGHT><BOTTOM>120</BOTTOM></POSITION>
    </WINDOW>
    <WINDOW type="button" name="ACCEPT">
      <POSITION><LEFT>600</LEFT><TOP>500</TOP><RIGHT>700</RIGHT><BOTTOM>530</BOTTOM></POSITION>
      <HOTKEY VIRTUAL>VK_RETURN</HOTKEY>
    </WINDOW>
    <WINDOW type="button" name="CANCEL">
      <POSITION><LEFT>400</LEFT><TOP>500</TOP><RIGHT>500</RIGHT><BOTTOM>530</BOTTOM></POSITION>
      <HOTKEY VIRTUAL>VK_ESCAPE</HOTKEY>
    </WINDOW>
    <WINDOW type="button" name="DEAD">
      <POSITION><LEFT>200</LEFT><TOP>500</TOP><RIGHT>300</RIGHT><BOTTOM>530</BOTTOM></POSITION>
      <HOTKEY VIRTUAL>VK_ENTER</HOTKEY>
      <HOTKEY VIRTUAL>VK_SPACE</HOTKEY>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


# A tab radio's {hot} label letter selects it (the 18 shipped tab labels carry
# one): the mnemonic registers for every class whose parse reads a STRING
# [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30].
func test_tab_radio_selects_by_its_letter() -> void:
	var driver := _framed_driver(KEYS_XML)
	var video := driver.widget_id("TAB_VIDEO")
	var audio := driver.widget_id("TAB_AUDIO")
	assert_true(driver.is_widget_checked(audio), "AUDIO starts checked")
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_V, "v".unicode_at(0))), "the letter is consumed")
	assert_true(driver.is_widget_checked(video), "the letter selects the tab")
	assert_false(driver.is_widget_checked(audio), "the sibling of its GROUP unchecks")
	assert_signal_emitted_with_parameters(driver, "widget_activated", [video, "TAB_VIDEO"])


# VK_ENTER names no key (only VK_RETURN, VK_ESCAPE and VK_SPACE do)
# [orig: CWnd_ParseVirtualKeyNameW @ 0x6467f0]: the shipped cmap WPNAME_OK and
# loadout ACCEPT rows are dead; VK_SPACE works.
func test_vk_names() -> void:
	var driver := _framed_driver(KEYS_XML)
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_ENTER)), "Enter is ACCEPT's")
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("ACCEPT"), "ACCEPT"])
	assert_true(driver.handle_key_input(_key(KEY_SPACE, " ".unicode_at(0))), "Space fires")
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("DEAD"), "DEAD"], 1)
	assert_signal_emit_count(driver, "widget_activated", 2,
			"Enter never reached the VK_ENTER row")


# With an edit focused the hotkey scan is off: ESC does nothing; Enter commits
# the edit, drops the focus and then presses the VK_RETURN button
# [orig: CEditWnd_HandleKeyEvent @ 0x6623a0 -> dispatch_key_event @ 0x63ac30].
func test_edit_focus_esc_and_enter() -> void:
	var driver := _framed_driver(KEYS_XML)
	var edit := driver.widget_id("PLAYERNAME")
	_click_widget(driver, "PLAYERNAME")
	assert_eq(driver.get_focused_widget(), edit, "the click focused the edit")
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_ESCAPE)), "the focused edit takes ESC")
	assert_signal_not_emitted(driver, "widget_activated", "ESC with the edit focused does nothing")
	assert_eq(driver.get_focused_widget(), edit, "the focus stays")
	assert_true(driver.handle_key_input(_key(KEY_V, "v".unicode_at(0))), "typed")
	assert_false(driver.is_widget_checked(driver.widget_id("TAB_VIDEO")),
			"a letter goes to the edit, not to the tab")
	assert_true(driver.handle_key_input(_key(KEY_ENTER)), "Enter consumed")
	assert_eq(driver.get_focused_widget(), -1, "the commit dropped the focus")
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["PLAYERNAME", "edit", -1, "v"])
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("ACCEPT"), "ACCEPT"])


# --- (d) widget_value_changed relay kinds ---------------------------------------


func test_select_row_emits_list_kind() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	var list := driver.widget_id("PICKLIST")
	watch_signals(driver)
	driver.select_row(list, 1)
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["PICKLIST", "list", 1, "BBB"])
	assert_eq(driver.selected_row(list), 1, "selection tracked")


func test_spin_cycle_wraps_and_emits_spinlist() -> void:
	var driver := _frameless_driver(ACTIONS_XML)
	var spin := driver.widget_id("SPIN")
	assert_eq(driver.selected_row(spin), 0, "spin starts on the first row")
	watch_signals(driver)
	driver.spin_cycle(spin, -1)
	assert_eq(driver.selected_row(spin), 2, "cycling back from 0 wraps to the last row")
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["SPIN", "spinlist", 2, "HIGH"])
	driver.spin_cycle(spin, 1)
	assert_eq(driver.selected_row(spin), 0, "cycling forward wraps modulo the row count")
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["SPIN", "spinlist", 0, "LOW"], 1)


func test_combo_popup_mouse_select() -> void:
	var driver := _framed_driver(BOARD_XML)
	var combo := driver.widget_id("MODE")
	_click_widget(driver, "MODE")
	assert_true(driver.is_combo_popup_open(combo), "clicking the combo opens its popup")
	watch_signals(driver)
	# The authored LIST_BOX (0,20)-(100,80) offsets to (200,20)-(300,80) with
	# 20px rows: (250,45) is popup row 1 [orig: D-MNU-7 combo-relative rect].
	_click(driver, Vector2(250, 45))
	assert_eq(driver.selected_row(combo), 1, "popup row click selects")
	assert_false(driver.is_combo_popup_open(combo), "a row pick closes the popup")
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["MODE", "combo", 1, "TWO"])


const SLIDER_XML := """
<SCREEN>
  <NAME>OPTIONS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="scroll" name="GAMMA">
      <POSITION><LEFT>100</LEFT><TOP>72</TOP><RIGHT>300</RIGHT><BOTTOM>92</BOTTOM></POSITION>
      <ORIENTATION>HORIZONTAL</ORIENTATION>
      <APPEARANCE type="color" state="default">303030</APPEARANCE>
      <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
      <SCROLLUP type="color" state="default">505050</SCROLLUP>
      <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


func test_scroll_slider_arrows_track_and_thumb_drag() -> void:
	# The witnessed CScrollWnd interaction [orig: CScrollWnd_HandleEvent
	# @ 0x64d050 — arrows step 1, track pages, the shuttle press captures and
	# drags through the travel ratio; change dispatch 0x4000001].
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(_doc(SLIDER_XML), null, null, null,
			"options.mnu"), "document opens on the frame")
	var gamma := driver.widget_id("GAMMA")
	driver.set_widget_scroll_range(gamma, 0, 100, 10, 50)
	watch_signals(driver)
	# Track 120..280, value 50 -> shuttle mid-track. Right arrow steps +1.
	_click(driver, Vector2(290, 80))
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["GAMMA", "scroll", 51, "51"])
	# Track before the shuttle pages down by the inclusive page.
	_click(driver, Vector2(125, 80))
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["GAMMA", "scroll", 41, "41"])
	# Thumb drag: press on the shuttle, drag to the track end, release.
	var index := frame.widget_index("GAMMA")
	var thumb_rect_probe := frame.scroll_hit_at(index, Vector2(180, 80))
	assert_eq(thumb_rect_probe, MenuFrame.SCROLL_HIT_SHUTTLE,
			"the mid-track point is the shuttle")
	driver.process_mouse(Vector2(180, 80), true)   # press = capture
	driver.process_mouse(Vector2(500, 80), true)   # drag past the end
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["GAMMA", "scroll", 100, "100"])
	driver.process_mouse(Vector2(500, 80), false)  # release ends the drag
	driver.process_mouse(Vector2(180, 80), true)
	driver.process_mouse(Vector2(180, 80), false)


const SLIDER_AND_BUTTON_XML := """
<SCREEN>
  <NAME>OPTIONS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="scroll" name="GAMMA">
      <POSITION><LEFT>100</LEFT><TOP>72</TOP><RIGHT>300</RIGHT><BOTTOM>92</BOTTOM></POSITION>
      <ORIENTATION>HORIZONTAL</ORIENTATION>
      <APPEARANCE type="color" state="default">303030</APPEARANCE>
      <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
      <SCROLLUP type="color" state="default">505050</SCROLLUP>
      <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
    </WINDOW>
    <WINDOW type="button" name="APPLY">
      <POSITION><LEFT>100</LEFT><TOP>200</TOP><RIGHT>300</RIGHT><BOTTOM>230</BOTTOM></POSITION>
      <STRING>APPLY</STRING>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


func test_scroll_press_never_ghost_clicks_another_widget() -> void:
	# A press that lands on a scrollbar part is captured by that part until
	# release [orig: CScrollWnd_HandleEvent @ 0x64d050 capture] — drifting
	# onto a neighboring button while held must not press or click it, and
	# the arrow must not auto-repeat on the held samples.
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(_doc(SLIDER_AND_BUTTON_XML), null, null,
			null, "options.mnu"), "document opens on the frame")
	var gamma := driver.widget_id("GAMMA")
	driver.set_widget_scroll_range(gamma, 0, 100, 10, 50)
	watch_signals(driver)
	driver.process_mouse(Vector2(290, 80), true)   # press the right arrow
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["GAMMA", "scroll", 51, "51"])
	driver.process_mouse(Vector2(200, 215), true)  # drift onto APPLY, held
	driver.process_mouse(Vector2(200, 215), true)  # further held samples
	driver.process_mouse(Vector2(200, 215), false) # release over APPLY
	assert_signal_not_emitted(driver, "widget_activated",
			"the scroll-captured press never activates the button")
	assert_signal_emit_count(driver, "widget_value_changed", 1,
			"the arrow steps once — no auto-repeat, no ghost press")


func test_combo_popup_row_hover_tracks_mouse() -> void:
	# The popup-exclusive pump hovers the row under the mouse (row style 2)
	# [orig: CUIScene_EndFrame @ 0x63e600 gate @ 0x63e691; CListWnd_DrawItems
	# @ 0x643f30 mouseover row style].
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(_doc(BOARD_XML), null, null, null,
			"menu.mnu"), "document opens on the frame")
	var combo := driver.widget_id("MODE")
	_click_widget(driver, "MODE")
	assert_true(driver.is_combo_popup_open(combo), "the popup is open")
	var index := frame.widget_index("MODE")
	driver.process_mouse(Vector2(250, 45), false)  # row 1
	assert_eq(frame.get_widget_hover_item(index), 1,
			"moving over popup row 1 hovers it")
	driver.process_mouse(Vector2(250, 25), false)  # row 0
	assert_eq(frame.get_widget_hover_item(index), 0,
			"moving to row 0 moves the hover")
	driver.process_mouse(Vector2(600, 500), false)  # outside the popup
	assert_eq(frame.get_widget_hover_item(index), -1,
			"outside the popup no row hovers")
	_click(driver, Vector2(600, 500))  # dismiss
	assert_eq(frame.get_widget_hover_item(index), -1,
			"the dismissed popup leaves no hover row")


const COMBO_SCROLL_XML := """
<SCREEN>
  <NAME>ARMORY</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="combobox" name="SCROLLY">
      <POSITION><LEFT>200</LEFT><TOP>0</TOP><RIGHT>300</RIGHT><BOTTOM>20</BOTTOM></POSITION>
      <ITEMS>
        <ITEM value="0">ZERO</ITEM><ITEM value="1">ONE</ITEM>
        <ITEM value="2">TWO</ITEM><ITEM value="3">THREE</ITEM>
        <ITEM value="4">FOUR</ITEM><ITEM value="5">FIVE</ITEM>
      </ITEMS>
      <LIST_BOX>
        <POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>100</RIGHT><BOTTOM>100</BOTTOM></POSITION>
        <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
        <SCROLLBAR>
          <POSITION><LEFT>80</LEFT><TOP>0</TOP><RIGHT>100</RIGHT><BOTTOM>80</BOTTOM></POSITION>
          <APPEARANCE type="color" state="default">303030</APPEARANCE>
          <SHUTTLE type="color" state="default">80FF0000</SHUTTLE>
          <SCROLLUP type="color" state="default">505050</SCROLLUP>
          <SCROLLDOWN type="color" state="default">505050</SCROLLDOWN>
        </SCROLLBAR>
      </LIST_BOX>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


func test_combo_popup_scrollbar_scrolls_rows() -> void:
	# The open dropdown's scrollbar child sees the sample ahead of row picking
	# [orig: CListWnd child walk @ 0x643f30; CScrollWnd_HandleEvent
	# @ 0x64d050]: arrows step the row window, the press neither picks a row
	# nor dismisses, and the scrolled top row picks its absolute index.
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(_doc(COMBO_SCROLL_XML), null, null, null,
			"weapon.mnu"), "document opens on the frame")
	var combo := driver.widget_id("SCROLLY")
	var index := frame.widget_index("SCROLLY")
	_click(driver, Vector2(250, 10))
	assert_true(driver.is_combo_popup_open(combo), "the popup is open")
	var selected_before := driver.selected_row(combo)
	# Popup (200,20)-(300,100), scrollbar (280,20)-(300,100): down arrow at
	# the bottom 20px.
	driver.process_mouse(Vector2(290, 90), true)
	assert_true(driver.is_combo_popup_open(combo),
			"the scrollbar press keeps the popup open")
	assert_eq(driver.selected_row(combo), selected_before,
			"the scrollbar press picks no row")
	assert_eq(frame.get_widget_hover_item(index), -1,
			"the claimed scrollbar clears the row hover")
	driver.process_mouse(Vector2(290, 90), false)
	assert_true(driver.is_combo_popup_open(combo),
			"release on the scrollbar keeps the popup open")
	# 6 rows, 4 visible: one down-arrow step made absolute row 1 the top
	# visible row — picking the top row proves the window moved.
	_click(driver, Vector2(240, 30))
	assert_eq(driver.selected_row(combo), 1,
			"the scrolled top row picks its absolute index")
	assert_false(driver.is_combo_popup_open(combo), "the row pick closes the popup")


func test_wheel_ticks_scroll_open_popup_rows() -> void:
	# D-MNU-18 deliberate divergence: one wheel notch = one row; the open
	# popup consumes the tick exclusively (retail ships no functioning menu
	# wheel scroll — the witness lives at the engine pump).
	var frame := MenuFrame.new()
	add_child_autofree(frame)
	frame.size = Vector2(800, 600)
	var driver := MenuDriver.new()
	driver.attach(frame, null)
	assert_true(driver.open_document(_doc(COMBO_SCROLL_XML), null, null, null,
			"weapon.mnu"), "document opens on the frame")
	var combo := driver.widget_id("SCROLLY")
	_click(driver, Vector2(250, 10))
	assert_true(driver.is_combo_popup_open(combo), "the popup is open")
	assert_true(driver.process_wheel(Vector2(240, 30), 1),
			"the wheel tick claims the open popup")
	assert_true(driver.is_combo_popup_open(combo),
			"the wheel keeps the popup open")
	# One down tick made absolute row 1 the top visible row.
	_click(driver, Vector2(240, 30))
	assert_eq(driver.selected_row(combo), 1,
			"the wheel-scrolled top row picks its absolute index")
	assert_false(driver.process_wheel(Vector2(240, 30), 1),
			"with the popup closed, a tick over nothing scrollable is unclaimed")


func test_combo_outside_click_dismisses_and_is_consumed() -> void:
	var driver := _framed_driver(BOARD_XML)
	var combo := driver.widget_id("MODE")
	_click_widget(driver, "MODE")
	assert_true(driver.is_combo_popup_open(combo), "popup open")
	watch_signals(driver)
	# A press outside the popup (over the OTHER button) dismisses; the
	# dismissing click is consumed [orig: CComboWnd_HandleEvent @ 0x65c190,
	# outside check @ 0x65c290 — D-MNU-11/12].
	_click_widget(driver, "OTHER")
	assert_false(driver.is_combo_popup_open(combo), "outside click dismissed the popup")
	assert_signal_not_emitted(driver, "widget_activated")
	# A later plain click DOES reach the button, proving only the dismissing
	# click was eaten.
	_click_widget(driver, "OTHER")
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("OTHER"), "OTHER"])


# --- (e) edit focus + typing ----------------------------------------------------


func test_edit_click_focus_type_and_enter_commit() -> void:
	var driver := _framed_driver(BOARD_XML)
	var edit := driver.widget_id("NAME_EDIT")
	_click_widget(driver, "NAME_EDIT")
	assert_eq(driver.get_focused_widget(), edit, "click focuses the edit")
	watch_signals(driver)
	assert_true(driver.handle_key_input(_key(KEY_A, "A".unicode_at(0))), "typing consumed")
	assert_eq(driver.get_widget_text(edit), "A", "the character landed")
	assert_signal_emitted_with_parameters(driver, "widget_value_changed",
			["NAME_EDIT", "edit", -1, "A"])
	# Enter commits and releases focus [orig: CEditWnd_HandleKeyEvent
	# @ 0x6623a0 — clears g_UIFocusWnd and fires the commit event].
	assert_true(driver.handle_key_input(_key(KEY_ENTER)), "Enter consumed")
	assert_eq(driver.get_focused_widget(), -1, "commit released focus")
	assert_eq(driver.get_widget_text(edit), "A", "committed text persists")


func test_edit_numeric_mode_rolls_back_out_of_range() -> void:
	var driver := _framed_driver(BOARD_XML)
	var edit := driver.widget_id("NUM_EDIT")
	_click_widget(driver, "NUM_EDIT")
	assert_eq(driver.get_focused_widget(), edit, "numeric edit focused")
	assert_true(driver.handle_key_input(_key(KEY_9, "9".unicode_at(0))))
	assert_true(driver.handle_key_input(_key(KEY_9, "9".unicode_at(0))))
	assert_eq(driver.get_widget_text(edit), "99", "in-range digits insert")
	assert_true(driver.handle_key_input(_key(KEY_9, "9".unicode_at(0))),
			"the rejected keystroke is still consumed by the focused edit")
	assert_eq(driver.get_widget_text(edit), "99",
			"an out-of-range result rolls the whole insert back (MAXVAL 99)")


# --- (f) checked toggling -------------------------------------------------------


func test_checkbox_click_toggles() -> void:
	var driver := _framed_driver(BOARD_XML)
	var chk := driver.widget_id("CHK")
	assert_false(driver.is_widget_checked(chk), "checkbox starts unchecked")
	_click_widget(driver, "CHK")
	assert_true(driver.is_widget_checked(chk), "click checks")
	_click_widget(driver, "CHK")
	assert_false(driver.is_widget_checked(chk), "second click unchecks")


func test_radio_group_exclusivity() -> void:
	var driver := _framed_driver(BOARD_XML)
	var r1 := driver.widget_id("R1")
	var r2 := driver.widget_id("R2")
	_click_widget(driver, "R1")
	assert_true(driver.is_widget_checked(r1), "clicked radio checks")
	_click_widget(driver, "R2")
	assert_true(driver.is_widget_checked(r2), "second radio checks")
	assert_false(driver.is_widget_checked(r1), "GROUP=1 sibling unchecks")


# --- (g) sound-trigger edges ----------------------------------------------------


func test_selected_sound_emits_sound_requested_on_activation() -> void:
	var driver := _framed_driver(BOARD_XML)
	watch_signals(driver)
	_click_widget(driver, "SND_BTN")
	assert_signal_emitted_with_parameters(driver, "widget_activated",
			[driver.widget_id("SND_BTN"), "SND_BTN"])
	# MenuAudio is null: play_widget_sound still emits the request seam.
	assert_signal_emitted_with_parameters(driver, "sound_requested",
			["bank.lwf", "CLICK_SET"])


const SOUND_XML := """
<SCREEN>
  <NAME>SND</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="button" name="BTN">
      <POSITION><LEFT>100</LEFT><TOP>100</TOP><RIGHT>300</RIGHT><BOTTOM>140</BOTTOM></POSITION>
      <SOUND state="mousein" trigger="OVER">bank.lwf</SOUND>
      <SOUND state="selected" trigger="CLICK">bank.lwf</SOUND>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


# DI-34: the pump's sound edges through the frame and the runtime, per window as
# the game's pump plays them (engine menu_sound.h's MenuSoundPump): OVER as the
# mouse comes onto BTN, nothing more while it stays or while the button is down,
# CLICK on the click, and OVER again on the next sample with the mouse still there.
func test_hover_sounds_follow_the_game_s_pump() -> void:
	var driver := _framed_driver(SOUND_XML)
	var heard: Array[String] = []
	driver.sound_requested.connect(func(_file: String, trigger: String) -> void: heard.append(trigger))
	var at := driver.widget_frame_rect(driver.widget_id("BTN")).get_center()
	driver.process_mouse(at, false)
	driver.process_mouse(at + Vector2(2, 0), false)
	assert_eq(heard, ["OVER"] as Array[String], "onto BTN: its MOUSEIN once")
	driver.process_mouse(at, true)
	driver.process_mouse(at, false)
	assert_eq(heard, ["OVER", "CLICK"] as Array[String], "held: nothing; let go over it: its SELECTED")
	driver.process_mouse(at + Vector2(1, 0), false)
	assert_eq(heard, ["OVER", "CLICK", "OVER"] as Array[String], "the next sample there: MOUSEIN again")
	driver.process_mouse(Vector2(700, 500), false)
	assert_eq(heard.size(), 3, "BTN has no MOUSEOUT row: leaving it plays nothing")


# --- (h) the end-of-round stat table --------------------------------------------

const STAT_XML := """
<SCREEN>
  <NAME>STATS</NAME>
  <WINDOW type="window" name="ROOT">
    <POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT><BOTTOM>600</BOTTOM></POSITION>
    <WINDOW type="table" name="RESULTLIST">
      <POSITION><LEFT>10</LEFT><TOP>10</TOP><RIGHT>610</RIGHT><BOTTOM>410</BOTTOM></POSITION>
      <MIN_ITEM_HEIGHT>20</MIN_ITEM_HEIGHT>
    </WINDOW>
  </WINDOW>
</SCREEN>
"""


# The stat RESULTLIST authors no COLUMN: fill_stat_results installs the feed's
# columns, adds the rows and sorts by the third column descending, as
# StatScreen_PopulateStatResultsList does [orig: StatScreen_PopulateStatResultsList @ 0x562240;
# CTableWnd_SortByColumn(2) @ 0x5626f9]. The column draw is pinned in the
# menu_frame_parity ctest.
func test_stat_results_fill_installs_columns_and_sorts() -> void:
	var driver := _framed_driver(STAT_XML, "stat.mnu")
	var table := driver.widget_id("RESULTLIST")
	assert_eq(driver.table_sort_column(table), -1, "an unfilled table is unsorted")
	var columns: Array[EndRoundColumn] = [EndRoundColumn.new(), EndRoundColumn.new(), EndRoundColumn.new()]
	var rows: Array[EndRoundRow] = [EndRoundRow.new(), EndRoundRow.new()]
	driver.fill_stat_results(table, columns, rows)
	assert_eq(driver.table_row_count(table), 2, "every feed row lands")
	assert_eq(driver.table_sort_column(table), 2, "sorted by the third column")
	var none: Array[EndRoundRow] = []
	driver.fill_stat_results(table, columns, none)
	assert_eq(driver.table_row_count(table), 0, "a refill clears the old rows")
