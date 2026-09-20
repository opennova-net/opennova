class_name MenuDriverFixture
extends RefCounted

## The frameless MenuDriver the menu companion seam tests open their
## documents on: the driver's state store carries the companion seam without
## a render surface (the documented headless-test contract). The opening
## test passes itself so the open is asserted in its own report. The markup
## helpers author the synthetic .mnu documents those tests open.


## Parse synthetic .mnu XML into a document, asserted on the test.
static func doc_from_xml(test: GutTest, xml: String) -> MnuDocument:
	var doc := MnuDocument.new()
	test.assert_eq(doc.load_from_bytes(xml.to_utf8_buffer()), OK,
			"the synthetic .mnu XML parses")
	return doc


## One named control window of `type`, 20 design pixels tall at `top`; `inner`
## is nested markup, `attrs` extra attributes on the WINDOW tag.
static func wnd(type: String, name: String, top: int, inner := "", attrs := "") -> String:
	return ('<WINDOW type="%s" name="%s"%s><POSITION><LEFT>10</LEFT><TOP>%d</TOP>'
			+ '<RIGHT>250</RIGHT><BOTTOM>%d</BOTTOM></POSITION>%s</WINDOW>') % [
			type, name, attrs, top, top + 20, inner]


## One SCREEN holding a full 800x600 MAIN window around `body`.
static func screen_xml(screen_name: String, body: String) -> String:
	return ('<SCREEN><NAME>%s</NAME><WINDOW type="window" name="MAIN">'
			+ '<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT>'
			+ '<BOTTOM>600</BOTTOM></POSITION>%s</WINDOW></SCREEN>') % [
			screen_name, body]


static func driver_over(test: GutTest, doc: MnuDocument, menu_file: String,
		screen := "") -> MenuDriver:
	var driver := MenuDriver.new()
	test.assert_true(driver.open_document(doc, null, null, null, menu_file, screen),
			"the document opens on the driver")
	return driver
