# -*- coding: utf-8 -*-
# PyonFX: An easy way to do KFX and complex typesetting based on subtitle format ASS (Advanced Substation Alpha).
# Copyright (C) 2019 Antonio Strippoli (CoffeeStraw/YellowFlash)
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# PyonFX is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program. If not, see http://www.gnu.org/licenses/.
"""
This file contains the Font class definition, which has some functions
to help getting informations from a specific font
"""
import sys

if "sphinx" not in sys.modules:
    import python_ass.ass as ass
    import python_ass.ass.renderer
    from datetime import timedelta

    _context = python_ass.ass.renderer.Context()
    _renderer = _context.make_renderer()
    _renderer.set_fonts(fontconfig_config="\0")
    _track = _context.make_track()


SCALE_FACTOR = 64


class Font:
    @staticmethod
    def glyph_data(line, text):
        from . import Line

        if not isinstance(line, Line):
            raise TypeError("Expected Line object, got %s." % type(line))

        def as_ass_text(obj, input_text):
            alignment = fr"\an{obj.styleref.alignment}"
            fontname = fr"\fn{obj.styleref.fontname}"
            fontsize = fr"\fs{obj.styleref.fontsize}"
            bold = r"\b1" if obj.styleref.bold else r""
            italic = r"\i1" if obj.styleref.italic else r""
            underline = r"\u1" if obj.styleref.underline else r""
            strikeout = r"\s1" if obj.styleref.strikeout else r""
            scale_x = fr"\fscx{obj.styleref.scale_x}"
            scale_y = fr"\fscy{obj.styleref.scale_y}"
            spacing = fr"\fsp{obj.styleref.spacing}"
            shadow = fr"\shad{obj.styleref.shadow}"
            outline = fr"\bord{obj.styleref.outline}"

            def remove_suffix(s, suffix):
                while s.endswith(suffix):
                    s = s[:-len(suffix)]
                return s

            input_text = remove_suffix(input_text, r"\N")
            input_text = remove_suffix(input_text, r"\n")

            return (fr"{{{alignment}{fontname}{fontsize}{bold}{italic}{underline}{strikeout}"
                    fr"{scale_x}{scale_y}{spacing}{shadow}{outline}}}{input_text}")

        meta = line.styleref.assref.meta
        doc = python_ass.ass.document.Document()

        doc.styles.append(python_ass.ass.document.Style(
            name="Default",
            primary_color=ass.data.Color.BLACK
        ))

        doc.events.append(python_ass.ass.document.Dialogue(
            start=timedelta(0),
            end=timedelta(milliseconds=1),
            style="Default",
            margin_l=line.margin_l if line.margin_l != 0 else line.styleref.margin_l,
            margin_r=line.margin_r if line.margin_r != 0 else line.styleref.margin_r,
            margin_v=line.margin_v if line.margin_v != 0 else line.styleref.margin_v,
            text=as_ass_text(line, text)
        ))

        if not (meta.play_res_x is not None and meta.play_res_y is not None
                and meta.play_res_x > 0 and meta.play_res_y > 0):
            raise Exception("Unknown resolution: cannot calculate positions")

        size = meta.play_res_x, meta.play_res_y
        wrap_style = 0
        scaled_border_and_shadow = "no"

        if hasattr(meta, "scaled_border_and_shadow") and meta.scaled_border_and_shadow:
            scaled_border_and_shadow = "yes"

        if hasattr(meta, "wrap_style"):
            wrap_style = meta.wrap_style

        doc.play_res_x, doc.play_res_y = size
        doc.scaled_border_and_shadow = scaled_border_and_shadow
        doc.wrap_style = wrap_style

        _renderer.set_all_sizes(size)
        _track.populate(doc)

        glyph_infos = _renderer.get_glyph_info(_track, timedelta(0))
        glyph_infos_size = glyph_infos.contents.size

        glyph_list = []

        for i in range(glyph_infos_size):
            glyph_list.append(glyph_infos[i])

        return glyph_list

    @staticmethod
    def get_metrics(line, text):
        if not text.strip():
            return 0.0, 0.0

        glyphs = Font.glyph_data(line, text)
        return Font.get_metrics_by_glyphs(glyphs)

    @staticmethod
    def get_metrics_by_glyphs(glyph_list):
        if not glyph_list:
            return 0.0, 0.0

        ascend, descend = 0, 0

        for i, glyph in enumerate(glyph_list):
            ascend = min(ascend, glyph.box_ymin)
            descend = max(descend, glyph.box_ymax)

        return abs(ascend) / SCALE_FACTOR, abs(descend) / SCALE_FACTOR

    @staticmethod
    def get_text_extents(line, text):
        if not text.strip():
            return 0.0, 0.0

        glyphs = Font.glyph_data(line, text + r"\h")
        width, height = 0, 0

        for i, glyph in enumerate(glyphs):
            if i == 0:
                width -= glyph.pos_x
            elif i == len(glyphs) - 1:
                width += glyph.pos_x

            height = max(height, abs(glyph.box_ymax - glyph.box_ymin) / SCALE_FACTOR)

        return width, height

    @staticmethod
    def text_to_shape(line, text):
        raise NotImplementedError
