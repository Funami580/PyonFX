import ass
import ass.renderer
from datetime import timedelta

doc = ass.document.Document()

doc.styles.append(ass.document.Style(
    name="Default",
    primary_color=ass.data.Color.BLACK
))

doc.events.append(ass.document.Dialogue(
    start=timedelta(0),
    end=timedelta(milliseconds=1),
    style="Default",
    margin_l=25,
    margin_r=25,
    margin_v=25,
    text=r"{\an8\fnMigu 1P\b1\fs48\shad0\bord2}surechigau kotoba no ura ni tozasareta kokoro no kagi"
))

SIZE = (1280, 720)

doc.play_res_x, doc.play_res_y = SIZE
doc.scaled_border_and_shadow = "yes"
doc.wrap_style = 0

ctx = ass.renderer.Context()

r = ctx.make_renderer()
r.set_fonts(fontconfig_config="\0")
r.set_all_sizes(SIZE)

t = ctx.make_track()
t.populate(doc)

glyph_infos = r.get_glyph_info(t, timedelta(0))
glyph_infos_size = glyph_infos.contents.size

for i in range(glyph_infos_size):
    print(glyph_infos[i].pos_x, glyph_infos[i].pos_y)
