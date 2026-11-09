#!/usr/bin/env python3
"""Copies the texture animation probe scene (texanim/probe of the live comparison tests) with the detached animations
running backwards, for the coverage runs (coverage_testscenes.sh).

Usage: make_texanim_reverse.py <texanim probe dir> <out dir>"""
import json, os, shutil, sys

source, out = sys.argv[1:3]
shutil.rmtree(out, ignore_errors=True)
shutil.copytree(source, out)
path = os.path.join(out, "scene.json")
scene = json.load(open(path))
changed = 0

for item in scene["objects"]:
    visible = item.get("visible")
    if isinstance(visible, dict) and "script" in visible:
        visible["script"] = visible["script"].replace("t[4].rate = 2;", "t[4].rate = -2; t[5].rate = -0.5;")
        changed += 1

json.dump(scene, open(path, "w"), indent=1)
print("ok" if changed else "no script found")
