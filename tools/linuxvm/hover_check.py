#!/usr/bin/env python3
"""hover_check.py <pointer on Mon.png> <pointer on Sun.png> [<pointer on the dock.png> <pointer at the top.png>] - the calendar of workshop item 3599498791 enlarges the weekday
under the pointer (its hover zones are cursorEnter scripts, the engine's cursor hit test). With the pointer on the Monday row
the big label has to be the Monday one, with it on the Sunday row the Sunday one; the big label has about four times the
bright pixels of a small one. The optional pair checks the dock, which magnifies the icons next to the pointer (that one follows
the scripts' cursor world position): the icons 04 and 05 rise above the dock bar with the pointer on it and stay flat with the
pointer at the top. Prints PASS or FAIL lines like check_frame.py. Screenshots are 1920x1080, the crops scale."""
import sys

from PIL import Image


def zones(path):
    """bright pixels in the narrow column of the Monday label and of the Sunday label"""
    image = Image.open(path).convert("L")
    sx, sy = image.width / 1920, image.height / 1080
    pixels = image.load()

    def count(y0, y1):
        return sum(
            1
            for y in range(int(y0 * sy), int(y1 * sy))
            for x in range(int(668 * sx), int(736 * sx))
            if pixels[x, y] >= 200
        )

    return count(440, 468), count(610, 642)


def dock_rise(path):
    """bright pixels just above the dock bar over the icons 04 and 05"""
    image = Image.open(path).convert("L")
    sx, sy = image.width / 1920, image.height / 1080
    pixels = image.load()
    return sum(
        1
        for y in range(int(900 * sy), int(931 * sy))
        for x in range(int(860 * sx), int(1061 * sx))
        if pixels[x, y] >= 200
    )


def main():
    mon_zone, sun_zone = zones(sys.argv[1])[0], zones(sys.argv[2])[1]
    mon_other, sun_other = zones(sys.argv[1])[1], zones(sys.argv[2])[0]
    ok_mon = mon_zone > 120 and mon_zone > 2 * mon_other
    ok_sun = sun_zone > 120 and sun_zone > 2 * sun_other
    print(("PASS" if ok_mon else "FAIL"), f"weekday hover: pointer on Monday, big label bright pixels Monday/Sunday {mon_zone}/{mon_other}")
    print(("PASS" if ok_sun else "FAIL"), f"weekday hover: pointer on Sunday, big label bright pixels Sunday/Monday {sun_zone}/{sun_other}")
    ok_dock = True
    if len(sys.argv) >= 5:
        near, far = dock_rise(sys.argv[3]), dock_rise(sys.argv[4])
        ok_dock = near > 300 and far < 60
        print(("PASS" if ok_dock else "FAIL"), f"dock zoom: bright pixels above the bar with the pointer on the dock/at the top {near}/{far}")
    return 0 if ok_mon and ok_sun and ok_dock else 1


if __name__ == "__main__":
    sys.exit(main())
