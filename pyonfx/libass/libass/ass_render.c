/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>

#include "ass_outline.h"
#include "ass_render.h"
#include "ass_parse.h"
#include "ass_priv.h"
#include "ass_shaper.h"

#define MAX_GLYPHS_INITIAL 1024
#define MAX_LINES_INITIAL 64
#define MAX_BITMAPS_INITIAL 16
#define MAX_SUB_BITMAPS_INITIAL 64
#define SUBPIXEL_MASK 63
#define STROKER_PRECISION 16     // stroker error in integer units, unrelated to final accuracy
#define RASTERIZER_PRECISION 16  // rasterizer spline approximation error in 1/64 pixel units
#define POSITION_PRECISION 8.0   // rough estimate of transform error in 1/64 pixel units
#define MAX_PERSP_SCALE 16.0
#define SUBPIXEL_ORDER 3  // ~ log2(64 / POSITION_PRECISION)
#define BLUR_PRECISION (1.0 / 256)  // blur error as fraction of full input range


ASS_Renderer *ass_renderer_init(ASS_Library *library)
{
    int error;
    FT_Library ft;
    ASS_Renderer *priv = 0;
    int vmajor, vminor, vpatch;

    error = FT_Init_FreeType(&ft);
    if (error) {
        ass_msg(library, MSGL_FATAL, "%s failed", "FT_Init_FreeType");
        goto fail;
    }

    FT_Library_Version(ft, &vmajor, &vminor, &vpatch);
    ass_msg(library, MSGL_V, "Raster: FreeType %d.%d.%d",
           vmajor, vminor, vpatch);

    priv = calloc(1, sizeof(ASS_Renderer));
    if (!priv) {
        FT_Done_FreeType(ft);
        goto fail;
    }

    priv->library = library;
    priv->ftlibrary = ft;
    // images_root and related stuff is zero-filled in calloc

#if (defined(__i386__) || defined(__x86_64__)) && CONFIG_ASM
    if (has_avx2())
        priv->engine = &ass_bitmap_engine_avx2;
    else if (has_sse2())
        priv->engine = &ass_bitmap_engine_sse2;
    else
        priv->engine = &ass_bitmap_engine_c;
#else
    priv->engine = &ass_bitmap_engine_c;
#endif

    if (!rasterizer_init(&priv->rasterizer, priv->engine->tile_order,
                         RASTERIZER_PRECISION))
        goto fail;

    priv->cache.font_cache = ass_font_cache_create();
    priv->cache.bitmap_cache = ass_bitmap_cache_create();
    priv->cache.composite_cache = ass_composite_cache_create();
    priv->cache.outline_cache = ass_outline_cache_create();
    if (!priv->cache.font_cache || !priv->cache.bitmap_cache || !priv->cache.composite_cache || !priv->cache.outline_cache)
        goto fail;

    priv->cache.glyph_max = GLYPH_CACHE_MAX;
    priv->cache.bitmap_max_size = BITMAP_CACHE_MAX_SIZE;
    priv->cache.composite_max_size = COMPOSITE_CACHE_MAX_SIZE;

    priv->text_info.max_bitmaps = MAX_BITMAPS_INITIAL;
    priv->text_info.max_glyphs = MAX_GLYPHS_INITIAL;
    priv->text_info.max_lines = MAX_LINES_INITIAL;
    priv->text_info.n_bitmaps = 0;
    priv->text_info.combined_bitmaps = calloc(MAX_BITMAPS_INITIAL, sizeof(CombinedBitmapInfo));
    priv->text_info.glyphs = calloc(MAX_GLYPHS_INITIAL, sizeof(GlyphInfo));
    priv->text_info.lines = calloc(MAX_LINES_INITIAL, sizeof(LineInfo));
    if (!priv->text_info.combined_bitmaps || !priv->text_info.glyphs || !priv->text_info.lines)
        goto fail;

    priv->settings.font_size_coeff = 1.;
    priv->settings.selective_style_overrides = ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE;

    if (!(priv->shaper = ass_shaper_new()))
        goto fail;

    ass_shaper_info(library);
    priv->settings.shaper = ASS_SHAPING_COMPLEX;

    ass_msg(library, MSGL_V, "Initialized");

    return priv;

fail:
    ass_msg(library, MSGL_ERR, "Initialization failed");
    ass_renderer_done(priv);

    return NULL;
}

void ass_renderer_done(ASS_Renderer *render_priv)
{
    if (!render_priv)
        return;

    ass_frame_unref(render_priv->images_root);
    ass_frame_unref(render_priv->prev_images_root);

    ass_cache_done(render_priv->cache.composite_cache);
    ass_cache_done(render_priv->cache.bitmap_cache);
    ass_cache_done(render_priv->cache.outline_cache);
    ass_shaper_free(render_priv->shaper);
    ass_cache_done(render_priv->cache.font_cache);

    rasterizer_done(&render_priv->rasterizer);

    if (render_priv->fontselect)
        ass_fontselect_free(render_priv->fontselect);
    if (render_priv->ftlibrary)
        FT_Done_FreeType(render_priv->ftlibrary);
    free(render_priv->eimg);
    free(render_priv->text_info.glyphs);
    free(render_priv->text_info.lines);

    free(render_priv->text_info.combined_bitmaps);

    free(render_priv->settings.default_font);
    free(render_priv->settings.default_family);

    free(render_priv->user_override_style.FontName);

    free(render_priv);
}

/**
 * \brief Mapping between script and screen coordinates
 */
static double x2scr_pos(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->orig_width / render_priv->font_scale_x / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
static double x2scr_left(ASS_Renderer *render_priv, double x)
{
    if (render_priv->state.explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->font_scale_x /
        render_priv->track->PlayResX;
}
static double x2scr_right(ASS_Renderer *render_priv, double x)
{
    if (render_priv->state.explicit || !render_priv->settings.use_margins)
        return x2scr_pos(render_priv, x);
    return x * render_priv->fit_width / render_priv->font_scale_x /
        render_priv->track->PlayResX +
        (render_priv->width - render_priv->fit_width);
}
static double x2scr_pos_scaled(ASS_Renderer *render_priv, double x)
{
    return x * render_priv->orig_width / render_priv->track->PlayResX +
        render_priv->settings.left_margin;
}
/**
 * \brief Mapping between script and screen coordinates
 */
static double y2scr_pos(ASS_Renderer *render_priv, double y)
{
    return y * render_priv->orig_height / render_priv->track->PlayResY +
        render_priv->settings.top_margin;
}
static double y2scr(ASS_Renderer *render_priv, double y)
{
    if (render_priv->state.explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height) * 0.5;
}

// the same for toptitles
static double y2scr_top(ASS_Renderer *render_priv, double y)
{
    if (render_priv->state.explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY;
}
// the same for subtitles
static double y2scr_sub(ASS_Renderer *render_priv, double y)
{
    if (render_priv->state.explicit || !render_priv->settings.use_margins)
        return y2scr_pos(render_priv, y);
    return y * render_priv->fit_height /
        render_priv->track->PlayResY +
        (render_priv->height - render_priv->fit_height);
}

static bool quantize_transform(double m[3][3], ASS_Vector *pos,
                               ASS_DVector *offset, bool first,
                               BitmapHashKey *key)
{
    // Full transform:
    // x_out = (m_xx * x + m_xy * y + m_xz) / z,
    // y_out = (m_yx * x + m_yy * y + m_yz) / z,
    // z     =  m_zx * x + m_zy * y + m_zz.

    const double max_val = 1000000;

    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Change input coordinates' origin to (x0, y0),
    // after that transformation x:[-dx, dx], y:[-dy, dy],
    // max|x| = dx and max|y| = dy.
    for (int i = 0; i < 3; i++)
        m[i][2] += m[i][0] * x0 + m[i][1] * y0;

    if (m[2][2] <= 0)
        return false;

    double w = 1 / m[2][2];
    // Transformed center of bounding box
    double center[2] = { m[0][2] * w, m[1][2] * w };
    // Change output coordinates' origin to center,
    // m_xz and m_yz is skipped as it becomes 0 and no longer needed.
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            m[i][j] -= m[2][j] * center[i];

    double delta[2] = {0};
    if (!first) {
        delta[0] = offset->x;
        delta[1] = offset->y;
    }

    int32_t qr[2];  // quantized center position
    for (int i = 0; i < 2; i++) {
        center[i] /= 64 >> SUBPIXEL_ORDER;
        center[i] -= delta[i];
        if (!(fabs(center[i]) < max_val))
            return false;
        qr[i] = lrint(center[i]);
    }

    // Minimal bounding box z coordinate
    double z0 = m[2][2] - fabs(m[2][0]) * dx - fabs(m[2][1]) * dy;
    // z0 clamped to z_center / MAX_PERSP_SCALE to mitigate problems with small z
    w = 1.0 / POSITION_PRECISION / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);
    double mul[2] = { dx * w, dy * w };  // 1 / q_x, 1 / q_y

    // z0 = m_zz - |m_zx| * dx - |m_zy| * dy,
    // m_zz = z0 + |m_zx| * dx + |m_zy| * dy,
    // z = m_zx * x + m_zy * y + m_zz
    //  = m_zx * (x + sign(m_zx) * dx) + m_zy * (y + sign(m_zy) * dy) + z0.

    // D(f)--absolute value of error in quantity f
    // as function of error in matrix coefficients, i. e. D(m_??).
    // Error in constant is zero, i. e. D(dx) = D(dy) = D(z0) = 0.
    // In the following calculation errors are considered small
    // and second- and higher-order terms are dropped.
    // That approximation is valid as long as glyph dimensions are larger than couple of pixels.
    // Therefore standard relations for derivatives can be used for D(?):
    // D(A * B) <= D(A) * max|B| + max|A| * D(B),
    // D(1 / C) <= D(C) * max|1 / C^2|.

    // D(x_out) = D((m_xx * x + m_xy * y) / z)
    //  <= D(m_xx * x + m_xy * y) * max|1 / z| + max|m_xx * x + m_xy * y| * D(1 / z)
    //  <= (D(m_xx) * dx + D(m_xy) * dy) / z0 + (|m_xx| * dx + |m_xy| * dy) * D(z) / z0^2,
    // D(y_out) = D((m_yx * x + m_yy * y) / z)
    //  <= D(m_yx * x + m_yy * y) * max|1 / z| + max|m_yx * x + m_yy * y| * D(1 / z)
    //  <= (D(m_yx) * dx + D(m_yy) * dy) / z0 + (|m_yx| * dx + |m_yy| * dy) * D(z) / z0^2,
    // |m_xx| * dx + |m_xy| * dy = x_lim,
    // |m_yx| * dx + |m_yy| * dy = y_lim,
    // D(z) <= 2 * (D(m_zx) * dx + D(m_zy) * dy),
    // D(x_out) <= (D(m_xx) * dx + D(m_xy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * x_lim / z0^2,
    // D(y_out) <= (D(m_yx) * dx + D(m_yy) * dy) / z0
    //       + 2 * (D(m_zx) * dx + D(m_zy) * dy) * y_lim / z0^2.

    // To estimate acceptable error in matrix coefficient
    // set error in all other coefficients to zero and solve system
    // D(x_out) <= ACCURACY & D(y_out) <= ACCURACY for desired D(m_??).
    // ACCURACY here is some part of total error, i. e. ACCURACY ~ POSITION_PRECISION.
    // Note that POSITION_PRECISION isn't total error, it's convenient constant.
    // True error can be up to several POSITION_PRECISION.

    // Quantization steps (ACCURACY ~ POSITION_PRECISION):
    // D(m_xx), D(m_yx) ~ q_x = POSITION_PRECISION * z0 / dx,
    // D(m_xy), D(m_yy) ~ q_y = POSITION_PRECISION * z0 / dy,
    // qm_xx = round(m_xx / q_x), qm_xy = round(m_xy / q_y),
    // qm_yx = round(m_yx / q_x), qm_yy = round(m_yy / q_y).

    int32_t qm[3][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            double val = m[i][j] * mul[j];
            if (!(fabs(val) < max_val))
                return false;
            qm[i][j] = lrint(val);
        }

    // x_lim = |m_xx| * dx + |m_xy| * dy
    //  ~= |qm_xx| * q_x * dx + |qm_xy| * q_y * dy
    //  = (|qm_xx| + |qm_xy|) * POSITION_PRECISION * z0,
    // y_lim = |m_yx| * dx + |m_yy| * dy
    //  ~= |qm_yx| * q_x * dx + |qm_yy| * q_y * dy
    //  = (|qm_yx| + |qm_yy|) * POSITION_PRECISION * z0,
    // max(x_lim, y_lim) / z0 ~= w
    //  = max(|qm_xx| + |qm_xy|, |qm_yx| + |qm_yy|) * POSITION_PRECISION.

    // Quantization steps (ACCURACY ~ 2 * POSITION_PRECISION):
    // D(m_zx) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dx ~= q_zx = q_x / w,
    // D(m_zy) ~ POSITION_PRECISION * z0^2 / max(x_lim, y_lim) / dy ~= q_zy = q_y / w,
    // qm_zx = round(m_zx / q_zx), qm_zy = round(m_zy / q_zy).

    int32_t qmx = abs(qm[0][0]) + abs(qm[0][1]);
    int32_t qmy = abs(qm[1][0]) + abs(qm[1][1]);
    w = POSITION_PRECISION * FFMAX(qmx, qmy);
    mul[0] *= w;
    mul[1] *= w;

    for (int j = 0; j < 2; j++) {
        double val = m[2][j] * mul[j];
        if (!(fabs(val) < max_val))
            return false;
        qm[2][j] = lrint(val);
    }

    if (first && offset) {
        offset->x = center[0] - qr[0];
        offset->y = center[1] - qr[1];
    }
    pos->x = qr[0] >> SUBPIXEL_ORDER;
    pos->y = qr[1] >> SUBPIXEL_ORDER;
    key->offset.x = qr[0] & ((1 << SUBPIXEL_ORDER) - 1);
    key->offset.y = qr[1] & ((1 << SUBPIXEL_ORDER) - 1);
    key->matrix_x.x = qm[0][0];  key->matrix_x.y = qm[0][1];
    key->matrix_y.x = qm[1][0];  key->matrix_y.y = qm[1][1];
    key->matrix_z.x = qm[2][0];  key->matrix_z.y = qm[2][1];
    return true;
}

static void restore_transform(double m[3][3], const BitmapHashKey *key)
{
    const ASS_Rect *bbox = &key->outline->cbox;
    double x0 = (bbox->x_min + bbox->x_max) / 2.0;
    double y0 = (bbox->y_min + bbox->y_max) / 2.0;
    double dx = (bbox->x_max - bbox->x_min) / 2.0 + 64;
    double dy = (bbox->y_max - bbox->y_min) / 2.0 + 64;

    // Arbitrary scale has chosen so that z0 = 1
    double q_x = POSITION_PRECISION / dx;
    double q_y = POSITION_PRECISION / dy;
    m[0][0] = key->matrix_x.x * q_x;
    m[0][1] = key->matrix_x.y * q_y;
    m[1][0] = key->matrix_y.x * q_x;
    m[1][1] = key->matrix_y.y * q_y;

    int32_t qmx = abs(key->matrix_x.x) + abs(key->matrix_x.y);
    int32_t qmy = abs(key->matrix_y.x) + abs(key->matrix_y.y);
    double scale_z = 1.0 / POSITION_PRECISION / FFMAX(qmx, qmy);
    m[2][0] = key->matrix_z.x * q_x * scale_z;  // qm_zx * q_zx
    m[2][1] = key->matrix_z.y * q_y * scale_z;  // qm_zy * q_zy

    m[0][2] = m[1][2] = 0;
    m[2][2] = 1 + fabs(m[2][0]) * dx + fabs(m[2][1]) * dy;
    m[2][2] = FFMIN(m[2][2], MAX_PERSP_SCALE);

    double center[2] = {
        key->offset.x * (64 >> SUBPIXEL_ORDER),
        key->offset.y * (64 >> SUBPIXEL_ORDER),
    };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] += m[2][j] * center[i];

    for (int i = 0; i < 3; i++)
        m[i][2] -= m[i][0] * x0 + m[i][1] * y0;
}

// Calculate bitmap memory footprint
static inline size_t bitmap_size(const Bitmap *bm)
{
    return bm->stride * bm->h;
}

static void compute_string_bbox(TextInfo *text, ASS_DRect *bbox)
{
    if (text->length > 0) {
        bbox->x_min = +32000;
        bbox->x_max = -32000;
        bbox->y_min = d6_to_double(text->glyphs[0].pos.y) - text->lines[0].asc;
        bbox->y_max = bbox->y_min + text->height;

        for (int i = 0; i < text->length; i++) {
            GlyphInfo *info = text->glyphs + i;
            if (info->skip) continue;
            double s = d6_to_double(info->pos.x);
            double e = s + d6_to_double(info->cluster_advance.x);
            bbox->x_min = FFMIN(bbox->x_min, s);
            bbox->x_max = FFMAX(bbox->x_max, e);
        }
    } else
        bbox->x_min = bbox->x_max = bbox->y_min = bbox->y_max = 0;
}

static ASS_Style *handle_selective_style_overrides(ASS_Renderer *render_priv,
                                                   ASS_Style *rstyle)
{
    // The script style is the one the event was declared with.
    ASS_Style *script = render_priv->track->styles +
                        render_priv->state.event->Style;
    // The user style was set with ass_set_selective_style_override().
    ASS_Style *user = &render_priv->user_override_style;
    ASS_Style *new = &render_priv->state.override_style_temp_storage;
    int explicit = render_priv->state.explicit;
    int requested = render_priv->settings.selective_style_overrides;
    double scale;

    user->Name = "OverrideStyle"; // name insignificant

    // Either the event's style, or the style forced with a \r tag.
    if (!rstyle)
        rstyle = script;

    // Create a new style that contains a mix of the original style and
    // user_style (the user's override style). Copy only fields from the
    // script's style that are deemed necessary.
    *new = *rstyle;

    render_priv->state.apply_font_scale =
        !explicit || !(requested & ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE);

    // On positioned events, do not apply most overrides.
    if (explicit)
        requested = 0;

    if (requested & ASS_OVERRIDE_BIT_STYLE)
        requested |= ASS_OVERRIDE_BIT_FONT_NAME |
                     ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS |
                     ASS_OVERRIDE_BIT_COLORS |
                     ASS_OVERRIDE_BIT_BORDER |
                     ASS_OVERRIDE_BIT_ATTRIBUTES;

    // Copies fields even not covered by any of the other bits.
    if (requested & ASS_OVERRIDE_FULL_STYLE)
        *new = *user;

    // The user style is supposed to be independent of the script resolution.
    // Treat the user style's values as if they were specified for a script with
    // PlayResY=288, and rescale the values to the current script.
    scale = render_priv->track->PlayResY / 288.0;

    if (requested & ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS) {
        new->FontSize = user->FontSize * scale;
        new->Spacing = user->Spacing * scale;
        new->ScaleX = user->ScaleX;
        new->ScaleY = user->ScaleY;
    }

    if (requested & ASS_OVERRIDE_BIT_FONT_NAME) {
        new->FontName = user->FontName;
        new->treat_fontname_as_pattern = user->treat_fontname_as_pattern;
    }

    if (requested & ASS_OVERRIDE_BIT_COLORS) {
        new->PrimaryColour = user->PrimaryColour;
        new->SecondaryColour = user->SecondaryColour;
        new->OutlineColour = user->OutlineColour;
        new->BackColour = user->BackColour;
    }

    if (requested & ASS_OVERRIDE_BIT_ATTRIBUTES) {
        new->Bold = user->Bold;
        new->Italic = user->Italic;
        new->Underline = user->Underline;
        new->StrikeOut = user->StrikeOut;
    }

    if (requested & ASS_OVERRIDE_BIT_BORDER) {
        new->BorderStyle = user->BorderStyle;
        new->Outline = user->Outline * scale;
        new->Shadow = user->Shadow * scale;
    }

    if (requested & ASS_OVERRIDE_BIT_ALIGNMENT)
        new->Alignment = user->Alignment;

    if (requested & ASS_OVERRIDE_BIT_JUSTIFY)
        new->Justify = user->Justify;

    if (requested & ASS_OVERRIDE_BIT_MARGINS) {
        new->MarginL = user->MarginL;
        new->MarginR = user->MarginR;
        new->MarginV = user->MarginV;
    }

    if (!new->FontName)
        new->FontName = rstyle->FontName;

    render_priv->state.style = new;
    render_priv->state.overrides = requested;

    return new;
}

static void init_font_scale(ASS_Renderer *render_priv)
{
    ASS_Settings *settings_priv = &render_priv->settings;

    double font_scr_h = render_priv->orig_height;
    if (!render_priv->state.explicit && render_priv->settings.use_margins)
        font_scr_h = render_priv->fit_height;

    render_priv->font_scale = font_scr_h / render_priv->track->PlayResY;
    if (settings_priv->storage_height)
        render_priv->blur_scale = font_scr_h / settings_priv->storage_height;
    else
        render_priv->blur_scale = font_scr_h / render_priv->track->PlayResY;
    if (render_priv->track->ScaledBorderAndShadow)
        render_priv->border_scale =
            font_scr_h / render_priv->track->PlayResY;
    else
        render_priv->border_scale = render_priv->blur_scale;

    if (render_priv->state.apply_font_scale) {
        render_priv->font_scale *= settings_priv->font_size_coeff;
        render_priv->border_scale *= settings_priv->font_size_coeff;
        render_priv->blur_scale *= settings_priv->font_size_coeff;
    }
}

/**
 * \brief partially reset render_context to style values
 * Works like {\r}: resets some style overrides
 */
void reset_render_context(ASS_Renderer *render_priv, ASS_Style *style)
{
    style = handle_selective_style_overrides(render_priv, style);

    init_font_scale(render_priv);

    render_priv->state.c[0] = style->PrimaryColour;
    render_priv->state.c[1] = style->SecondaryColour;
    render_priv->state.c[2] = style->OutlineColour;
    render_priv->state.c[3] = style->BackColour;
    render_priv->state.flags =
        (style->Underline ? DECO_UNDERLINE : 0) |
        (style->StrikeOut ? DECO_STRIKETHROUGH : 0);
    render_priv->state.font_size = style->FontSize;

    char* new_family = strdup(style->FontName);
    if (new_family) {
        free(render_priv->state.family);
        render_priv->state.family = new_family;
        render_priv->state.treat_family_as_pattern =
            style->treat_fontname_as_pattern;
    }
    render_priv->state.bold = style->Bold;
    render_priv->state.italic = style->Italic;
    update_font(render_priv);

    render_priv->state.border_style = style->BorderStyle;
    render_priv->state.border_x = style->Outline;
    render_priv->state.border_y = style->Outline;
    render_priv->state.scale_x = style->ScaleX;
    render_priv->state.scale_y = style->ScaleY;
    render_priv->state.hspacing = style->Spacing;
    render_priv->state.be = 0;
    render_priv->state.blur = style->Blur;
    render_priv->state.shadow_x = style->Shadow;
    render_priv->state.shadow_y = style->Shadow;
    render_priv->state.frx = render_priv->state.fry = 0.;
    render_priv->state.frz = style->Angle;
    render_priv->state.fax = render_priv->state.fay = 0.;
    render_priv->state.font_encoding = style->Encoding;
}

/**
 * \brief Start new event. Reset render_priv->state.
 */
static void
init_render_context(ASS_Renderer *render_priv, ASS_Event *event)
{
    render_priv->state.event = event;
    render_priv->state.parsed_tags = 0;
    render_priv->state.evt_type = EVENT_NORMAL;

    render_priv->state.wrap_style = render_priv->track->WrapStyle;

    render_priv->state.pos_x = 0;
    render_priv->state.pos_y = 0;
    render_priv->state.org_x = 0;
    render_priv->state.org_y = 0;
    render_priv->state.have_origin = 0;
    render_priv->state.clip_x0 = 0;
    render_priv->state.clip_y0 = 0;
    render_priv->state.clip_x1 = render_priv->track->PlayResX;
    render_priv->state.clip_y1 = render_priv->track->PlayResY;
    render_priv->state.clip_mode = 0;
    render_priv->state.detect_collisions = 1;
    render_priv->state.fade = 0;
    render_priv->state.drawing_scale = 0;
    render_priv->state.pbo = 0;
    render_priv->state.effect_type = EF_NONE;
    render_priv->state.effect_timing = 0;
    render_priv->state.effect_skip_timing = 0;

    apply_transition_effects(render_priv, event);
    render_priv->state.explicit = render_priv->state.evt_type != EVENT_NORMAL ||
                                  event_has_hard_overrides(event->Text);

    reset_render_context(render_priv, NULL);
    render_priv->state.alignment = render_priv->state.style->Alignment;
    render_priv->state.justify = render_priv->state.style->Justify;
}

static void free_render_context(ASS_Renderer *render_priv)
{
    ass_cache_dec_ref(render_priv->state.font);
    free(render_priv->state.family);
    free(render_priv->state.clip_drawing_text);

    render_priv->state.font = NULL;
    render_priv->state.family = NULL;
    render_priv->state.clip_drawing_text = NULL;

    TextInfo *text_info = &render_priv->text_info;
    for (int n = 0; n < text_info->length; n++)
        free(text_info->glyphs[n].drawing_text);
    text_info->length = 0;
}

/**
 * \brief Get normal and outline (border) glyphs
 * \param info out: struct filled with extracted data
 * Tries to get both glyphs from cache.
 * If they can't be found, gets a glyph from font face, generates outline,
 * and add them to cache.
 */
static void
get_outline_glyph(ASS_Renderer *priv, GlyphInfo *info)
{
    OutlineHashValue *val;
    ASS_DVector scale, offset = {0};

    int32_t asc, desc;
    OutlineHashKey key;
    if (info->drawing_text) {
        key.type = OUTLINE_DRAWING;
        key.u.drawing.text = info->drawing_text;
        val = ass_cache_get(priv->cache.outline_cache, &key, priv);
        if (!val || !val->valid) {
            ass_cache_dec_ref(val);
            return;
        }

        double w = priv->font_scale / (1 << (info->drawing_scale - 1));
        scale.x = info->scale_x * w;
        scale.y = info->scale_y * w;
        desc = 64 * info->drawing_pbo;
        asc = val->asc - desc;

        offset.y = -asc * scale.y;
    } else {
        key.type = OUTLINE_GLYPH;
        GlyphHashKey *k = &key.u.glyph;
        k->font = info->font;
        k->size = info->font_size;
        k->face_index = info->face_index;
        k->glyph_index = info->glyph_index;
        k->bold = info->bold;
        k->italic = info->italic;
        k->flags = info->flags;

        val = ass_cache_get(priv->cache.outline_cache, &key, priv);
        if (!val || !val->valid) {
            ass_cache_dec_ref(val);
            return;
        }

        scale.x = info->scale_x;
        scale.y = info->scale_y;
        asc  = val->asc;
        desc = val->desc;
    }

    info->outline = val;
    info->transform.scale = scale;
    info->transform.offset = offset;

    info->bbox.x_min = lrint(val->cbox.x_min * scale.x + offset.x);
    info->bbox.y_min = lrint(val->cbox.y_min * scale.y + offset.y);
    info->bbox.x_max = lrint(val->cbox.x_max * scale.x + offset.x);
    info->bbox.y_max = lrint(val->cbox.y_max * scale.y + offset.y);

    if (info->drawing_text || priv->settings.shaper == ASS_SHAPING_SIMPLE) {
        info->cluster_advance.x = info->advance.x = lrint(val->advance * scale.x);
        info->cluster_advance.y = info->advance.y = 0;
    }
    info->asc  = lrint(asc  * scale.y);
    info->desc = lrint(desc * scale.y);
}

size_t ass_outline_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    OutlineHashKey *outline_key = key;
    OutlineHashValue *v = value;
    memset(v, 0, sizeof(*v));

    switch (outline_key->type) {
    case OUTLINE_GLYPH:
        {
            GlyphHashKey *k = &outline_key->u.glyph;
            ass_face_set_size(k->font->faces[k->face_index], k->size);
            FT_Glyph glyph =
                ass_font_get_glyph(k->font, k->face_index, k->glyph_index,
                                   render_priv->settings.hinting, k->flags);
            if (glyph != NULL) {
                FT_Outline *src = &((FT_OutlineGlyph) glyph)->outline;
                if (!outline_convert(&v->outline[0], src))
                    return 1;
                v->advance = d16_to_d6(glyph->advance.x);
                FT_Done_Glyph(glyph);
                ass_font_get_asc_desc(k->font, k->face_index,
                                      &v->asc, &v->desc);
            }
            break;
        }
    case OUTLINE_DRAWING:
        {
            ASS_Rect bbox;
            const char *text = outline_key->u.drawing.text;
            if (!ass_drawing_parse(&v->outline[0], &bbox, text, render_priv->library))
                return 1;

            v->advance = bbox.x_max - bbox.x_min;
            v->asc = bbox.y_max - bbox.y_min;
            v->desc = 0;
            break;
        }
    case OUTLINE_BORDER:
        {
            BorderHashKey *k = &outline_key->u.border;
            if (!k->border.x && !k->border.y)
                break;
            if (!k->outline->outline[0].n_points)
                break;

            ASS_Outline src;
            if (!outline_scale_pow2(&src, &k->outline->outline[0],
                                    k->scale_ord_x, k->scale_ord_y))
                return 1;
            if (!outline_stroke(&v->outline[0], &v->outline[1], &src,
                                k->border.x * STROKER_PRECISION,
                                k->border.y * STROKER_PRECISION,
                                STROKER_PRECISION)) {
                ass_msg(render_priv->library, MSGL_WARN, "Cannot stroke outline");
                outline_free(&v->outline[0]);
                outline_free(&v->outline[1]);
                outline_free(&src);
                return 1;
            }
            outline_free(&src);
            break;
        }
    case OUTLINE_BOX:
        {
            ASS_Outline *ol = &v->outline[0];
            if (!outline_alloc(ol, 4, 4))
                return 1;
            ol->points[0].x = ol->points[3].x = 0;
            ol->points[1].x = ol->points[2].x = 64;
            ol->points[0].y = ol->points[1].y = 0;
            ol->points[2].y = ol->points[3].y = 64;
            ol->segments[0] = OUTLINE_LINE_SEGMENT;
            ol->segments[1] = OUTLINE_LINE_SEGMENT;
            ol->segments[2] = OUTLINE_LINE_SEGMENT;
            ol->segments[3] = OUTLINE_LINE_SEGMENT | OUTLINE_CONTOUR_END;
            ol->n_points = ol->n_segments = 4;
            break;
        }
    default:
        return 1;
    }

    rectangle_reset(&v->cbox);
    outline_update_cbox(&v->outline[0], &v->cbox);
    outline_update_cbox(&v->outline[1], &v->cbox);
    if (v->cbox.x_min > v->cbox.x_max || v->cbox.y_min > v->cbox.y_max)
        v->cbox.x_min = v->cbox.y_min = v->cbox.x_max = v->cbox.y_max = 0;
    v->valid = true;
    return 1;
}

/**
 * \brief Calculate outline transformation matrix
 */
static void calc_transform_matrix(ASS_Renderer *render_priv,
                                  GlyphInfo *info, double m[3][3])
{
    double frx = M_PI / 180 * info->frx;
    double fry = M_PI / 180 * info->fry;
    double frz = M_PI / 180 * info->frz;

    double sx = -sin(frx), cx = cos(frx);
    double sy =  sin(fry), cy = cos(fry);
    double sz = -sin(frz), cz = cos(frz);

    double fax = info->fax * info->scale_x / info->scale_y;
    double fay = info->fay * info->scale_y / info->scale_x;
    double x1[3] = { 1, fax, info->shift.x + info->asc * fax };
    double y1[3] = { fay, 1, info->shift.y };

    double x2[3], y2[3];
    for (int i = 0; i < 3; i++) {
        x2[i] = x1[i] * cz - y1[i] * sz;
        y2[i] = x1[i] * sz + y1[i] * cz;
    }

    double y3[3], z3[3];
    for (int i = 0; i < 3; i++) {
        y3[i] = y2[i] * cx;
        z3[i] = y2[i] * sx;
    }

    double x4[3], z4[3];
    for (int i = 0; i < 3; i++) {
        x4[i] = x2[i] * cy - z3[i] * sy;
        z4[i] = x2[i] * sy + z3[i] * cy;
    }

    double dist = 20000 * render_priv->blur_scale;
    z4[2] += dist;

    double scale_x = dist * render_priv->font_scale_x;
    double offs_x = info->pos.x - info->shift.x * render_priv->font_scale_x;
    double offs_y = info->pos.y - info->shift.y;
    for (int i = 0; i < 3; i++) {
        m[0][i] = z4[i] * offs_x + x4[i] * scale_x;
        m[1][i] = z4[i] * offs_y + y3[i] * dist;
        m[2][i] = z4[i];
    }
}

/**
 * \brief Get bitmaps for a glyph
 * \param info glyph info
 * Tries to get glyph bitmaps from bitmap cache.
 * If they can't be found, they are generated by rotating and rendering the glyph.
 * After that, bitmaps are added to the cache.
 * They are returned in info->bm (glyph), info->bm_o (outline).
 */
static void
get_bitmap_glyph(ASS_Renderer *render_priv, GlyphInfo *info,
                 int32_t *leftmost_x,
                 ASS_Vector *pos, ASS_Vector *pos_o,
                 ASS_DVector *offset, bool first, int flags)
{
    if (!info->outline || info->symbol == '\n' || info->symbol == 0 || info->skip) {
        ass_cache_dec_ref(info->outline);
        return;
    }

    double m1[3][3], m2[3][3], m[3][3];
    const ASS_Transform *tr = &info->transform;
    calc_transform_matrix(render_priv, info, m1);
    for (int i = 0; i < 3; i++) {
        m2[i][0] = m1[i][0] * tr->scale.x;
        m2[i][1] = m1[i][1] * tr->scale.y;
        m2[i][2] = m1[i][0] * tr->offset.x + m1[i][1] * tr->offset.y + m1[i][2];
    }
    memcpy(m, m2, sizeof(m));

    if (info->effect_type == EF_KARAOKE_KF)
        outline_update_min_transformed_x(&info->outline->outline[0], m, leftmost_x);

    BitmapHashKey key;
    key.outline = info->outline;
    if (!quantize_transform(m, pos, offset, first, &key)) {
        ass_cache_dec_ref(info->outline);
        return;
    }
    info->bm = ass_cache_get(render_priv->cache.bitmap_cache, &key, render_priv);
    if (!info->bm || !info->bm->buffer) {
        ass_cache_dec_ref(info->bm);
        info->bm = NULL;
    }
    *pos_o = *pos;

    OutlineHashKey ol_key;
    if (flags & FILTER_BORDER_STYLE_3) {
        if (!(flags & (FILTER_NONZERO_BORDER | FILTER_NONZERO_SHADOW)))
            return;

        ol_key.type = OUTLINE_BOX;

        double w = 64 * render_priv->border_scale;
        ASS_DVector bord = { info->border_x * w, info->border_y * w };
        double width = info->hspacing_scaled + info->advance.x;
        double height = info->asc + info->desc;

        ASS_DVector orig_scale;
        orig_scale.x = info->scale_x * info->scale_fix;
        orig_scale.y = info->scale_y * info->scale_fix;

        // Emulate the WTFish behavior of VSFilter, i.e. double-scale
        // the sizes of the opaque box.
        bord.x *= orig_scale.x;
        bord.y *= orig_scale.y;
        width  *= orig_scale.x;
        height *= orig_scale.y;

        // to avoid gaps
        bord.x = FFMAX(64, bord.x);
        bord.y = FFMAX(64, bord.y);

        ASS_DVector scale = {
            (width  + 2 * bord.x) / 64,
            (height + 2 * bord.y) / 64,
        };
        ASS_DVector offset = { -bord.x, -bord.y - info->asc };
        for (int i = 0; i < 3; i++) {
            m[i][0] = m1[i][0] * scale.x;
            m[i][1] = m1[i][1] * scale.y;
            m[i][2] = m1[i][0] * offset.x + m1[i][1] * offset.y + m1[i][2];
        }
    } else {
        if (!(flags & FILTER_NONZERO_BORDER))
            return;

        ol_key.type = OUTLINE_BORDER;
        BorderHashKey *k = &ol_key.u.border;
        k->outline = info->outline;

        double w = 64 * render_priv->border_scale;
        double bord_x = w * info->border_x / tr->scale.x;
        double bord_y = w * info->border_y / tr->scale.y;

        const ASS_Rect *bbox = &info->outline->cbox;
        // Estimate bounding box half size after stroking
        double dx = (bbox->x_max - bbox->x_min) / 2.0 + (bord_x + 64);
        double dy = (bbox->y_max - bbox->y_min) / 2.0 + (bord_y + 64);

        // Matrix after quantize_transform() has
        // input and output origin at bounding box center.
        double mxx = fabs(m[0][0]), mxy = fabs(m[0][1]);
        double myx = fabs(m[1][0]), myy = fabs(m[1][1]);
        double mzx = fabs(m[2][0]), mzy = fabs(m[2][1]);

        double z0 = m[2][2] - mzx * dx - mzy * dy;
        w = 1 / FFMAX(z0, m[2][2] / MAX_PERSP_SCALE);

        // Notation from quantize_transform().
        // Note that goal here is to estimate acceptable error for stroking, i. e. D(x) and D(y).
        // Matrix coefficients are constants now, so D(m_??) = 0.

        // D(z) <= |m_zx| * D(x) + |m_zy| * D(y),
        // D(x_out) = D((m_xx * x + m_xy * y) / z)
        //  <= (|m_xx| * D(x) + |m_xy| * D(y)) / z0 + x_lim * D(z) / z0^2
        //  <= (|m_xx| / z0 + |m_zx| * x_lim / z0^2) * D(x)
        //   + (|m_xy| / z0 + |m_zy| * x_lim / z0^2) * D(y),
        // D(y_out) = D((m_yx * x + m_yy * y) / z)
        //  <= (|m_yx| * D(x) + |m_yy| * D(y)) / z0 + y_lim * D(z) / z0^2
        //  <= (|m_yx| / z0 + |m_zx| * y_lim / z0^2) * D(x)
        //   + (|m_yy| / z0 + |m_zy| * y_lim / z0^2) * D(y).

        // Quantization steps (ACCURACY ~ POSITION_PRECISION):
        // STROKER_PRECISION / 2^scale_ord_x ~ D(x) ~ POSITION_PRECISION /
        //   (max(|m_xx|, |m_yx|) / z0 + |m_zx| * max(x_lim, y_lim) / z0^2),
        // STROKER_PRECISION / 2^scale_ord_y ~ D(y) ~ POSITION_PRECISION /
        //   (max(|m_xy|, |m_yy|) / z0 + |m_zy| * max(x_lim, y_lim) / z0^2).

        double x_lim = mxx * dx + mxy * dy;
        double y_lim = myx * dx + myy * dy;
        double rz = FFMAX(x_lim, y_lim) * w;

        w *= STROKER_PRECISION / POSITION_PRECISION;
        frexp(w * (FFMAX(mxx, myx) + mzx * rz), &k->scale_ord_x);
        frexp(w * (FFMAX(mxy, myy) + mzy * rz), &k->scale_ord_y);
        bord_x = ldexp(bord_x, k->scale_ord_x);
        bord_y = ldexp(bord_y, k->scale_ord_y);
        if (!(bord_x < OUTLINE_MAX && bord_y < OUTLINE_MAX))
            return;
        k->border.x = lrint(bord_x / STROKER_PRECISION);
        k->border.y = lrint(bord_y / STROKER_PRECISION);
        if (!k->border.x && !k->border.y) {
            ass_cache_inc_ref(info->bm);
            info->bm_o = info->bm;
            return;
        }

        for (int i = 0; i < 3; i++) {
            m[i][0] = ldexp(m2[i][0], -k->scale_ord_x);
            m[i][1] = ldexp(m2[i][1], -k->scale_ord_y);
            m[i][2] = m2[i][2];
        }
    }

    key.outline = ass_cache_get(render_priv->cache.outline_cache, &ol_key, render_priv);
    if (!key.outline || !key.outline->valid ||
            !quantize_transform(m, pos_o, offset, false, &key)) {
        ass_cache_dec_ref(key.outline);
        return;
    }
    info->bm_o = ass_cache_get(render_priv->cache.bitmap_cache, &key, render_priv);
    if (!info->bm_o || !info->bm_o->buffer) {
        ass_cache_dec_ref(info->bm_o);
        info->bm_o = NULL;
        *pos_o = *pos;
    } else if (!info->bm)
        *pos = *pos_o;
}

size_t ass_bitmap_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    BitmapHashKey *k = key;
    Bitmap *bm = value;

    double m[3][3];
    restore_transform(m, k);

    ASS_Outline outline[2];
    if (k->matrix_z.x || k->matrix_z.y) {
        outline_transform_3d(&outline[0], &k->outline->outline[0], m);
        outline_transform_3d(&outline[1], &k->outline->outline[1], m);
    } else {
        outline_transform_2d(&outline[0], &k->outline->outline[0], m);
        outline_transform_2d(&outline[1], &k->outline->outline[1], m);
    }

    if (!outline_to_bitmap(render_priv, bm, &outline[0], &outline[1]))
        memset(bm, 0, sizeof(*bm));
    outline_free(&outline[0]);
    outline_free(&outline[1]);

    return sizeof(BitmapHashKey) + sizeof(Bitmap) + bitmap_size(bm);
}

static void measure_text_on_eol(ASS_Renderer *render_priv, double scale, int cur_line,
                                int max_asc, int max_desc,
                                double max_border_x, double max_border_y)
{
    render_priv->text_info.lines[cur_line].asc  = scale * max_asc;
    render_priv->text_info.lines[cur_line].desc = scale * max_desc;
    render_priv->text_info.height += scale * max_asc + scale * max_desc;
    // For *VSFilter compatibility do biased rounding on max_border*
    // https://github.com/Cyberbeing/xy-VSFilter/blob/xy_sub_filter_rc4@%7B2020-05-17%7D/src/subtitles/RTS.cpp#L1465
    render_priv->text_info.border_bottom = (int) (render_priv->border_scale * max_border_y + 0.5);
    if (cur_line == 0)
        render_priv->text_info.border_top = render_priv->text_info.border_bottom;
    // VSFilter takes max \bordx into account for collision, even if far from edge
    render_priv->text_info.border_x = FFMAX(render_priv->text_info.border_x,
            (int) (render_priv->border_scale * max_border_x + 0.5));
}


/**
 * This function goes through text_info and calculates text parameters.
 * The following text_info fields are filled:
 *   height
 *   border_top
 *   border_bottom
 *   border_x
 *   lines[].asc
 *   lines[].desc
 */
static void measure_text(ASS_Renderer *render_priv)
{
    TextInfo *text_info = &render_priv->text_info;
    text_info->height = 0;
    text_info->border_x = 0;

    int cur_line = 0;
    double scale = 0.5 / 64;
    int max_asc = 0, max_desc = 0;
    double max_border_y = 0, max_border_x = 0;
    bool empty_trimmed_line = true;
    for (int i = 0; i < text_info->length; i++) {
        if (text_info->glyphs[i].linebreak) {
            measure_text_on_eol(render_priv, scale, cur_line,
                    max_asc, max_desc, max_border_x, max_border_y);
            empty_trimmed_line = true;
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
            scale = 0.5 / 64;
            cur_line++;
        }
        GlyphInfo *cur = text_info->glyphs + i;
        // VSFilter ignores metrics of line-leading/trailing (trimmed)
        // whitespace, except when the line becomes empty after trimming
        if (empty_trimmed_line && !cur->is_trimmed_whitespace) {
            empty_trimmed_line = false;
            // Forget metrics of line-leading whitespace
            max_asc = max_desc = 0;
            max_border_y = max_border_x = 0;
        } else if (!empty_trimmed_line && cur->is_trimmed_whitespace) {
            // Ignore metrics of line-trailing whitespace
            continue;
        }
        max_asc  = FFMAX(max_asc,  cur->asc);
        max_desc = FFMAX(max_desc, cur->desc);
        max_border_y = FFMAX(max_border_y, cur->border_y);
        max_border_x = FFMAX(max_border_x, cur->border_x);
        if (cur->symbol != '\n')
            scale = 1.0 / 64;
    }
    assert(cur_line == text_info->n_lines - 1);
    measure_text_on_eol(render_priv, scale, cur_line,
            max_asc, max_desc, max_border_x, max_border_y);
    text_info->height += cur_line * render_priv->settings.line_spacing;
}

/**
 * Mark extra whitespace for later removal.
 */
#define IS_WHITESPACE(x) ((x->symbol == ' ' || x->symbol == '\n') \
                          && !x->linebreak)
static void trim_whitespace(ASS_Renderer *render_priv)
{
    int i, j;
    GlyphInfo *cur;
    TextInfo *ti = &render_priv->text_info;

    // Mark trailing spaces
    i = ti->length - 1;
    cur = ti->glyphs + i;
    while (i && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + --i;
    }

    // Mark leading whitespace
    i = 0;
    cur = ti->glyphs;
    while (i < ti->length && IS_WHITESPACE(cur)) {
        cur->skip = true;
        cur->is_trimmed_whitespace = true;
        cur = ti->glyphs + ++i;
    }
    if (i < ti->length)
        cur->starts_new_run = true;

    // Mark all extraneous whitespace inbetween
    for (i = 0; i < ti->length; ++i) {
        cur = ti->glyphs + i;
        if (cur->linebreak) {
            // Mark whitespace before
            j = i - 1;
            cur = ti->glyphs + j;
            while (j && IS_WHITESPACE(cur)) {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                cur = ti->glyphs + --j;
            }
            // A break itself can contain a whitespace, too
            cur = ti->glyphs + i;
            if (cur->symbol == ' ' || cur->symbol == '\n') {
                cur->skip = true;
                cur->is_trimmed_whitespace = true;
                // Mark whitespace after
                j = i + 1;
                cur = ti->glyphs + j;
                while (j < ti->length && IS_WHITESPACE(cur)) {
                    cur->skip = true;
                    cur->is_trimmed_whitespace = true;
                    cur = ti->glyphs + ++j;
                }
                i = j - 1;
            }
            if (cur < ti->glyphs + ti->length)
                cur->starts_new_run = true;
        }
    }
}
#undef IS_WHITESPACE

/**
 * \brief rearrange text between lines
 * \param max_text_width maximal text line width in pixels
 * The algo is similar to the one in libvo/sub.c:
 * 1. Place text, wrapping it when current line is full
 * 2. Try moving words from the end of a line to the beginning of the next one while it reduces
 * the difference in lengths between this two lines.
 * The result may not be optimal, but usually is good enough.
 *
 * FIXME: implement style 0 and 3 correctly
 */
static void
wrap_lines_smart(ASS_Renderer *render_priv, double max_text_width)
{
    int i;
    GlyphInfo *cur, *s1, *e1, *s2, *s3;
    int last_space;
    int break_type;
    int exit;
    double pen_shift_x;
    double pen_shift_y;
    int cur_line;
    TextInfo *text_info = &render_priv->text_info;

    last_space = -1;
    text_info->n_lines = 1;
    break_type = 0;
    s1 = text_info->glyphs;     // current line start
    for (i = 0; i < text_info->length; ++i) {
        int break_at = -1;
        double s_offset, len;
        cur = text_info->glyphs + i;
        s_offset = d6_to_double(s1->bbox.x_min + s1->pos.x);
        len = d6_to_double(cur->bbox.x_max + cur->pos.x) - s_offset;

        if (cur->symbol == '\n') {
            break_type = 2;
            break_at = i;
            ass_msg(render_priv->library, MSGL_DBG2,
                    "forced line break at %d", break_at);
        } else if (cur->symbol == ' ') {
            last_space = i;
        } else if (len >= max_text_width
                   && (render_priv->state.wrap_style != 2)) {
            break_type = 1;
            break_at = last_space;
            if (break_at >= 0)
                ass_msg(render_priv->library, MSGL_DBG2, "line break at %d",
                        break_at);
        }

        if (break_at != -1) {
            // need to use one more line
            // marking break_at+1 as start of a new line
            int lead = break_at + 1;    // the first symbol of the new line
            if (text_info->n_lines >= text_info->max_lines) {
                // Raise maximum number of lines
                text_info->max_lines *= 2;
                text_info->lines = realloc(text_info->lines,
                                           sizeof(LineInfo) *
                                           text_info->max_lines);
            }
            if (lead < text_info->length) {
                text_info->glyphs[lead].linebreak = break_type;
                last_space = -1;
                s1 = text_info->glyphs + lead;
                text_info->n_lines++;
            }
        }
    }
#define DIFF(x,y) (((x) < (y)) ? (y - x) : (x - y))
    exit = 0;
    while (!exit && render_priv->state.wrap_style != 1) {
        exit = 1;
        s3 = text_info->glyphs;
        s1 = s2 = 0;
        for (i = 0; i <= text_info->length; ++i) {
            cur = text_info->glyphs + i;
            if ((i == text_info->length) || cur->linebreak) {
                s1 = s2;
                s2 = s3;
                s3 = cur;
                if (s1 && (s2->linebreak == 1)) {       // have at least 2 lines, and linebreak is 'soft'
                    double l1, l2, l1_new, l2_new;
                    GlyphInfo *w = s2;

                    do {
                        --w;
                    } while ((w > s1) && (w->symbol == ' '));
                    while ((w > s1) && (w->symbol != ' ')) {
                        --w;
                    }
                    e1 = w;
                    while ((e1 > s1) && (e1->symbol == ' ')) {
                        --e1;
                    }
                    if (w->symbol == ' ')
                        ++w;

                    l1 = d6_to_double(((s2 - 1)->bbox.x_max + (s2 - 1)->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2 = d6_to_double(((s3 - 1)->bbox.x_max + (s3 - 1)->pos.x) -
                        (s2->bbox.x_min + s2->pos.x));
                    l1_new = d6_to_double(
                        (e1->bbox.x_max + e1->pos.x) -
                        (s1->bbox.x_min + s1->pos.x));
                    l2_new = d6_to_double(
                        ((s3 - 1)->bbox.x_max + (s3 - 1)->pos.x) -
                        (w->bbox.x_min + w->pos.x));

                    if (DIFF(l1_new, l2_new) < DIFF(l1, l2)) {
                        if (w->linebreak || w == text_info->glyphs)
                            text_info->n_lines--;
                        if (w != text_info->glyphs)
                            w->linebreak = 1;
                        s2->linebreak = 0;
                        exit = 0;
                    }
                }
            }
            if (i == text_info->length)
                break;
        }

    }
    assert(text_info->n_lines >= 1);
#undef DIFF

    trim_whitespace(render_priv);
    measure_text(render_priv);

    cur_line = 1;

    i = 0;
    cur = text_info->glyphs + i;
    while (i < text_info->length && cur->skip)
        cur = text_info->glyphs + ++i;
    pen_shift_x = d6_to_double(-cur->pos.x);
    pen_shift_y = 0.;

    for (i = 0; i < text_info->length; ++i) {
        cur = text_info->glyphs + i;
        if (cur->linebreak) {
            while (i < text_info->length && cur->skip && cur->symbol != '\n')
                cur = text_info->glyphs + ++i;
            double height =
                text_info->lines[cur_line - 1].desc +
                text_info->lines[cur_line].asc;
            text_info->lines[cur_line - 1].len = i -
                text_info->lines[cur_line - 1].offset;
            text_info->lines[cur_line].offset = i;
            cur_line++;
            pen_shift_x = d6_to_double(-cur->pos.x);
            pen_shift_y += height + render_priv->settings.line_spacing;
        }
        cur->pos.x += double_to_d6(pen_shift_x);
        cur->pos.y += double_to_d6(pen_shift_y);
    }
    text_info->lines[cur_line - 1].len =
        text_info->length - text_info->lines[cur_line - 1].offset;

#if 0
    // print line info
    for (i = 0; i < text_info->n_lines; i++) {
        printf("line %d offset %d length %d\n", i, text_info->lines[i].offset,
                text_info->lines[i].len);
    }
#endif
}

/**
 * \brief Calculate base point for positioning and rotation
 * \param bbox text bbox
 * \param alignment alignment
 * \param bx, by out: base point coordinates
 */
static void get_base_point(ASS_DRect *bbox, int alignment, double *bx, double *by)
{
    const int halign = alignment & 3;
    const int valign = alignment & 12;
    if (bx)
        switch (halign) {
        case HALIGN_LEFT:
            *bx = bbox->x_min;
            break;
        case HALIGN_CENTER:
            *bx = (bbox->x_max + bbox->x_min) / 2.0;
            break;
        case HALIGN_RIGHT:
            *bx = bbox->x_max;
            break;
        }
    if (by)
        switch (valign) {
        case VALIGN_TOP:
            *by = bbox->y_min;
            break;
        case VALIGN_CENTER:
            *by = (bbox->y_max + bbox->y_min) / 2.0;
            break;
        case VALIGN_SUB:
            *by = bbox->y_max;
            break;
        }
}

/**
 * \brief Adjust the glyph's font size and scale factors to ensure smooth
 *  scaling and handle pathological font sizes. The main problem here is
 *  freetype's grid fitting, which destroys animations by font size, or will
 *  result in incorrect final text size if font sizes are very small and
 *  scale factors very large. See Google Code issue #46.
 * \param priv guess what
 * \param glyph the glyph to be modified
 */
static void
fix_glyph_scaling(ASS_Renderer *priv, GlyphInfo *glyph)
{
    double ft_size;
    if (priv->settings.hinting == ASS_HINTING_NONE) {
        // arbitrary, not too small to prevent grid fitting rounding effects
        // XXX: this is a rather crude hack
        ft_size = 256.0;
    } else {
        // If hinting is enabled, we want to pass the real font size
        // to freetype. Normalize scale_y to 1.0.
        ft_size = glyph->scale_y * glyph->font_size;
    }
    double mul = glyph->font_size / ft_size;
    glyph->scale_fix = 1 / mul;
    glyph->scale_x *= mul;
    glyph->scale_y *= mul;
    glyph->font_size = ft_size;
}

// Initial run splitting based purely on the characters' styles
static void split_style_runs(ASS_Renderer *render_priv)
{
    Effect last_effect_type = render_priv->text_info.glyphs[0].effect_type;
    render_priv->text_info.glyphs[0].starts_new_run = true;
    for (int i = 1; i < render_priv->text_info.length; i++) {
        GlyphInfo *info = render_priv->text_info.glyphs + i;
        GlyphInfo *last = render_priv->text_info.glyphs + (i - 1);
        Effect effect_type = info->effect_type;
        info->starts_new_run =
            info->effect_timing ||  // but ignore effect_skip_timing
            (effect_type != EF_NONE && effect_type != last_effect_type) ||
            info->drawing_text ||
            last->drawing_text ||
            strcmp(last->font->desc.family, info->font->desc.family) ||
            last->font->desc.vertical != info->font->desc.vertical ||
            last->font_size != info->font_size ||
            last->c[0] != info->c[0] ||
            last->c[1] != info->c[1] ||
            last->c[2] != info->c[2] ||
            last->c[3] != info->c[3] ||
            last->be != info->be ||
            last->blur != info->blur ||
            last->shadow_x != info->shadow_x ||
            last->shadow_y != info->shadow_y ||
            last->frx != info->frx ||
            last->fry != info->fry ||
            last->frz != info->frz ||
            last->fax != info->fax ||
            last->fay != info->fay ||
            last->scale_x != info->scale_x ||
            last->scale_y != info->scale_y ||
            last->border_style != info->border_style ||
            last->border_x != info->border_x ||
            last->border_y != info->border_y ||
            last->hspacing != info->hspacing ||
            last->italic != info->italic ||
            last->bold != info->bold ||
            ((last->flags ^ info->flags) & ~DECO_ROTATE);
        if (effect_type != EF_NONE)
            last_effect_type = effect_type;
    }
}

// Parse event text.
// Fill render_priv->text_info.
static bool parse_events(ASS_Renderer *render_priv, ASS_Event *event)
{
    TextInfo *text_info = &render_priv->text_info;

    char *p = event->Text, *q;
    char *drawing_text;

    // Event parsing.
    while (true) {
        drawing_text = NULL;

        // get next char, executing style override
        // this affects render_context
        unsigned code = 0;
        while (*p) {
            if ((*p == '{') && (q = strchr(p, '}'))) {
                p = parse_tags(render_priv, p, q, 1., false);
                assert(*p == '}');
                p++;
            } else if (render_priv->state.drawing_scale) {
                q = p;
                if (*p == '{')
                    q++;
                while ((*q != '{') && (*q != 0))
                    q++;
                drawing_text = strndup(p, q - p);
                code = 0xfffc; // object replacement character
                p = q;
                break;
            } else {
                code = get_next_char(render_priv, &p);
                break;
            }
        }

        if (code == 0)
            break;

        // face could have been changed in get_next_char
        if (!render_priv->state.font)
            goto fail;

        if (text_info->length >= text_info->max_glyphs) {
            // Raise maximum number of glyphs
            int new_max = 2 * FFMIN(text_info->max_glyphs, INT_MAX / 2);
            if (text_info->length >= new_max)
                goto fail;
            if (!ASS_REALLOC_ARRAY(text_info->glyphs, new_max))
                goto fail;
            text_info->max_glyphs = new_max;
        }

        GlyphInfo *info = &text_info->glyphs[text_info->length];

        // Clear current GlyphInfo
        memset(info, 0, sizeof(GlyphInfo));

        // Parse drawing
        if (drawing_text) {
            info->drawing_text = drawing_text;
            info->drawing_scale = render_priv->state.drawing_scale;
            info->drawing_pbo = render_priv->state.pbo;
        }

        // Fill glyph information
        info->symbol = code;
        info->font = render_priv->state.font;
        if (!drawing_text)
            ass_cache_inc_ref(info->font);
        for (int i = 0; i < 4; i++) {
            uint32_t clr = render_priv->state.c[i];
            // VSFilter compatibility: apply fade only when it's positive
            info->a_pre_fade[i] = _a(clr);
            if (render_priv->state.fade > 0)
                change_alpha(&clr,
                             mult_alpha(_a(clr), render_priv->state.fade), 1.);
            info->c[i] = clr;
        }

        info->effect_type = render_priv->state.effect_type;
        info->effect_timing = render_priv->state.effect_timing;
        info->effect_skip_timing = render_priv->state.effect_skip_timing;
        info->font_size =
            render_priv->state.font_size * render_priv->font_scale;
        info->be = render_priv->state.be;
        info->blur = render_priv->state.blur;
        info->shadow_x = render_priv->state.shadow_x;
        info->shadow_y = render_priv->state.shadow_y;
        info->scale_x = render_priv->state.scale_x;
        info->scale_y = render_priv->state.scale_y;
        info->border_style = render_priv->state.border_style;
        info->border_x = render_priv->state.border_x;
        info->border_y = render_priv->state.border_y;
        info->hspacing = render_priv->state.hspacing;
        info->bold = render_priv->state.bold;
        info->italic = render_priv->state.italic;
        info->flags = render_priv->state.flags;
        if (info->font->desc.vertical && code >= VERTICAL_LOWER_BOUND)
            info->flags |= DECO_ROTATE;
        info->frx = render_priv->state.frx;
        info->fry = render_priv->state.fry;
        info->frz = render_priv->state.frz;
        info->fax = render_priv->state.fax;
        info->fay = render_priv->state.fay;

        info->hspacing_scaled = double_to_d6(info->hspacing *
                render_priv->font_scale * info->scale_x);
        info->scale_fix = 1;

        if (!drawing_text)
            fix_glyph_scaling(render_priv, info);

        text_info->length++;

        render_priv->state.effect_type = EF_NONE;
        render_priv->state.effect_timing = 0;
        render_priv->state.effect_skip_timing = 0;
    }

    return true;

fail:
    free_render_context(render_priv);
    free(drawing_text);
    return false;
}

// Process render_priv->text_info and load glyph outlines.
static void retrieve_glyphs(ASS_Renderer *render_priv)
{
    GlyphInfo *glyphs = render_priv->text_info.glyphs;
    int i;

    for (i = 0; i < render_priv->text_info.length; i++) {
        GlyphInfo *info = glyphs + i;
        do {
            get_outline_glyph(render_priv, info);
            info = info->next;
        } while (info);
        info = glyphs + i;

        // Add additional space after italic to non-italic style changes
        if (i && glyphs[i - 1].italic && !info->italic) {
            int back = i - 1;
            GlyphInfo *og = &glyphs[back];
            while (back && og->bbox.x_max - og->bbox.x_min == 0
                    && og->italic)
                og = &glyphs[--back];
            if (og->bbox.x_max > og->cluster_advance.x)
                og->cluster_advance.x = og->bbox.x_max;
        }

        // add horizontal letter spacing
        info->cluster_advance.x += info->hspacing_scaled;

        // add displacement for vertical shearing
        info->cluster_advance.y += (info->fay / info->scale_x * info->scale_y) * info->cluster_advance.x;
    }
}

// Preliminary layout (for line wrapping)
static void preliminary_layout(ASS_Renderer *render_priv)
{
    ASS_Vector pen = { 0, 0 };
    for (int i = 0; i < render_priv->text_info.length; i++) {
        GlyphInfo *info = render_priv->text_info.glyphs + i;
        ASS_Vector cluster_pen = pen;
        do {
            info->pos.x = cluster_pen.x;
            info->pos.y = cluster_pen.y;

            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;

            info = info->next;
        } while (info);
        info = render_priv->text_info.glyphs + i;
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
    }
}

// Reorder text into visual order
static void reorder_text(ASS_Renderer *render_priv)
{
    TextInfo *text_info = &render_priv->text_info;
    FriBidiStrIndex *cmap = ass_shaper_reorder(render_priv->shaper, text_info);
    if (!cmap) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to reorder text");
        ass_shaper_cleanup(render_priv->shaper, text_info);
        free_render_context(render_priv);
        return;
    }

    // Reposition according to the map
    ASS_Vector pen = { 0, 0 };
    int lineno = 1;
    double last_pen_x = 0;
    double last_fay = 0;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + cmap[i];
        if (text_info->glyphs[i].linebreak) {
            pen.y -= (last_fay / info->scale_x * info->scale_y) * (pen.x - last_pen_x);
            last_pen_x = pen.x = 0;
            pen.y += double_to_d6(text_info->lines[lineno-1].desc);
            pen.y += double_to_d6(text_info->lines[lineno].asc);
            pen.y += double_to_d6(render_priv->settings.line_spacing);
            lineno++;
        }
        else if (last_fay != info->fay) {
            pen.y -= (last_fay / info->scale_x * info->scale_y) * (pen.x - last_pen_x);
            last_pen_x = pen.x;
        }
        last_fay = info->fay;
        if (info->skip) continue;
        ASS_Vector cluster_pen = pen;
        while (info) {
            info->pos.x = info->offset.x + cluster_pen.x;
            info->pos.y = info->offset.y + cluster_pen.y;
            cluster_pen.x += info->advance.x;
            cluster_pen.y += info->advance.y;
            info = info->next;
        }
        info = text_info->glyphs + cmap[i];
        pen.x += info->cluster_advance.x;
        pen.y += info->cluster_advance.y;
    }
}

static void align_lines(ASS_Renderer *render_priv, double max_text_width)
{
    TextInfo *text_info = &render_priv->text_info;
    GlyphInfo *glyphs = text_info->glyphs;
    int i, j;
    double width = 0;
    int last_break = -1;
    int halign = render_priv->state.alignment & 3;
    int justify = render_priv->state.justify;
    double max_width = 0;

    if (render_priv->state.evt_type & EVENT_HSCROLL) {
        justify = halign;
        halign = HALIGN_LEFT;
    }

    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            max_width = FFMAX(max_width,width);
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
    for (i = 0; i <= text_info->length; ++i) {   // (text_info->length + 1) is the end of the last line
        if ((i == text_info->length) || glyphs[i].linebreak) {
            double shift = 0;
            if (halign == HALIGN_LEFT) {    // left aligned, no action
                if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = max_width - width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = (max_width - width) / 2.0;
                } else {
                    shift = 0;
                }
            } else if (halign == HALIGN_RIGHT) {    // right aligned
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = max_text_width - max_width;
                } else if (justify == ASS_JUSTIFY_CENTER) {
                    shift = max_text_width - max_width + (max_width - width) / 2.0;
                } else {
                    shift = max_text_width - width;
                }
            } else if (halign == HALIGN_CENTER) {   // centered
                if (justify == ASS_JUSTIFY_LEFT) {
                    shift = (max_text_width - max_width) / 2.0;
                } else if (justify == ASS_JUSTIFY_RIGHT) {
                    shift = (max_text_width - max_width) / 2.0 + max_width - width;
                } else {
                    shift = (max_text_width - width) / 2.0;
                }
            }
            for (j = last_break + 1; j < i; ++j) {
                GlyphInfo *info = glyphs + j;
                while (info) {
                    info->pos.x += double_to_d6(shift);
                    info = info->next;
                }
            }
            last_break = i - 1;
            width = 0;
        }
        if (i < text_info->length && !glyphs[i].skip &&
                glyphs[i].symbol != '\n' && glyphs[i].symbol != 0) {
            width += d6_to_double(glyphs[i].cluster_advance.x);
        }
    }
}

static void calculate_rotation_params(ASS_Renderer *render_priv, ASS_DRect *bbox,
                                      double device_x, double device_y)
{
    ASS_DVector center;
    if (render_priv->state.have_origin) {
        center.x = x2scr_pos(render_priv, render_priv->state.org_x);
        center.y = y2scr_pos(render_priv, render_priv->state.org_y);
    } else {
        double bx = 0., by = 0.;
        get_base_point(bbox, render_priv->state.alignment, &bx, &by);
        center.x = device_x + bx;
        center.y = device_y + by;
    }

    TextInfo *text_info = &render_priv->text_info;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        while (info) {
            info->shift.x = info->pos.x + double_to_d6(device_x - center.x +
                    info->shadow_x * render_priv->border_scale /
                    render_priv->font_scale_x);
            info->shift.y = info->pos.y + double_to_d6(device_y - center.y +
                    info->shadow_y * render_priv->border_scale);
            info = info->next;
        }
    }
}


static int quantize_blur(double radius, int32_t *shadow_mask)
{
    // Gaussian filter kernel (1D):
    // G(x, r2) = exp(-x^2 / (2 * r2)) / sqrt(2 * pi * r2),
    // position unit is 1/64th of pixel, r = 64 * radius, r2 = r^2.

    // Difference between kernels with different but near r2:
    // G(x, r2 + dr2) - G(x, r2) ~= dr2 * G(x, r2) * (x^2 - r2) / (2 * r2^2).
    // Maximal possible error relative to full pixel value is half of
    // integral (from -inf to +inf) of absolute value of that difference.
    // E_max ~= dr2 / 2 * integral(G(x, r2) * |x^2 - r2| / (2 * r2^2), x)
    //  = dr2 / (4 * r2) * integral(G(y, 1) * |y^2 - 1|, y)
    //  = dr2 / (4 * r2) * 4 / sqrt(2 * pi * e)
    //  ~ dr2 / (4 * r2) ~= dr / (2 * r).
    // E_max ~ BLUR_PRECISION / 2 as we have 2 dimensions.

    // To get discretized blur radius solve the following
    // differential equation (n--quantization index):
    // dr(n) / dn = BLUR_PRECISION * r + POSITION_PRECISION, r(0) = 0,
    // r(n) = (exp(BLUR_PRECISION * n) - 1) * POSITION_PRECISION / BLUR_PRECISION,
    // n = log(1 + r * BLUR_PRECISION / POSITION_PRECISION) / BLUR_PRECISION.

    // To get shadow offset quantization estimate difference of
    // G(x + dx, r2) - G(x, r2) ~= dx * G(x, r2) * (-x / r2).
    // E_max ~= dx / 2 * integral(G(x, r2) * |x| / r2, x)
    //  = dx / sqrt(2 * pi * r2) ~ dx / (2 * r).
    // 2^ord ~ dx ~ BLUR_PRECISION * r + POSITION_PRECISION.

    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    radius *= scale;

    int ord;
    // ord = floor(log2(BLUR_PRECISION * r + POSITION_PRECISION))
    //     = floor(log2(64 * radius * BLUR_PRECISION + POSITION_PRECISION))
    //     = floor(log2((radius * scale + 1) * POSITION_PRECISION)),
    // floor(log2(x)) = frexp(x) - 1 = frexp(x / 2).
    frexp((1 + radius) * (POSITION_PRECISION / 2), &ord);
    *shadow_mask = ((uint32_t) 1 << ord) - 1;
    return lrint(log1p(radius) / BLUR_PRECISION);
}

static double restore_blur(int qblur)
{
    const double scale = 64 * BLUR_PRECISION / POSITION_PRECISION;
    double sigma = expm1(BLUR_PRECISION * qblur) / scale;
    return sigma * sigma;
}

// Convert glyphs to bitmaps, combine them, apply blur, generate shadows.
static void render_and_combine_glyphs(ASS_Renderer *render_priv,
                                      double device_x, double device_y)
{
    TextInfo *text_info = &render_priv->text_info;
    int left = render_priv->settings.left_margin;
    device_x = (device_x - left) * render_priv->font_scale_x + left;
    unsigned nb_bitmaps = 0;
    bool new_run = true;
    CombinedBitmapInfo *combined_info = text_info->combined_bitmaps;
    CombinedBitmapInfo *current_info = NULL;
    ASS_DVector offset;
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        if (info->starts_new_run) new_run = true;
        if (info->skip) {
            for (; info; info = info->next)
                ass_cache_dec_ref(info->outline);
            continue;
        }
        for (; info; info = info->next) {
            int flags = 0;
            if (info->border_style == 3)
                flags |= FILTER_BORDER_STYLE_3;
            if (info->border_x || info->border_y)
                flags |= FILTER_NONZERO_BORDER;
            if (info->shadow_x || info->shadow_y)
                flags |= FILTER_NONZERO_SHADOW;
            if (flags & FILTER_NONZERO_SHADOW &&
                (info->effect_type == EF_KARAOKE_KF ||
                 info->effect_type == EF_KARAOKE_KO ||
                 (info->a_pre_fade[0]) != 0xFF ||
                 info->border_style == 3))
                flags |= FILTER_FILL_IN_SHADOW;
            if (!(flags & FILTER_NONZERO_BORDER) &&
                !(flags & FILTER_FILL_IN_SHADOW))
                flags &= ~FILTER_NONZERO_SHADOW;
            if ((flags & FILTER_NONZERO_BORDER &&
                 info->a_pre_fade[0] == 0 &&
                 info->a_pre_fade[1] == 0 &&
                 _a(info->c[2]) == 0) ||
                info->border_style == 3)
                flags |= FILTER_FILL_IN_BORDER;

            if (new_run) {
                if (nb_bitmaps >= text_info->max_bitmaps) {
                    size_t new_size = 2 * text_info->max_bitmaps;
                    if (!ASS_REALLOC_ARRAY(text_info->combined_bitmaps, new_size)) {
                        ass_cache_dec_ref(info->outline);
                        continue;
                    }
                    text_info->max_bitmaps = new_size;
                    combined_info = text_info->combined_bitmaps;
                }
                current_info = &combined_info[nb_bitmaps];

                memcpy(&current_info->c, &info->c, sizeof(info->c));
                current_info->effect_type = info->effect_type;
                current_info->effect_timing = info->effect_timing;
                current_info->leftmost_x = OUTLINE_MAX;

                FilterDesc *filter = &current_info->filter;
                filter->flags = flags;
                filter->be = info->be;

                int32_t shadow_mask;
                double blur_scale = render_priv->blur_scale * (2 / sqrt(log(256)));
                filter->blur = quantize_blur(info->blur * blur_scale, &shadow_mask);
                if (flags & FILTER_NONZERO_SHADOW) {
                    int32_t x = double_to_d6(info->shadow_x * render_priv->border_scale);
                    int32_t y = double_to_d6(info->shadow_y * render_priv->border_scale);
                    filter->shadow.x = (x + (shadow_mask >> 1)) & ~shadow_mask;
                    filter->shadow.y = (y + (shadow_mask >> 1)) & ~shadow_mask;
                } else
                    filter->shadow.x = filter->shadow.y = 0;

                current_info->x = current_info->y = INT_MAX;
                current_info->bm = current_info->bm_o = current_info->bm_s = NULL;
                current_info->image = NULL;

                current_info->bitmap_count = current_info->max_bitmap_count = 0;
                current_info->bitmaps = malloc(MAX_SUB_BITMAPS_INITIAL * sizeof(BitmapRef));
                if (!current_info->bitmaps) {
                    ass_cache_dec_ref(info->outline);
                    continue;
                }
                current_info->max_bitmap_count = MAX_SUB_BITMAPS_INITIAL;

                nb_bitmaps++;
                new_run = false;
            }
            assert(current_info);

            // PyonFX glyph_x
            info->real_pos.x = device_x + d6_to_double(info->pos.x) * render_priv->font_scale_x;
            // end

            ASS_Vector pos, pos_o;
            info->pos.x = double_to_d6(device_x + d6_to_double(info->pos.x) * render_priv->font_scale_x);
            info->pos.y = double_to_d6(device_y) + info->pos.y;
            get_bitmap_glyph(render_priv, info, &current_info->leftmost_x, &pos, &pos_o,
                             &offset, !current_info->bitmap_count, flags);

            if (!info->bm && !info->bm_o) {
                ass_cache_dec_ref(info->bm);
                ass_cache_dec_ref(info->bm_o);
                continue;
            }

            if (current_info->bitmap_count >= current_info->max_bitmap_count) {
                size_t new_size = 2 * current_info->max_bitmap_count;
                if (!ASS_REALLOC_ARRAY(current_info->bitmaps, new_size)) {
                    ass_cache_dec_ref(info->bm);
                    ass_cache_dec_ref(info->bm_o);
                    continue;
                }
                current_info->max_bitmap_count = new_size;
            }

            current_info->x = FFMIN(current_info->x, pos.x);
            current_info->y = FFMIN(current_info->y, pos.y);
        }
    }

    // PyonFX glyph_y
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        info->real_pos.y = current_info->y;
    }
    // end
}

static inline void rectangle_combine(ASS_Rect *rect, const Bitmap *bm, ASS_Vector pos)
{
    pos.x += bm->left;
    pos.y += bm->top;
    rectangle_update(rect, pos.x, pos.y, pos.x + bm->w, pos.y + bm->h);
}

size_t ass_composite_construct(void *key, void *value, void *priv)
{
    ASS_Renderer *render_priv = priv;
    CompositeHashKey *k = key;
    CompositeHashValue *v = value;
    memset(v, 0, sizeof(*v));

    ASS_Rect rect, rect_o;
    rectangle_reset(&rect);
    rectangle_reset(&rect_o);

    size_t n_bm = 0, n_bm_o = 0;
    BitmapRef *last = NULL, *last_o = NULL;
    for (int i = 0; i < k->bitmap_count; i++) {
        BitmapRef *ref = &k->bitmaps[i];
        if (ref->bm) {
            rectangle_combine(&rect, ref->bm, ref->pos);
            last = ref;
            n_bm++;
        }
        if (ref->bm_o) {
            rectangle_combine(&rect_o, ref->bm_o, ref->pos_o);
            last_o = ref;
            n_bm_o++;
        }
    }

    int bord = be_padding(k->filter.be);
    if (!bord && n_bm == 1) {
        copy_bitmap(render_priv->engine, &v->bm, last->bm);
        v->bm.left += last->pos.x;
        v->bm.top  += last->pos.y;
    } else if (n_bm && alloc_bitmap(render_priv->engine, &v->bm,
                                    rect.x_max - rect.x_min + 2 * bord,
                                    rect.y_max - rect.y_min + 2 * bord,
                                    true)) {
        Bitmap *dst = &v->bm;
        dst->left = rect.x_min - bord;
        dst->top  = rect.y_min - bord;
        for (int i = 0; i < k->bitmap_count; i++) {
            Bitmap *src = k->bitmaps[i].bm;
            if (!src)
                continue;
            int x = k->bitmaps[i].pos.x + src->left - dst->left;
            int y = k->bitmaps[i].pos.y + src->top  - dst->top;
            assert(x >= 0 && x + src->w <= dst->w);
            assert(y >= 0 && y + src->h <= dst->h);
            unsigned char *buf = dst->buffer + y * dst->stride + x;
            render_priv->engine->add_bitmaps(buf, dst->stride,
                                             src->buffer, src->stride,
                                             src->h, src->w);
        }
    }
    if (!bord && n_bm_o == 1) {
        copy_bitmap(render_priv->engine, &v->bm_o, last_o->bm_o);
        v->bm_o.left += last_o->pos_o.x;
        v->bm_o.top  += last_o->pos_o.y;
    } else if (n_bm_o && alloc_bitmap(render_priv->engine, &v->bm_o,
                                      rect_o.x_max - rect_o.x_min + 2 * bord,
                                      rect_o.y_max - rect_o.y_min + 2 * bord,
                                      true)) {
        Bitmap *dst = &v->bm_o;
        dst->left = rect_o.x_min - bord;
        dst->top  = rect_o.y_min - bord;
        for (int i = 0; i < k->bitmap_count; i++) {
            Bitmap *src = k->bitmaps[i].bm_o;
            if (!src)
                continue;
            int x = k->bitmaps[i].pos_o.x + src->left - dst->left;
            int y = k->bitmaps[i].pos_o.y + src->top  - dst->top;
            assert(x >= 0 && x + src->w <= dst->w);
            assert(y >= 0 && y + src->h <= dst->h);
            unsigned char *buf = dst->buffer + y * dst->stride + x;
            render_priv->engine->add_bitmaps(buf, dst->stride,
                                             src->buffer, src->stride,
                                             src->h, src->w);
        }
    }

    int flags = k->filter.flags;
    double r2 = restore_blur(k->filter.blur);
    if (!(flags & FILTER_NONZERO_BORDER) || (flags & FILTER_BORDER_STYLE_3))
        ass_synth_blur(render_priv->engine, &v->bm, k->filter.be, r2);
    ass_synth_blur(render_priv->engine, &v->bm_o, k->filter.be, r2);

    if (!(flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
        fix_outline(&v->bm, &v->bm_o);

    if (flags & FILTER_NONZERO_SHADOW) {
        if (flags & FILTER_NONZERO_BORDER) {
            copy_bitmap(render_priv->engine, &v->bm_s, &v->bm_o);
            if ((flags & FILTER_FILL_IN_BORDER) && !(flags & FILTER_FILL_IN_SHADOW))
                fix_outline(&v->bm, &v->bm_s);
        } else if (flags & FILTER_BORDER_STYLE_3) {
            v->bm_s = v->bm_o;
            memset(&v->bm_o, 0, sizeof(v->bm_o));
        } else {
            copy_bitmap(render_priv->engine, &v->bm_s, &v->bm);
        }

        // Works right even for negative offsets
        // '>>' rounds toward negative infinity, '&' returns correct remainder
        v->bm_s.left += k->filter.shadow.x >> 6;
        v->bm_s.top  += k->filter.shadow.y >> 6;
        shift_bitmap(&v->bm_s, k->filter.shadow.x & SUBPIXEL_MASK, k->filter.shadow.y & SUBPIXEL_MASK);
    }

    if ((flags & FILTER_FILL_IN_SHADOW) && !(flags & FILTER_FILL_IN_BORDER))
        fix_outline(&v->bm, &v->bm_o);

    return sizeof(CompositeHashKey) + sizeof(CompositeHashValue) +
        bitmap_size(&v->bm) + bitmap_size(&v->bm_o) + bitmap_size(&v->bm_s);
}

/**
 * \brief Main ass rendering function, glues everything together
 * \param event event to render
 * \param event_images struct containing resulting images, will also be initialized
 * Process event, appending resulting ASS_Image's to images_root.
 */
static bool
ass_render_event(ASS_Renderer *render_priv, ASS_Event *event,
                 EventImages *event_images)
{
    if (event->Style >= render_priv->track->n_styles) {
        ass_msg(render_priv->library, MSGL_WARN, "No style found");
        return false;
    }
    if (!event->Text) {
        ass_msg(render_priv->library, MSGL_WARN, "Empty event");
        return false;
    }

    free_render_context(render_priv);
    init_render_context(render_priv, event);

    if (!parse_events(render_priv, event))
        return false;

    TextInfo *text_info = &render_priv->text_info;
    if (text_info->length == 0) {
        // no valid symbols in the event; this can be smth like {comment}
        free_render_context(render_priv);
        return false;
    }

    split_style_runs(render_priv);

    // Find shape runs and shape text
    ass_shaper_set_base_direction(render_priv->shaper,
            resolve_base_direction(render_priv->state.font_encoding));
    ass_shaper_find_runs(render_priv->shaper, render_priv, text_info->glyphs,
            text_info->length);
    if (!ass_shaper_shape(render_priv->shaper, text_info)) {
        ass_msg(render_priv->library, MSGL_ERR, "Failed to shape text");
        free_render_context(render_priv);
        return false;
    }

    retrieve_glyphs(render_priv);

    preliminary_layout(render_priv);

    int valign = render_priv->state.alignment & 12;

    int MarginL =
        (event->MarginL) ? event->MarginL : render_priv->state.style->MarginL;
    int MarginR =
        (event->MarginR) ? event->MarginR : render_priv->state.style->MarginR;
    int MarginV =
        (event->MarginV) ? event->MarginV : render_priv->state.style->MarginV;

    // calculate max length of a line
    double max_text_width =
        x2scr_right(render_priv, render_priv->track->PlayResX - MarginR) -
        x2scr_left(render_priv, MarginL);

    // wrap lines
    wrap_lines_smart(render_priv, max_text_width);

    // depends on glyph x coordinates being monotonous within runs, so it should be done before reorder
    process_karaoke_effects(render_priv);

    reorder_text(render_priv);

    align_lines(render_priv, max_text_width);

    // determing text bounding box
    ASS_DRect bbox;
    compute_string_bbox(text_info, &bbox);

    // determine device coordinates for text
    double device_x = 0;
    double device_y = 0;

    // handle positioned events first: an event can be both positioned and
    // scrolling, and the scrolling effect overrides the position on one axis
    if (render_priv->state.evt_type & EVENT_POSITIONED) {
        double base_x = 0;
        double base_y = 0;
        get_base_point(&bbox, render_priv->state.alignment, &base_x, &base_y);
        device_x =
            x2scr_pos(render_priv, render_priv->state.pos_x) - base_x;
        device_y =
            y2scr_pos(render_priv, render_priv->state.pos_y) - base_y;
    }

    // x coordinate
    if (render_priv->state.evt_type & EVENT_HSCROLL) {
        if (render_priv->state.scroll_direction == SCROLL_RL)
            device_x =
                x2scr_pos(render_priv,
                      render_priv->track->PlayResX -
                      render_priv->state.scroll_shift);
        else if (render_priv->state.scroll_direction == SCROLL_LR)
            device_x =
                x2scr_pos(render_priv, render_priv->state.scroll_shift) -
                (bbox.x_max - bbox.x_min);
    } else if (!(render_priv->state.evt_type & EVENT_POSITIONED)) {
        device_x = x2scr_left(render_priv, MarginL);
    }

    // y coordinate
    if (render_priv->state.evt_type & EVENT_VSCROLL) {
        if (render_priv->state.scroll_direction == SCROLL_TB)
            device_y =
                y2scr(render_priv,
                      render_priv->state.scroll_y0 +
                      render_priv->state.scroll_shift) -
                bbox.y_max;
        else if (render_priv->state.scroll_direction == SCROLL_BT)
            device_y =
                y2scr(render_priv,
                      render_priv->state.scroll_y1 -
                      render_priv->state.scroll_shift) -
                bbox.y_min;
    } else if (!(render_priv->state.evt_type & EVENT_POSITIONED)) {
        if (valign == VALIGN_TOP) {     // toptitle
            device_y =
                y2scr_top(render_priv,
                          MarginV) + text_info->lines[0].asc;
        } else if (valign == VALIGN_CENTER) {   // midtitle
            double scr_y =
                y2scr(render_priv, render_priv->track->PlayResY / 2.0);
            device_y = scr_y - (bbox.y_max + bbox.y_min) / 2.0;
        } else {                // subtitle
            double line_pos = render_priv->state.explicit ?
                0 : render_priv->settings.line_position;
            double scr_top, scr_bottom, scr_y0;
            if (valign != VALIGN_SUB)
                ass_msg(render_priv->library, MSGL_V,
                       "Invalid valign, assuming 0 (subtitle)");
            scr_bottom =
                y2scr_sub(render_priv,
                          render_priv->track->PlayResY - MarginV);
            scr_top = y2scr_top(render_priv, 0); //xxx not always 0?
            device_y = scr_bottom + (scr_top - scr_bottom) * line_pos / 100.0;
            device_y -= text_info->height;
            device_y += text_info->lines[0].asc;
            // clip to top to avoid confusion if line_position is very high,
            // turning the subtitle into a toptitle
            // also, don't change behavior if line_position is not used
            scr_y0 = scr_top + text_info->lines[0].asc;
            if (device_y < scr_y0 && line_pos > 0) {
                device_y = scr_y0;
            }
        }
    }

    // fix clip coordinates
    if (render_priv->state.explicit || !render_priv->settings.use_margins) {
        render_priv->state.clip_x0 =
            x2scr_pos_scaled(render_priv, render_priv->state.clip_x0);
        render_priv->state.clip_x1 =
            x2scr_pos_scaled(render_priv, render_priv->state.clip_x1);
        render_priv->state.clip_y0 =
            y2scr_pos(render_priv, render_priv->state.clip_y0);
        render_priv->state.clip_y1 =
            y2scr_pos(render_priv, render_priv->state.clip_y1);

        if (render_priv->state.explicit) {
            // we still need to clip against screen boundaries
            double zx = x2scr_pos_scaled(render_priv, 0);
            double zy = y2scr_pos(render_priv, 0);
            double sx = x2scr_pos_scaled(render_priv, render_priv->track->PlayResX);
            double sy = y2scr_pos(render_priv, render_priv->track->PlayResY);

            render_priv->state.clip_x0 = FFMAX(render_priv->state.clip_x0, zx);
            render_priv->state.clip_y0 = FFMAX(render_priv->state.clip_y0, zy);
            render_priv->state.clip_x1 = FFMIN(render_priv->state.clip_x1, sx);
            render_priv->state.clip_y1 = FFMIN(render_priv->state.clip_y1, sy);
        }
    } else {
        // no \clip (explicit==0) and use_margins => only clip to screen with margins
        render_priv->state.clip_x0 = 0;
        render_priv->state.clip_y0 = 0;
        render_priv->state.clip_x1 = render_priv->settings.frame_width;
        render_priv->state.clip_y1 = render_priv->settings.frame_height;
    }

    if (render_priv->state.evt_type & EVENT_VSCROLL) {
        double y0 = y2scr_pos(render_priv, render_priv->state.scroll_y0);
        double y1 = y2scr_pos(render_priv, render_priv->state.scroll_y1);

        render_priv->state.clip_y0 = FFMAX(render_priv->state.clip_y0, y0);
        render_priv->state.clip_y1 = FFMIN(render_priv->state.clip_y1, y1);
    }

    calculate_rotation_params(render_priv, &bbox, device_x, device_y);

    render_and_combine_glyphs(render_priv, device_x, device_y);

    // PyonFX removed
    // ass_shaper_cleanup(render_priv->shaper, text_info);
    // free_render_context(render_priv);
    // end

    return true;
}

/**
 * \brief Check cache limits and reset cache if they are exceeded
 */
static void check_cache_limits(ASS_Renderer *priv, CacheStore *cache)
{
    ass_cache_cut(cache->composite_cache, cache->composite_max_size);
    ass_cache_cut(cache->bitmap_cache, cache->bitmap_max_size);
    ass_cache_cut(cache->outline_cache, cache->glyph_max);
}

/**
 * \brief Start a new frame
 */
static bool
ass_start_frame(ASS_Renderer *render_priv, ASS_Track *track,
                long long now)
{
    ASS_Settings *settings_priv = &render_priv->settings;

    if (!render_priv->settings.frame_width
        && !render_priv->settings.frame_height)
        return false;               // library not initialized

    if (!render_priv->fontselect)
        return false;

    if (render_priv->library != track->library)
        return false;

    if (track->n_events == 0)
        return false;               // nothing to do

    render_priv->track = track;
    render_priv->time = now;

    ass_lazy_track_init(render_priv->library, render_priv->track);

    ass_shaper_set_kerning(render_priv->shaper, track->Kerning);
    ass_shaper_set_language(render_priv->shaper, track->Language);
    ass_shaper_set_level(render_priv->shaper, render_priv->settings.shaper);
#ifdef USE_FRIBIDI_EX_API
    ass_shaper_set_bidi_brackets(render_priv->shaper,
            track->parser_priv->bidi_brackets);
#endif

    // PAR correction
    double par = render_priv->settings.par;
    if (par == 0.) {
        if (render_priv->orig_width && render_priv->orig_height &&
            settings_priv->storage_width && settings_priv->storage_height) {
            double dar = ((double) render_priv->orig_width) /
                         render_priv->orig_height;
            double sar = ((double) settings_priv->storage_width) /
                         settings_priv->storage_height;
            par = dar / sar;
        } else
            par = 1.0;
    }
    render_priv->font_scale_x = par;

    render_priv->prev_images_root = render_priv->images_root;
    render_priv->images_root = NULL;

    check_cache_limits(render_priv, &render_priv->cache);

    return true;
}

static int cmp_event_layer(const void *p1, const void *p2)
{
    ASS_Event *e1 = ((EventImages *) p1)->event;
    ASS_Event *e2 = ((EventImages *) p2)->event;
    if (e1->Layer < e2->Layer)
        return -1;
    if (e1->Layer > e2->Layer)
        return 1;
    if (e1->ReadOrder < e2->ReadOrder)
        return -1;
    if (e1->ReadOrder > e2->ReadOrder)
        return 1;
    return 0;
}

static ASS_RenderPriv *get_render_priv(ASS_Renderer *render_priv,
                                       ASS_Event *event)
{
    if (!event->render_priv) {
        event->render_priv = calloc(1, sizeof(ASS_RenderPriv));
        if (!event->render_priv)
            return NULL;
    }
    if (render_priv->render_id != event->render_priv->render_id) {
        memset(event->render_priv, 0, sizeof(ASS_RenderPriv));
        event->render_priv->render_id = render_priv->render_id;
    }

    return event->render_priv;
}

static int overlap(Rect *s1, Rect *s2)
{
    if (s1->y0 >= s2->y1 || s2->y0 >= s1->y1 ||
        s1->x0 >= s2->x1 || s2->x0 >= s1->x1)
        return 0;
    return 1;
}

static int cmp_rect_y0(const void *p1, const void *p2)
{
    return ((Rect *) p1)->y0 - ((Rect *) p2)->y0;
}

static void
shift_event(ASS_Renderer *render_priv, EventImages *ei, int shift)
{
    ASS_Image *cur = ei->imgs;
    while (cur) {
        cur->dst_y += shift;
        // clip top and bottom
        if (cur->dst_y < 0) {
            int clip = -cur->dst_y;
            cur->h -= clip;
            cur->bitmap += clip * cur->stride;
            cur->dst_y = 0;
        }
        if (cur->dst_y + cur->h >= render_priv->height) {
            int clip = cur->dst_y + cur->h - render_priv->height;
            cur->h -= clip;
        }
        if (cur->h <= 0) {
            cur->h = 0;
            cur->dst_y = 0;
        }
        cur = cur->next;
    }
    ei->top += shift;
}

// dir: 1 - move down
//      -1 - move up
static int fit_rect(Rect *s, Rect *fixed, int *cnt, int dir)
{
    int i;
    int shift = 0;

    if (dir == 1)               // move down
        for (i = 0; i < *cnt; ++i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y1 - s->y0;
    } else                      // dir == -1, move up
        for (i = *cnt - 1; i >= 0; --i) {
            if (s->y1 + shift <= fixed[i].y0 || s->y0 + shift >= fixed[i].y1 ||
                s->x1 <= fixed[i].x0 || s->x0 >= fixed[i].x1)
                continue;
            shift = fixed[i].y0 - s->y1;
        }

    fixed[*cnt].y0 = s->y0 + shift;
    fixed[*cnt].y1 = s->y1 + shift;
    fixed[*cnt].x0 = s->x0;
    fixed[*cnt].x1 = s->x1;
    (*cnt)++;
    qsort(fixed, *cnt, sizeof(*fixed), cmp_rect_y0);

    return shift;
}

static void
fix_collisions(ASS_Renderer *render_priv, EventImages *imgs, int cnt)
{
    Rect *used = ass_realloc_array(NULL, cnt, sizeof(*used));
    int cnt_used = 0;
    int i, j;

    if (!used)
        return;

    // fill used[] with fixed events
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        // VSFilter considers events colliding if their intersections area is non-zero,
        // zero-area events are therefore effectively fixed as well
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height > 0) { // it's a fixed event
            Rect s;
            s.y0 = priv->top;
            s.y1 = priv->top + priv->height;
            s.x0 = priv->left;
            s.x1 = priv->left + priv->width;
            if (priv->height != imgs[i].height) {       // no, it's not
                ass_msg(render_priv->library, MSGL_WARN,
                        "Event height has changed");
                priv->top = 0;
                priv->height = 0;
                priv->left = 0;
                priv->width = 0;
            }
            for (j = 0; j < cnt_used; ++j)
                if (overlap(&s, used + j)) {    // no, it's not
                    priv->top = 0;
                    priv->height = 0;
                    priv->left = 0;
                    priv->width = 0;
                }
            if (priv->height > 0) {     // still a fixed event
                used[cnt_used].y0 = priv->top;
                used[cnt_used].y1 = priv->top + priv->height;
                used[cnt_used].x0 = priv->left;
                used[cnt_used].x1 = priv->left + priv->width;
                cnt_used++;
                shift_event(render_priv, imgs + i, priv->top - imgs[i].top);
            }
        }
    }
    qsort(used, cnt_used, sizeof(*used), cmp_rect_y0);

    // try to fit other events in free spaces
    for (i = 0; i < cnt; ++i) {
        ASS_RenderPriv *priv;
        if (!imgs[i].detect_collisions || !imgs[i].height  || !imgs[i].width)
            continue;
        priv = get_render_priv(render_priv, imgs[i].event);
        if (priv && priv->height == 0) {        // not a fixed event
            int shift;
            Rect s;
            s.y0 = imgs[i].top;
            s.y1 = imgs[i].top + imgs[i].height;
            s.x0 = imgs[i].left;
            s.x1 = imgs[i].left + imgs[i].width;
            shift = fit_rect(&s, used, &cnt_used, imgs[i].shift_direction);
            if (shift)
                shift_event(render_priv, imgs + i, shift);
            // make it fixed
            priv->top = imgs[i].top;
            priv->height = imgs[i].height;
            priv->left = imgs[i].left;
            priv->width = imgs[i].width;
        }

    }

    free(used);
}

/**
 * \brief compare two images
 * \param i1 first image
 * \param i2 second image
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_image_compare(ASS_Image *i1, ASS_Image *i2)
{
    if (i1->w != i2->w)
        return 2;
    if (i1->h != i2->h)
        return 2;
    if (i1->stride != i2->stride)
        return 2;
    if (i1->color != i2->color)
        return 2;
    if (i1->bitmap != i2->bitmap)
        return 2;
    if (i1->dst_x != i2->dst_x)
        return 1;
    if (i1->dst_y != i2->dst_y)
        return 1;
    return 0;
}

/**
 * \brief compare current and previous image list
 * \param priv library handle
 * \return 0 if identical, 1 if different positions, 2 if different content
 */
static int ass_detect_change(ASS_Renderer *priv)
{
    ASS_Image *img, *img2;
    int diff;

    img = priv->prev_images_root;
    img2 = priv->images_root;
    diff = 0;
    while (img && diff < 2) {
        ASS_Image *next, *next2;
        next = img->next;
        if (img2) {
            int d = ass_image_compare(img, img2);
            if (d > diff)
                diff = d;
            next2 = img2->next;
        } else {
            // previous list is shorter
            diff = 2;
            break;
        }
        img = next;
        img2 = next2;
    }

    // is the previous list longer?
    if (img2)
        diff = 2;

    return diff;
}

/**
 * \brief render a frame
 * \param priv library handle
 * \param track track
 * \param now current video timestamp (ms)
 * \param detect_change a value describing how the new images differ from the previous ones will be written here:
 *        0 if identical, 1 if different positions, 2 if different content.
 *        Can be NULL, in that case no detection is performed.
 */
ASS_Image *ass_render_frame(ASS_Renderer *priv, ASS_Track *track,
                            long long now, int *detect_change)
{
    // init frame
    if (!ass_start_frame(priv, track, now)) {
        if (detect_change)
            *detect_change = 2;
        return NULL;
    }

    // render events separately
    int cnt = 0;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if ((event->Start <= now)
            && (now < (event->Start + event->Duration))) {
            if (cnt >= priv->eimg_size) {
                priv->eimg_size += 100;
                priv->eimg =
                    realloc(priv->eimg,
                            priv->eimg_size * sizeof(EventImages));
            }
            if (ass_render_event(priv, event, priv->eimg + cnt))
                cnt++;
        }
    }

    // sort by layer
    if (cnt > 0)
        qsort(priv->eimg, cnt, sizeof(EventImages), cmp_event_layer);

    // call fix_collisions for each group of events with the same layer
    EventImages *last = priv->eimg;
    for (int i = 1; i < cnt; i++)
        if (last->event->Layer != priv->eimg[i].event->Layer) {
            fix_collisions(priv, last, priv->eimg + i - last);
            last = priv->eimg + i;
        }
    if (cnt > 0)
        fix_collisions(priv, last, priv->eimg + cnt - last);

    // concat lists
    ASS_Image **tail = &priv->images_root;
    for (int i = 0; i < cnt; i++) {
        ASS_Image *cur = priv->eimg[i].imgs;
        while (cur) {
            *tail = cur;
            tail = &cur->next;
            cur = cur->next;
        }
    }
    ass_frame_ref(priv->images_root);

    if (detect_change)
        *detect_change = ass_detect_change(priv);

    // free the previous image list
    ass_frame_unref(priv->prev_images_root);
    priv->prev_images_root = NULL;

    return priv->images_root;
}

// PyonFX
/**
 * \brief get glyph info from events
 * \param priv library handle
 * \param track track
 * \param now current video timestamp (ms)
 */
struct GlyphData *ass_glyph_info(ASS_Renderer *priv, ASS_Track *track,
                            long long now)
{
    // init frame
    if (!ass_start_frame(priv, track, now)) {
        return NULL;
    }

    // render events separately
    int cnt = 0;
    for (int i = 0; i < track->n_events; i++) {
        ASS_Event *event = track->events + i;
        if ((event->Start <= now)
            && (now < (event->Start + event->Duration))) {
            if (cnt >= priv->eimg_size) {
                priv->eimg_size += 100;
                priv->eimg =
                        realloc(priv->eimg,
                                priv->eimg_size * sizeof(EventImages));
            }
            if (ass_render_event(priv, event, priv->eimg + cnt))
                cnt++;
        }
    }

    TextInfo *text_info = &priv->text_info;
    struct GlyphData *glyphData = malloc(sizeof(struct GlyphData) * text_info->length);

    // Init GlyphData array
    for (int i = 0; i < text_info->length; i++) {
        GlyphInfo *info = text_info->glyphs + i;
        glyphData[i].size = text_info->length;
        glyphData[i].pos_x = info->real_pos.x;
        glyphData[i].pos_y = info->real_pos.y;
        glyphData[i].box_xmin = info->bbox.x_min;
        glyphData[i].box_ymin = info->bbox.y_min;
        glyphData[i].box_xmax = info->bbox.x_max;
        glyphData[i].box_ymax = info->bbox.y_max;
        glyphData[i].ascender = info->asc;
        glyphData[i].descender = info->desc;
        glyphData[i].n_segments = info->outline->outline[0].n_segments;
        glyphData[i].n_points = info->outline->outline[0].n_points;
        glyphData[i].segments = info->outline->outline[0].segments;
        glyphData[i].points = info->outline->outline[0].points;
    }

    // clean up
    ass_shaper_cleanup(priv->shaper, text_info);
    free_render_context(priv);

    return glyphData;
}
// end

/**
 * \brief Add reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_ref(ASS_Image *img)
{
    if (!img)
        return;
    ((ASS_ImagePriv *) img)->ref_count++;
}

/**
 * \brief Release reference to a frame image list.
 * \param image_list image list returned by ass_render_frame()
 */
void ass_frame_unref(ASS_Image *img)
{
    if (!img || --((ASS_ImagePriv *) img)->ref_count)
        return;
    do {
        ASS_ImagePriv *priv = (ASS_ImagePriv *) img;
        img = img->next;
        ass_cache_dec_ref(priv->source);
        ass_aligned_free(priv->buffer);
        free(priv);
    } while (img);
}
