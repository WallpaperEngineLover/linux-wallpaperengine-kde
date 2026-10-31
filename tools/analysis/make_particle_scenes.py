#!/usr/bin/env python3
"""make_particle_scenes.py <out dir>: one small scene per particle initializer, operator and emitter that no installed
workshop item uses (found with a coverage build), each a single particle system on a grey background, for
tools/analysis/coverage_testscenes.sh. Parameters are the keys Data/Parsers/ObjectParser.cpp reads, missing ones take
the parser's defaults. Usage: make_particle_scenes.py ~/.local/share/we_live/tests/particles"""
import json
import os
import sys

BASE_INITIALIZERS = [
    {"name": "lifetimerandom", "min": 2, "max": 3},
    {"name": "sizerandom", "min": 30, "max": 50},
    {"name": "velocityrandom", "min": "-60 -60 0", "max": "60 60 0"},
    {"name": "colorrandom", "min": "255 255 255", "max": "255 200 100"},
]
BASE_OPERATORS = [{"name": "movement", "gravity": "0 -20 0"}, {"name": "alphafade", "fadeintime": 0.1}]
BASE_EMITTER = {"name": "sphererandom", "rate": 40, "origin": "0 0 0", "distancemin": 10, "distancemax": 120}
CONTROL_POINTS = [{"id": 0, "flags": 0, "offset": "0 0 0"}, {"id": 1, "flags": 0, "offset": "200 100 0"}]

INITIALIZERS = {
    "hsvcolorrandom": {"huemin": 0, "huemax": 1, "saturationmin": 0.5, "saturationmax": 1, "valuemin": 0.5, "valuemax": 1,
                       "huesteps": 4},
    "colorlist": {"colors": ["1 0 0", "0 1 0", "0 0 1"], "huenoise": 0.1, "saturationnoise": 0.1, "valuenoise": 0.1},
    "turbulentvelocityrandom": {"speedmin": 20, "speedmax": 100, "scale": 1, "timescale": 1, "phasemin": 0,
                                "phasemax": 1, "offset": 0, "forward": "0 1 0", "right": "1 0 0"},
    "positionoffsetrandom": {"distance": 60, "scale": 1, "octaves": 2, "directions": "1 1 0", "timescale": 1},
    "alpharandom": {"min": 0.3, "max": 1},
    "rotationrandom": {"min": "0 0 0", "max": "0 0 3.14"},
    "angularvelocityrandom": {"min": "0 0 -2", "max": "0 0 2"},
    "mapsequencearoundcontrolpoint": {"controlpoint": 1, "count": 4, "speedmin": 1, "speedmax": 2, "axis": "0 0 1"},
    "mapsequencebetweencontrolpoints": {"count": 4, "limitbehavior": 0, "controlpoint": 1},
    "remapinitialvalue": {},
}
OPERATORS = {
    "angularmovement": {"drag": 0.1, "force": "0 0 5"},
    "sizechange": {"startvalue": 1, "endvalue": 0.2, "starttime": 0, "endtime": 1},
    "alphachange": {"startvalue": 1, "endvalue": 0, "starttime": 0, "endtime": 1},
    "colorchange": {"startvalue": "1 1 1", "endvalue": "1 0 0", "starttime": 0, "endtime": 1},
    "turbulence": {"scale": 0.01, "speedmin": 20, "speedmax": 80, "timescale": 1, "phasemin": 0, "phasemax": 1},
    "vortex_v2": {"controlpoint": 0, "axis": "0 0 1", "distanceinner": 50, "distanceouter": 300, "speedinner": 300,
                  "speedouter": 50, "centerforce": 5, "ringradius": 100, "ringwidth": 50},
    "controlpointattract": {"controlpoint": 1, "scale": 100, "threshold": 300, "deletethreshold": 5},
    "oscillatealpha": {"frequencymin": 1, "frequencymax": 3, "scalemin": 0.2, "scalemax": 1, "phasemin": 0, "phasemax": 3},
    "oscillatesize": {"frequencymin": 1, "frequencymax": 3, "scalemin": 0.5, "scalemax": 1.5, "phasemin": 0, "phasemax": 3},
    "oscillateposition": {"frequencymin": 1, "frequencymax": 3, "scalemin": 5, "scalemax": 30, "phasemin": 0,
                          "phasemax": 3, "mask": "1 1 0"},
    "capvelocity": {"maxspeed": 40},
    "boids": {"separationthreshold": 20, "neighborthreshold": 100, "maxspeed": 100, "separationfactor": 1,
              "alignmentfactor": 1, "cohesionfactor": 1},
    "remapvalue": {},
    "maintaindistancetocontrolpoint": {"controlpoint": 1, "distance": 100, "scale": 5},
    "maintaindistancebetweencontrolpoints": {"controlpoint": 0, "controlpoint2": 1, "distance": 100, "scale": 5},
    "reducemovementnearcontrolpoint": {"controlpoint": 1, "distance": 150, "scale": 0.5},
    "collisionsphere": {"controlpoint": 1, "radius": 80, "restitution": 0.8},
    "collisionbox": {"controlpoint": 1, "size": "100 100 100", "restitution": 0.8},
    "collisionbounds": {"controlpoint": 1, "size": "300 300 300", "restitution": 0.8},
    "collisionquad": {"controlpoint": 1, "size": "200 200", "restitution": 0.8},
    "drag": {"drag": 0.5},
}
EMITTERS = {
    "boxrandom": {"distancemin": "10 10 0", "distancemax": "150 100 0"},
    "sphererandom": {"distancemin": 0, "distancemax": 200, "directions": "1 1 0"},
    "randomposition": {},
}


def scene(objects):
    return {
        "version": 3,
        "camera": {"center": "0 0 -1", "eye": "0 0 0", "up": "0 1 0"},
        "general": {"orthogonalprojection": {"width": 1920, "height": 1080}, "clearcolor": "0.1 0.1 0.15",
                    "bloom": False, "cameraparallax": False},
        "objects": objects,
    }


def write(out, name, particle, extra=None):
    folder = os.path.join(out, name)
    os.makedirs(os.path.join(folder, "particles"), exist_ok=True)
    for filename, content in (extra or {}).items():
        json.dump(content, open(os.path.join(folder, "particles", filename), "w"), indent=1)
    json.dump({"file": "scene.json", "type": "scene", "title": "particle " + name, "general": {"properties": {}}},
              open(os.path.join(folder, "project.json"), "w"))
    json.dump(particle, open(os.path.join(folder, "particles", "test.json"), "w"), indent=1)
    background = {"id": 1, "name": "bg", "image": "models/util/solidlayer.json", "size": "1920 1080",
                  "origin": "960 540 0", "color": "0.3 0.3 0.3", "alpha": 1.0, "visible": True}
    system = {"id": 2, "name": "system", "particle": "particles/test.json", "origin": "960 540 0", "scale": "1 1 1",
              "angles": "0 0 0", "visible": True, "instanceoverride": None}
    json.dump(scene([background, system]), open(os.path.join(folder, "scene.json"), "w"), indent=1)


def particle(initializers=(), operators=(), emitter=None):
    return {
        "material": "materials/particle/halo.json",
        "maxcount": 200,
        "starttime": 2,
        "emitter": [emitter or BASE_EMITTER],
        "initializer": BASE_INITIALIZERS + [dict(name=name, **values) for name, values in initializers],
        "operator": BASE_OPERATORS + [dict(name=name, **values) for name, values in operators],
        "controlpoint": CONTROL_POINTS,
    }


REMAP_VALUES = ["lifetimefraction", "maxlifetime", "size", "opacity", "speed", "rotation", "angularspeed",
                "distancetocontrolpoint", "positionbetweentwocontrolpoints", "runtime", "timeofday",
                "particlesystemtime", "layertime", "color", "position", "velocity", "controlpoint",
                "deltatocontrolpoint", "directiontocontrolpoint", "layerorigin"]
EVENT_INPUTS = ["setcolor", "multiplycolor", "setopacity", "multiplyopacity", "setcoloropacity", "multiplycoloropacity",
                "setvelocity", "addvelocity", "setsize", "multiplysize", "setrotation", "addrotation",
                "setangularvelocity", "addangularvelocity"]
EVENT_CHILD_TYPES = ["eventfollow", "eventspawn", "eventdeath"]
REMAP_OPERATIONS = ["remap", "multiply", "add", "subtract"]
REMAP_COMPONENTS = ["all", "x", "y", "z", "sum", "average", "max", "min"]
REMAP_TRANSFORMS = ["none", "sine", "square", "saw", "triangle", "simplexnoise", "fbmnoise"]


def remap_entries(count):
    """a deterministic sweep over inputs, outputs, operations, components and transforms, a few per particle system"""
    for i in range(count):
        yield {
            "input": REMAP_VALUES[i % len(REMAP_VALUES)],
            "output": REMAP_VALUES[(i * 7 + 2) % len(REMAP_VALUES)],
            "operation": REMAP_OPERATIONS[i % len(REMAP_OPERATIONS)],
            "inputcomponent": REMAP_COMPONENTS[(i * 3) % len(REMAP_COMPONENTS)],
            "outputcomponent": REMAP_COMPONENTS[(i * 5 + 1) % len(REMAP_COMPONENTS)],
            "transformfunction": REMAP_TRANSFORMS[i % len(REMAP_TRANSFORMS)],
            "inputrangemin": 0,
            "inputrangemax": 1,
            "outputrangemin": 0.2,
            "outputrangemax": 1.5,
            "inputcontrolpoint0": 0,
            "inputcontrolpoint1": 1,
            "outputcontrolpoint0": 0,
            "outputcontrolpoint1": 1,
            "flags": 1 + (i % 3),
        }


def main():
    out = sys.argv[1]
    entries = list(remap_entries(160))
    for number in range(0, len(entries), 6):
        group = entries[number:number + 6]
        write(out, "remapvalue-%02d" % (number // 6), particle(operators=[("remapvalue", e) for e in group]))
        write(out, "remapinitial-%02d" % (number // 6), particle(initializers=[("remapinitialvalue", e) for e in group]))
    for mode in (1, 2, 3):
        write(out, "turbulent-audio-%d" % mode, particle(initializers=[("turbulentvelocityrandom", dict(
            INITIALIZERS["turbulentvelocityrandom"], audioprocessingmode=mode, audioprocessingbounds="0 1",
            audioprocessingexponent=1.5, audioprocessingfrequencystart=2, audioprocessingfrequencyend=30))]))

    for name, values in INITIALIZERS.items():
        write(out, "init-" + name, particle(initializers=[(name, values)]))
    for name, values in OPERATORS.items():
        write(out, "op-" + name, particle(operators=[(name, values)]))
    for name, values in EMITTERS.items():
        write(out, "emit-" + name, particle(emitter=dict(BASE_EMITTER, name=name, **values)))
    for number, mode in enumerate(EVENT_INPUTS):
        kind = EVENT_CHILD_TYPES[number % len(EVENT_CHILD_TYPES)]
        child = particle(initializers=[("inheritinitialvaluefromevent", {"input": mode})],
                         operators=[("inheritvaluefromevent", {"input": mode, "blendinstart": 0.1, "blendinend": 0.3,
                                                              "blendoutstart": 0.7, "blendoutend": 0.9})])
        child["maxcount"] = 60
        parent = particle()
        parent["children"] = [{"type": kind, "name": "particles/child.json", "maxcount": 20, "probability": 1.0,
                               "origin": "10 0 0", "scale": "1 1 1", "angles": "0 0 0.3"}]
        write(out, "event-%s-%s" % (kind, mode), parent, {"child.json": child})
    write(out, "all-operators", particle(operators=list(OPERATORS.items())))
    write(out, "all-initializers", particle(initializers=list(INITIALIZERS.items())))


if __name__ == "__main__":
    main()
