#!/usr/bin/env python3
"""Copies a scene that has one light (volshadow/point of the live comparison tests) into variants with the other light
kinds, for the coverage runs (coverage_testscenes.sh).

Usage: make_light_scenes.py <scene dir with a light> <out dir>"""
import copy, json, os, shutil, sys

source, out = sys.argv[1:3]
VARIANTS = {
    "tube": {"light": "ltube", "controlpoint": "6 9.5 1.5", "castshadow": False},
    "tube-volumetric": {"light": "ltube", "controlpoint": "-4 9.5 1.5", "castshadow": False, "castvolumetrics": True},
    "directional": {"light": "ldirectional", "castshadow": True, "castvolumetrics": False},
    "directional-plain": {"light": "ldirectional", "castshadow": False, "castvolumetrics": False},
    "spot-volumetric": {"light": "lspot", "castshadow": False, "castvolumetrics": True, "innercone": 20, "outercone": 40},
    "point-plain": {"light": "lpoint", "castshadow": False, "castvolumetrics": False},
}

scene = json.load(open(os.path.join(source, "scene.json")))

for name, changes in VARIANTS.items():
    folder = os.path.join(out, name)
    shutil.rmtree(folder, ignore_errors=True)
    shutil.copytree(source, folder)
    variant = copy.deepcopy(scene)

    for light in variant["objects"]:
        if "light" in light:
            light.update(changes)

    config = variant["general"].get("lightconfig")
    if config is not None:
        variant["general"]["lightconfig"] = {"tube": 2, "directional": 2, "directionalshadow": 1, "spot": 2, "point": 2}

    json.dump(variant, open(os.path.join(folder, "scene.json"), "w"), indent=1)
