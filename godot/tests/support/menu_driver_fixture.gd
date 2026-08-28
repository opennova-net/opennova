class_name MenuDriverFixture
extends RefCounted

## The frameless MenuDriver the menu companion seam tests open their
## documents on: the driver's state store carries the companion seam without
## a render surface (the documented headless-test contract). The opening
## test passes itself so the open is asserted in its own report.


static func driver_over(test: GutTest, doc: MnuDocument, menu_file: String,
		screen := "") -> MenuDriver:
	var driver := MenuDriver.new()
	test.assert_true(driver.open_document(doc, null, null, null, menu_file, screen),
			"the document opens on the driver")
	return driver
