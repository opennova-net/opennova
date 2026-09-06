# Original world preview example

This native data tree is the source for `godot/examples/world_preview.tscn`.
It contains no retail art.

- Tmap.cpt and its indexed PCX maps come from the synthetic
  `fixtures/terrain/tmap` terrain. Tmap.trn uses simplified materials and
  omits the fixture's external foliage definitions.
- crate.3di and house.3di come from `fixtures/threedi/synth`, authored by
  `tests/fixtures/minimal_3di_gen.cpp` through the native writer.
- Small mnml terrain texture tiles come from the project's authored minimal
  data. Other TGA files are original solid-color swatches.
- mnml.env uses the minimal environment with external sky/model references removed.
- items.def and preview.bms are authored for this example. Rebuild mission
  placements with Godot:
  `--headless --path godot --script res://tests/tools/world_preview_author.gd`.

This demonstrates preview and source ownership, not a complete playable
installation. No retail files or ignored local staging are needed.
