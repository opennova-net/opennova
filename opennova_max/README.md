# OpenNova 3ds Max addon

Imports NovaLogic `.3di` assets into 3ds Max using the same parsed 3DI3 model and
coordinate-space helpers as the Blender importer. The Max builder creates the
full scene categories expected by the Blender path: LOD 0 meshes, additional
LODs, center/attach/user markers, lights, collision helpers,
occlusion helpers, and skinned skeleton/weights where available.

Outputs are intentionally shared where the format is host-neutral:

- `.ase` is written through `pyopennova.ase_from_3di3`, which uses the same native
  `pyopennova.ase_ffi` writer instead of Max's native ASE exporter.
- `.3dp` and `.3da` are written through `pyopennova.project_writer`.
- `.max` is saved from the active 3ds Max scene next to those outputs by
  default.

## Install

Download `opennova_max-v<version>.mzp` from a release and drag it into 3ds Max.
The installer copies a versioned bundle into the user ApplicationPlugins
directory. Restart 3ds Max after install or update, then open the importer from
`OpenNova > Importer...`.

Build the MZP from source without 3ds Max:

```powershell
scripts\package_max_mzp.ps1
```

For source development with Max's bundled Python, install the package editable
and install the user startup hook:

```powershell
& "C:\Program Files\Autodesk\3ds Max 2022\Python37\python.exe" -m pip install -e .
powershell -ExecutionPolicy Bypass -File scripts\install_max_editable_startup.ps1
```

The editable startup hook calls `register_menu()` on launch and prints the same
loaded/failure diagnostics as the MZP startup path.

## Scene-only import

```python
python.execute "from opennova_max import import_loose; import_loose(r'C:\\path\\to\\Shed.3di')"
```

## Single output import

```python
python.execute "from opennova_max import run_loose_import; run_loose_import(r'C:\\assets\\Shed.3di', r'C:\\out\\Shed')"
```

Definition-backed imports still specify the game resource and output root:

```python
python.execute "from opennova_max import run_import; run_import(r'D:\\JO_ASSETS', 'WPN_M16', 'weapon', r'C:\\out')"
```

## Max UI

Open the importer from the main menu:

```text
OpenNova > Importer...
```

The UI is a resizable Qt dialog that uses the same PySide binding bundled with
3ds Max. The listener command opens the same dialog and is useful for
troubleshooting menu registration:

```python
python.execute "from opennova_max import show_importer; show_importer()"
```

The dialog supports loose `.3di`, selected DEF resources, and batch DEF imports.
It delegates to the same `run_loose_import()` and `run_import()` paths as
scripted usage, so ASE and 3DP/3DA outputs stay on the shared writers while the
native `.max` scene is saved from Max.

Register the launcher macro/menu manually if startup did not run:

```python
python.execute "from opennova_max import register_menu; register_menu()"
```

## Batch output import

```python
python.execute "from opennova_max import MaxImportRequest, run_batch; run_batch([MaxImportRequest(resource=r'C:\\assets\\Shed.3di', output_dir=r'C:\\out\\Shed')])"
```

`run_batch()` is sequential and runs in the current Max process. CI should not
expect `3dsmaxbatch.exe`.

## Local validation

The always-on Python suite validates the batch listener logic. Run an
interactive Max import manually when you need to validate a local Autodesk
install:

```powershell
3dsmaxbatch.exe path\to\validation_script.py -v 3
```
