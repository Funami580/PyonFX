import os
from pyonfx import *

# Get ass path
dir_path = os.path.dirname(os.path.realpath(__file__))
path_ass = os.path.join(dir_path, "Ass", "in.ass")

# Extract infos from ass file
io = Ass(path_ass)
meta, styles, lines = io.get_data()


def test_coloralpha():
    assert Convert.color_ass_alpha("&HFF&") == 255
    assert Convert.color_alpha_ass(255) == "&HFF&"

    assert Convert.color_ass_rgb("&H0000FF&") == (255, 0, 0)
    assert Convert.color_ass_rgb("&H550000FF") == (255, 0, 0)
    assert Convert.color_rgb_ass((255, 0, 0)) == "&H0000FF&"

    assert Convert.color_ass_rgb("&H0000FF&", as_str=True) == "#FF0000"
    assert Convert.color_ass_rgb("&H550000FF", as_str=True) == "#FF0000"
    assert Convert.color_rgb_ass("#FF0000") == "&H0000FF&"

    assert Convert.color_ass_rgba("&H00FF00&") == (0, 255, 0, 255)
    assert Convert.color_ass_rgba("&HFF00FF00") == (0, 255, 0, 255)
    assert Convert.color_rgba_ass((0, 255, 0, 255)) == "&HFF00FF00"

    assert Convert.color_ass_rgba("&H00FF00&", as_str=True) == "#00FF00FF"
    assert Convert.color_ass_rgba("&HFF00FF00", as_str=True) == "#00FF00FF"
    assert Convert.color_rgba_ass("#00FF00FF") == "&HFF00FF00"

    assert Convert.color_ass_hsv("&H0000FF&") == (0, 100, 100)
    assert Convert.color_ass_hsv("&H550000FF") == (0, 100, 100)
    assert Convert.color_hsv_ass((0, 100, 100)) == "&H0000FF&"


def test_text_to_shape():
    pass  # TODO: raise NotImplementedError
