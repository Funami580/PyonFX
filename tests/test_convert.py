import os
import sys
import pytest_check as check
from pyonfx import *

# Get ass path
dir_path = os.path.dirname(os.path.realpath(__file__))
path_ass = os.path.join(dir_path, "Ass", "in.ass")

# Extract infos from ass file
io = Ass(path_ass)
meta, styles, lines = io.get_data()

# Config
max_deviation = 3


def test_coloralpha():
    assert Convert.coloralpha(255) == "&HFF&"
    assert Convert.coloralpha("&HFF&") == 255

    assert Convert.coloralpha("&H0000FF&") == (255, 0, 0)
    assert Convert.coloralpha(255, 0, 0) == "&H0000FF&"

    assert Convert.coloralpha("&HFF00FF00") == (0, 255, 0, 255)
    assert Convert.coloralpha(0, 255, 0, 255) == "&HFF00FF00"


def test_text_to_shape():
    pass  # TODO: raise NotImplementedError
