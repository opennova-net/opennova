"""Export the person's clip table from work/on_person.blend:

    blender art/on_person/work/on_person.blend -b --python art/on_person/export.py

What Export Animations does on `on_person`, into export/ beside this file:
US01.adm (the table ITEMS.DEF's US01 names), US01_rst.bad and a
US01_s<slot>.bad per clip. Never tracked (build_clips.py says why).
"""
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_clips  # noqa: E402

a = build_clips.addon()
model = bpy.data.objects[build_clips.MODEL]
out = os.path.join(HERE, "export")
os.makedirs(out, exist_ok=True)
model.o3d.adm_path = os.path.join(out, build_clips.TABLE)
message, notes = a.animation.export_animations(bpy.context, model)
print("on_person:", message)
for note in notes:
    print("on_person: note:", note)
