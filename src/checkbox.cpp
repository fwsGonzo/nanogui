/*
    src/checkbox.cpp -- Two-state check box widget

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/checkbox.h>
#include <nanogui/opengl.h>
#include <nanogui/theme.h>
#include <nanogui/icons.h>

#include <algorithm>

NAMESPACE_BEGIN(nanogui)

/// Lifts every channel towards white by \c amount, leaving alpha alone.
static Color lighten(const Color &c, float amount) {
    return Color(std::min(c.r() + amount, 1.f),
                 std::min(c.g() + amount, 1.f),
                 std::min(c.b() + amount, 1.f), c.a());
}

CheckBox::CheckBox(Widget *parent, const std::string &caption,
                   const std::function<void(bool) > &callback)
    : Widget(parent), m_caption(caption), m_pushed(false), m_checked(false),
      m_callback(callback) {
    m_icon_extra_scale = 1.2f; // widget override
}

bool CheckBox::mouse_button_event(const Vector2i &p, int button, bool down,
                                int modifiers) {
    Widget::mouse_button_event(p, button, down, modifiers);
    if (!m_enabled)
        return false;

    if (button == GLFW_MOUSE_BUTTON_1) {
        if (down) {
            m_pushed = true;
        } else if (m_pushed) {
            if (contains(p)) {
                m_checked = !m_checked;
                if (m_callback)
                    m_callback(m_checked);
            }
            m_pushed = false;
        }
        return true;
    }
    return false;
}

Vector2i CheckBox::preferred_size(NVGcontext *ctx) const {
    if (m_fixed_size != Vector2i(0))
        return m_fixed_size;
    nvgFontSize(ctx, font_size());
    nvgFontFace(ctx, "sans");
    return Vector2i(
        nvgTextBounds(ctx, 0, 0, m_caption.c_str(), nullptr, nullptr) +
            1.8f * font_size(),
        font_size() * 1.3f);
}

void CheckBox::draw(NVGcontext *ctx) {
    Widget::draw(ctx);

    nvgFontSize(ctx, font_size());
    nvgFontFace(ctx, "sans");
    nvgFillColor(ctx,
                 m_enabled ? m_theme->m_text_color : m_theme->m_disabled_text_color);
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(ctx, m_pos.x() + 1.6f * font_size(), m_pos.y() + m_size.y() * 0.5f,
            m_caption.c_str(), nullptr);

    /* The box: a square as tall as the row, inset one pixel so its outline lands
       inside the widget's own bounds and never bleeds into a neighbour. */
    const float box  = m_size.y() - 2.0f;
    const float bx   = m_pos.x() + 1.0f;
    const float by   = m_pos.y() + 1.0f;
    const float rad  = 3.0f;

    Color fill = m_pushed ? m_theme->m_check_box_background_pushed
               : m_checked ? m_theme->m_check_box_background_checked
                           : m_theme->m_check_box_background;
    Color border = focused() ? m_theme->m_check_box_border_focused
                             : m_theme->m_check_box_border;
    Color check  = m_theme->m_check_box_check;

    if (!m_enabled) {
        /* Half-transparent fill plus the disabled text color on both outline and
           mark, so a disabled box reads as inert next to an enabled one. */
        fill.a() *= 0.4f;
        border = m_theme->m_disabled_text_color;
        check  = m_theme->m_disabled_text_color;
    } else if (m_mouse_focus) {
        fill = lighten(fill, 0.10f);
    }

    /* Focus doubles the outline: a gamepad or keyboard user has no cursor to
       point at the row, so the ring is the only marker of where they are. */
    const float bw = m_theme->m_check_box_border_width * (focused() ? 2.0f : 1.0f);

    nvgBeginPath(ctx);
    nvgRoundedRect(ctx, bx + bw * 0.5f, by + bw * 0.5f,
                   box - bw, box - bw, rad);
    nvgFillColor(ctx, fill);
    nvgFill(ctx);
    nvgStrokeWidth(ctx, bw);
    nvgStrokeColor(ctx, border);
    nvgStroke(ctx);

    if (m_checked) {
        if (m_theme->m_check_box_icon == FA_CHECK) {
            /* Stroked rather than set in the icon font: the tick keeps a
               thickness proportional to the box at any GUI scale, where the
               glyph thins out at small sizes. */
            nvgBeginPath(ctx);
            nvgMoveTo(ctx, bx + box * 0.25f, by + box * 0.52f);
            nvgLineTo(ctx, bx + box * 0.44f, by + box * 0.72f);
            nvgLineTo(ctx, bx + box * 0.77f, by + box * 0.30f);
            nvgStrokeWidth(ctx, std::max(2.0f, box * 0.16f));
            nvgStrokeColor(ctx, check);
            nvgLineCap(ctx, NVG_ROUND);
            nvgLineJoin(ctx, NVG_ROUND);
            nvgStroke(ctx);
            nvgLineCap(ctx, NVG_BUTT);
            nvgLineJoin(ctx, NVG_MITER);
        } else {
            /* A theme that names its own mark gets that glyph. */
            nvgFontSize(ctx, icon_scale() * m_size.y());
            nvgFontFace(ctx, "icons");
            nvgFillColor(ctx, check);
            nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(ctx, m_pos.x() + m_size.y() * 0.5f + 1,
                    m_pos.y() + m_size.y() * 0.5f,
                    utf8(m_theme->m_check_box_icon).data(), nullptr);
        }
    }

    /* NanoVG state is global to the frame and several widgets stroke without
       setting a width first; hand back the default they expect. */
    nvgStrokeWidth(ctx, 1.0f);
}

NAMESPACE_END(nanogui)
