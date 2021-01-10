import os
import pytest_check as check
from pyonfx import *

# Get ass path used for tests
dir_path = os.path.dirname(os.path.realpath(__file__))
path_ass = os.path.join(dir_path, "Ass", "ass_core.ass")

# Extract infos from ass file
io = Ass(path_ass)
meta, styles, lines = io.get_data()


def test_meta_values():
    # Tests if all the meta values are taken correctly
    # check.equal(meta.wrap_style, 0)                     # -> not in this .ass, so let's comment this
    # check.equal(meta.scaled_border_and_shadow, True)  # -> not in this .ass, so let's comment this
    check.equal(meta.play_res_x, 1280)
    check.equal(meta.play_res_y, 720)
    # check.equal(meta.audio, "")                         # -> not in this .ass, so let's comment this
    check.equal(meta.video, "?dummy:23.976000:2250:1920:1080:11:135:226:c")


def test_line_values():
    # Comment recognition
    check.equal(lines[0].comment, True)
    check.equal(lines[1].comment, False)

    # Line fields
    check.equal(lines[0].layer, 42)
    check.equal(lines[1].layer, 0)

    check.equal(lines[0].style, "Default")
    check.equal(lines[1].style, "Normal")

    check.equal(lines[0].actor, "Test")
    check.equal(lines[1].actor, "")

    check.equal(lines[0].effect, "Test; Wow")
    check.equal(lines[1].effect, "")

    check.equal(lines[0].margin_l, 1)
    check.equal(lines[1].margin_l, 0)

    check.equal(lines[0].margin_r, 2)
    check.equal(lines[1].margin_r, 0)

    check.equal(lines[0].margin_v, 3)
    check.equal(lines[1].margin_v, 50)

    check.equal(lines[1].start_time, Convert.time("0:00:00.00"))
    check.equal(lines[1].end_time, Convert.time("0:00:09.99"))
    check.equal(
        lines[1].duration, Convert.time("0:00:09.99") - Convert.time("0:00:00.00")
    )

    check.equal(
        lines[11].raw_text,
        "{\\k56}{\\1c&HFFFFFF&}su{\\k13}re{\\k22}chi{\\k36}ga{\\k48}u {\\k25\\-Pyon}{\\k34}ko{\\k33}to{\\k50}ba {\\k15}no {\\k17}u{\\k34}ra {\\k46}ni{\\k33} {\\k28}to{\\k36}za{\\k65}sa{\\1c&HFFFFFF&\\k33\\1c&HFFFFFF&\\k30\\1c&HFFFFFF&}re{\\k51\\-FX}ta{\\k16} {\\k33}ko{\\k33}ko{\\k78}ro {\\k15}no {\\k24}ka{\\k95}gi",
    )
    check.equal(lines[11].text, "surechigau kotoba no ura ni tozasareta kokoro no kagi")

    # Normal style (no bold, italic and with a normal fs)
    check.equal(lines[1].width, 438.375)
    check.equal(lines[1].height, 26.15625)
    check.equal(lines[1].ascent, 25.8125)
    check.equal(lines[1].descent, 0.34375)
    check.equal(lines[1].max_ascent, 36.984375)
    check.equal(lines[1].max_descent, 11.015625)
    check.equal(lines[1].x, lines[1].center)
    check.equal(lines[1].y, lines[1].top)
    check.equal(lines[1].left, 420.8125)
    check.equal(lines[1].center, 640.0)
    check.equal(lines[1].right, 859.1875)
    check.equal(lines[1].top, 50.0)
    check.equal(lines[1].middle, 74.0)
    check.equal(lines[1].bottom, 98.0)

    # Bold style
    check.equal(lines[2].width, 463.125)
    check.equal(lines[2].height, 26.15625)

    # Italic style
    check.equal(lines[3].width, 438.375)
    check.equal(lines[3].height, 26.15625)

    # Bold-italic style
    check.equal(lines[4].width, 463.125)
    check.equal(lines[4].height, 26.15625)

    # Normal-spaced style
    check.equal(lines[5].width, 578.375)
    check.equal(lines[5].height, 26.15625)

    # Normal - fscx style
    check.equal(lines[6].width, 613.75)
    check.equal(lines[6].height, 26.15625)

    # Normal - fscy style
    check.equal(lines[7].width, 438.375)
    check.equal(lines[7].height, 36.609375)

    # Normal - Big FS
    check.equal(lines[8].width, 821.859375)
    check.equal(lines[8].height, 49.03125)

    # Normal - Big FS - Spaced
    check.equal(lines[9].width, 1101.859375)
    check.equal(lines[9].height, 49.03125)

    # Bold - Text with non latin characters (kanji)
    check.equal(lines[10].width, 310.5)
    check.equal(lines[10].height, 31.9375)

    # Bold - Text with some tags
    check.equal(lines[11].width, 943.6875)
    check.equal(lines[11].height, 33.71875)

    # Bold - Vertical Text
    check.equal(lines[12].width, 31.625)
    check.equal(lines[12].height, 279.0)
