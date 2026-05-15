# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_all

datas = []
binaries = [('pyopennova\\lib\\windows-x64\\opennova.dll', 'pyopennova\\lib\\windows-x64')]
hiddenimports = []

for pkg in ("bpy", "numpy", "PySide6"):
    tmp = collect_all(pkg)
    datas += tmp[0]
    binaries += tmp[1]
    hiddenimports += tmp[2]

# New host-agnostic packages must be discoverable.
hiddenimports += [
    "opennova_jobs",
    "opennova_qt_ui",
    "opennova_qt_ui.dialog",
    "opennova_qt_ui.backend",
    "opennova_qt_ui.filtering",
    "opennova_qt_ui.preferences",
    "opennova_blender",
    "opennova_blender.scene_builder",
    "opennova_blender.bpy_session",
    "opennova_blender.import_runner",
    "opennova_blender.resource_plan",
    "opennova_blender.exports",
    "opennova_blender.dispatcher",
    "opennova_blender.worker",
    "opennova_blender.backend",
    "pyopennova.scan",
]

a = Analysis(
    ['apps\\onimport\\__main__.py'],
    pathex=[],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='onimport-v0.1.39',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=True,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
