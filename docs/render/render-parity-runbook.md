# Retired render-parity workflow

The registered-capture publication workflow described here was retired when
the repository removed its Python tooling and Python-only validation gates.
Historical render records may still link to this page or name the former tools;
those references document how the retained evidence was produced and are not
current commands.

The remaining PowerShell helpers under `scripts/render/` support local
OpenNova capture and image comparison. They do not mint or validate registered
retail evidence. Native renderer behavior is covered by CTest and the Godot GUT
suite.
