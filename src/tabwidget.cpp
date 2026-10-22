/*
    nanogui/tabwidget.cpp -- Widget for organizing multiple
    sub-widgets into tabs

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/tabwidget.h>
#include <nanogui/theme.h>
#include <nanogui/screen.h>
#include <nanogui/popup.h>
#include <nanogui/layout.h>
#include <nanogui/button.h>
#include <nanogui/opengl.h>
#include <nanogui/icons.h>

NAMESPACE_BEGIN(nanogui)

TabWidgetBase::TabWidgetBase(Widget *parent, const std::string &font)
    : Widget(parent), m_font(font), m_background_color(Color(0.f, 0.f)) {
    m_tab_offsets.push_back(0);
}

void TabWidgetBase::remove_tab(int id) {
    int index = tab_index(id);
    bool close_active = index == m_active_tab;
    m_tab_captions.erase(m_tab_captions.begin() + index);
    m_tab_ids.erase(m_tab_ids.begin() + index);
    m_tab_icons.erase(m_tab_icons.begin() + index);
    if (m_tooltip_tab >= 0)
        set_tooltip_tab(-1);
    if (index <= m_active_tab)
        m_active_tab = std::max(0, m_active_tab - 1);
    TabWidgetBase::perform_layout(screen()->nvg_context());
    if (m_close_callback)
        m_close_callback(id);
    if (close_active) {
        if (m_callback)
            m_callback(selected_id());
        update_visibility();
    }
}

int TabWidgetBase::insert_tab(int index, const std::string &caption) {
    int id = m_tab_counter++;
    m_tab_captions.insert(m_tab_captions.begin() + index, caption);
    m_tab_ids.insert(m_tab_ids.begin() + index, id);
    m_tab_icons.insert(m_tab_icons.begin() + index, 0);
    if (index < m_active_tab)
        m_active_tab++;
    TabWidgetBase::perform_layout(screen()->nvg_context());
    if (m_tab_ids.size() == 1) {
        m_active_tab = 0;
        if (m_callback)
            m_callback(id);
        update_visibility();
    }
    return id;
}

int TabWidgetBase::append_tab(const std::string &caption) {
    return insert_tab((int) m_tab_captions.size(), caption);
}

void TabWidgetBase::set_tab_icon(int id, int icon) {
    m_tab_icons[tab_index(id)] = icon;
    TabWidgetBase::perform_layout(screen()->nvg_context());
    screen()->request_layout();
}

int TabWidgetBase::tab_icon_size() const {
    return m_tab_icon_size > 0 ? m_tab_icon_size : int(std::lround(font_size() * 1.5f));
}

void TabWidgetBase::set_compact_captions(bool value) {
    if (m_compact_captions == value)
        return;
    m_compact_captions = value;
    set_tooltip_tab(-1);
    if (screen()) {
        TabWidgetBase::perform_layout(screen()->nvg_context());
        screen()->request_layout();
    }
}

bool TabWidgetBase::has_tab_icons() const {
    for (int icon : m_tab_icons)
        if (icon != 0)
            return true;
    return false;
}

bool TabWidgetBase::tab_caption_shown(int index) const {
    return !m_compact_captions || m_tab_icons[index] == 0 || index == m_active_tab;
}

int TabWidgetBase::tab_height() const {
    const int content = has_tab_icons() ? std::max(font_size(), tab_icon_size()) : font_size();
    return content + 2 * int(std::lround(m_theme->m_tab_button_vertical_padding * m_theme->m_margin_scale));
}

int TabWidgetBase::tab_button_width(NVGcontext *ctx, int index) const {
    int width = 2 * int(std::lround(m_theme->m_tab_button_horizontal_padding * m_theme->m_margin_scale));
    const int icon_size = tab_icon_size();
    if (m_tab_icons[index] != 0)
        width += icon_size;
    if (tab_caption_shown(index)) {
        float unused[4];
        nvgFontFace(ctx, m_font.c_str());
        nvgFontSize(ctx, font_size());
        nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        width += int(nvgTextBounds(ctx, 0, 0, m_tab_captions[index].c_str(), nullptr, unused));
        if (m_tab_icons[index] != 0)
            width += icon_size / 4;
    }
    if (m_tabs_closeable)
        width += m_close_width;
    return width;
}

void TabWidgetBase::set_tooltip_tab(int index) {
    if (index >= 0 && tab_caption_shown(index))
        index = -1;
    if (index == m_tooltip_tab)
        return;
    m_tooltip_tab = index;
    m_tooltip = index >= 0 ? m_tab_captions[index] : std::string();
}

Vector2i TabWidgetBase::tooltip_anchor() const {
    if (m_tooltip_tab < 0 || m_tooltip_tab + 1 >= int(m_tab_offsets.size()))
        return Widget::tooltip_anchor();
    const int center = (m_tab_offsets[m_tooltip_tab] + m_tab_offsets[m_tooltip_tab + 1]) / 2;
    return absolute_position() + Vector2i(center - m_tab_scroll, tab_height() + 10);
}

int TabWidgetBase::tab_index(int id) const {
    for (size_t i = 0; i<m_tab_ids.size(); ++i) {
        if (m_tab_ids[i] == id)
            return (int) i;
    }
    throw std::runtime_error("TabWidgetBase::tab_index(): not found!");
}

void TabWidgetBase::update_visibility() {
    // Compact captions follow the selection, so the tab widths do too.
    if (m_compact_captions && screen())
        TabWidgetBase::perform_layout(screen()->nvg_context());
    set_tooltip_tab(-1);
    reveal_selected_tab();
    if (screen()) screen()->request_layout();
}

void TabWidgetBase::reveal_selected_tab() {
    if (m_active_tab < 0 || m_active_tab + 1 >= int(m_tab_offsets.size())) return;
    const int left = m_tab_offsets[m_active_tab], right = m_tab_offsets[m_active_tab + 1];
    if (left < m_tab_scroll) m_tab_scroll = left;
    if (right > m_tab_scroll + width()) m_tab_scroll = std::min(left, right - width());
    m_tab_scroll = std::clamp(m_tab_scroll, 0, std::max(0, m_tab_offsets.back() - width()));
}

bool TabWidgetBase::scroll_event(const Vector2i &p, const Vector2f &rel) {
    if (p.y() - m_pos.y() < tab_height() && !m_tab_offsets.empty() && m_tab_offsets.back() > width()) {
        m_tab_scroll = std::clamp(m_tab_scroll - int((rel.x() != 0 ? rel.x() : rel.y()) * 40),
            0, std::max(0, m_tab_offsets.back() - width()));
        screen()->set_needs_redraw(true);
        return true;
    }
    return Widget::scroll_event(p, rel);
}

void TabWidgetBase::perform_layout(NVGcontext* ctx) {
    m_tab_offsets.clear();

    int width = 0;
    for (int i = 0; i < tab_count(); ++i) {
        m_tab_offsets.push_back(width);
        width += tab_button_width(ctx, i);
    }
    m_tab_offsets.push_back(width);
    reveal_selected_tab();

    float unused[4];
    nvgFontSize(ctx, font_size());
    nvgFontFace(ctx, "icons");
    m_close_width =
        nvgTextBounds(ctx, 0, 0, utf8(FA_TIMES_CIRCLE).data(), nullptr, unused);
}

Vector2i TabWidgetBase::preferred_size(NVGcontext* ctx) const {
    const int padding = int(std::lround(m_padding * m_theme->m_margin_scale));

    int width = 0;
    for (int i = 0; i < tab_count(); ++i)
        width += tab_button_width(ctx, i);

    return Vector2i(width + 1, tab_height() + 2*padding);
}

void TabWidgetBase::draw(NVGcontext* ctx) {
    if (m_tab_offsets.size() != m_tab_captions.size() + 1)
        throw std::runtime_error("Must run TabWidget::perform_layout() after adding/removing tabs!");

    const int tab_height = this->tab_height();
    const int icon_size = tab_icon_size();

    if (m_background_color.w() != 0.f) {
        nvgFillColor(ctx, m_background_color);
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + .5f, m_pos.y() + .5f + tab_height, m_size.x(),
                       m_size.y() - tab_height - 2, m_theme->m_button_corner_radius);
        nvgFill(ctx);
    }

    Widget::draw(ctx);

    NVGpaint tab_background_color = nvgLinearGradient(
        ctx, m_pos.x(), m_pos.y() + 1, m_pos.x(), m_pos.y() + tab_height,
        m_theme->m_button_gradient_top_pushed, m_theme->m_button_gradient_bot_pushed);

    nvgSave(ctx);
    nvgIntersectScissor(ctx, m_pos.x(), m_pos.y(), m_size.x(), tab_height);
    nvgFontSize(ctx, font_size());
    nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    for (size_t i = 0; i< m_tab_captions.size(); ++i) {
        int x_pos = m_pos.x() - m_tab_scroll + m_tab_offsets[i],
            y_pos = m_pos.y(),
            width = m_tab_offsets[i + 1] - m_tab_offsets[i];

        Color textColor;
        if (i == (size_t) m_active_tab) {
            // Active tab uses default text color
            textColor = m_theme->m_text_color;
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x_pos + 0.5f, y_pos + 1.5f, width,
                           tab_height + 4, m_theme->m_button_corner_radius);
            nvgStrokeColor(ctx, m_theme->m_border_light);
            nvgStroke(ctx);

            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x_pos + 0.5f, y_pos + 0.5f, width,
                           tab_height + 4, m_theme->m_button_corner_radius);
            nvgStrokeColor(ctx, m_theme->m_border_dark);
            nvgStroke(ctx);
        } else {
            // Inactive tab uses disabled text color
            textColor = m_theme->m_disabled_text_color;
            nvgBeginPath(ctx);
            nvgRoundedRect(ctx, x_pos + 0.5f, y_pos + 1.5f, width,
                           tab_height + 4, m_theme->m_button_corner_radius);

            nvgFillPaint(ctx, tab_background_color);
            nvgFill(ctx);

            nvgStrokeColor(ctx, m_theme->m_border_dark);
            nvgStroke(ctx);
        }
        x_pos += int(std::lround(m_theme->m_tab_button_horizontal_padding * m_theme->m_margin_scale));
        if (m_tab_icons[i] != 0) {
            draw_tab_icon(ctx, (int) i, x_pos, y_pos + (tab_height - icon_size) / 2 + 1,
                          icon_size, textColor);
            x_pos += icon_size + icon_size / 4;
        }
        y_pos += (tab_height - font_size()) / 2 + 1;
        if (tab_caption_shown((int) i)) {
            nvgFillColor(ctx, textColor);
            nvgFontFace(ctx, m_font.c_str());
            nvgFontSize(ctx, font_size());
            nvgTextAlign(ctx, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
            nvgText(ctx, x_pos, y_pos, m_tab_captions[i].c_str(), nullptr);
        }

        if (m_tabs_closeable) {
            x_pos = m_pos.x() - m_tab_scroll + m_tab_offsets[i + 1] -
                    int(std::lround(m_theme->m_tab_button_horizontal_padding * m_theme->m_margin_scale)) - m_close_width + 5;
            nvgFontFace(ctx, "icons");
            nvgFillColor(ctx, i == (size_t) m_close_index_pushed ? m_theme->m_text_color_shadow
                                                                 : m_theme->m_text_color);
            bool highlight = m_close_index == (int) i;
            auto icon = highlight ? FA_TIMES_CIRCLE : FA_TIMES;
            float fs       = font_size() * (highlight ? 1.f : .70f),
                  offset_x = highlight ? 0.f : (fs * .40f),
                  offset_y = highlight ? 0.f : (fs * .21f);
            nvgFontSize(ctx, fs);
            nvgText(ctx, x_pos + offset_x, y_pos + offset_y + .5f, utf8(icon).data(), nullptr);
            nvgFontSize(ctx, font_size());
        }
    }
    if (m_tab_drag_index != -1 && m_tab_drag_start != m_tab_drag_end) {
        int x_pos = m_pos.x() - m_tab_scroll + m_tab_drag_min + m_tab_drag_end - m_tab_drag_start;
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, x_pos + 0.5f, m_pos.y() + 1.5f, m_tab_drag_max - m_tab_drag_min,
                       tab_height + 4, m_theme->m_button_corner_radius);
        nvgFillColor(ctx, Color(255, 255, 255, 30));
        nvgFill(ctx);
    }
    nvgRestore(ctx);

    int x0 = std::clamp(m_tab_offsets[m_active_tab] - m_tab_scroll, 0, width()),
        x1 = std::clamp(m_tab_offsets[m_tab_offsets.size() > 1 ? m_active_tab + 1 : 0] - m_tab_scroll, 0, width());
    for (int i = 1; i >= 0; --i) {
        /* Top border */
        nvgBeginPath(ctx);
        nvgMoveTo(ctx, m_pos.x() + .5f, m_pos.y() + tab_height + i + .5f);
        nvgLineTo(ctx, m_pos.x() + x0 + 1.0f, m_pos.y() + tab_height + i + .5f);
        nvgMoveTo(ctx, m_pos.x() + x1, m_pos.y() + tab_height + i + .5f);
        nvgLineTo(ctx, m_pos.x() + m_size.x() + .5f, m_pos.y() + tab_height + i + .5f);
        nvgStrokeWidth(ctx, 1.0f);
        nvgStrokeColor(ctx, (i == 0) ? m_theme->m_border_dark : m_theme->m_border_light);
        nvgStroke(ctx);

        /* Bottom + side borders */
        nvgSave(ctx);
        nvgIntersectScissor(ctx, m_pos.x(), m_pos.y() + tab_height, m_size.x(), m_size.y());
        nvgBeginPath(ctx);
        nvgRoundedRect(ctx, m_pos.x() + .5f, m_pos.y() + i + .5f, m_size.x() - 1,
                       m_size.y() - 2, m_theme->m_button_corner_radius);
        nvgStroke(ctx);
        nvgRestore(ctx);
    }
}

void TabWidgetBase::draw_tab_icon(NVGcontext *ctx, int index, float x, float y,
                                  float size, const Color &color) {
    const int icon = m_tab_icons[index];
    if (nvg_is_font_icon(icon)) {
        nvgFontFace(ctx, "icons");
        nvgFontSize(ctx, size);
        nvgTextAlign(ctx, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(ctx, color);
        nvgText(ctx, x + size * 0.5f, y + size * 0.5f, utf8(icon).data(), nullptr);
    } else if (icon > 0) {
        int w, h;
        nvgImageSize(ctx, icon, &w, &h);
        const float scale = size / float(std::max(1, std::max(w, h)));
        const float iw = w * scale, ih = h * scale;
        const float ix = x + (size - iw) * 0.5f, iy = y + (size - ih) * 0.5f;
        nvgBeginPath(ctx);
        nvgRect(ctx, ix, iy, iw, ih);
        nvgFillPaint(ctx, nvgImagePattern(ctx, ix, iy, iw, ih, 0, icon, color.a()));
        nvgFill(ctx);
    }
}

std::pair<int, bool> TabWidgetBase::tab_at_position(const Vector2i &p, bool test_vertical) const {
    const int tab_height = this->tab_height();
    if (test_vertical && (p.y() <= m_pos.y() || p.y() > m_pos.y() + tab_height))
        return { -1, false };

    int x = p.x() - m_pos.x() + m_tab_scroll;
    for (size_t i = 0; i < m_tab_offsets.size() - 1; ++i) {
        if (x >= m_tab_offsets[i] && x < m_tab_offsets[i + 1]) {
            int r = m_tab_offsets[i + 1] - x;
            return {
                (int) i, m_tabs_closeable &&
                   r < int(std::lround(m_theme->m_tab_button_horizontal_padding * m_theme->m_margin_scale)) + m_close_width - 4 &&
                   r > int(std::lround(m_theme->m_tab_button_horizontal_padding * m_theme->m_margin_scale)) - 4 &&
                   p.y() - m_pos.y() > int(std::lround(m_theme->m_tab_button_vertical_padding * m_theme->m_margin_scale)) &&
                   p.y() - m_pos.y() <= tab_height - int(std::lround(m_theme->m_tab_button_vertical_padding * m_theme->m_margin_scale))
            };
        }
    }
    return { -1, false };
}

bool TabWidgetBase::mouse_button_event(const Vector2i &p, int button, bool down,
                                       int modifiers) {
    int index; bool close;
    std::tie(index, close) = tab_at_position(p);
    bool handled = false;

    Screen *screen = this->screen();
    if (m_popup) {
        m_popup->mouse_button_event(
            p - m_pos + absolute_position() - m_popup->absolute_position() + m_popup->position(),
            button, down, modifiers
        );
        screen->update_focus(this);
        screen->remove_child(m_popup);
        m_popup = nullptr;
        handled = true;
    }

    bool drag_in_progress = m_tab_drag_index != -1 && m_tab_drag_start != m_tab_drag_end;
    if (m_popup_callback && button == GLFW_MOUSE_BUTTON_2 && down && index != -1 &&
        !drag_in_progress) {
        m_popup = m_popup_callback(tab_id(index), screen);
        m_popup->set_position(p + Vector2i(8, -6));
        m_popup->set_anchor_offset(8);
        m_popup->set_anchor_size(8);
        if (m_popup->layout() == nullptr)
            m_popup->set_layout(new GroupLayout(5, 3));
        for (Widget *w : m_popup->children()) {
            Button *b = dynamic_cast<Button *>(w);
            if (!b)
                continue;
            b->set_icon_position(Button::IconPosition::Right);
            b->set_flags(Button::MenuButton);
        }
        NVGcontext *ctx = screen->nvg_context();
        m_popup->set_size(m_popup->preferred_size(ctx) + Vector2i(40, 0));
        m_popup->perform_layout(ctx);
        handled = true;
    }

    if (button == GLFW_MOUSE_BUTTON_1 && m_popup == nullptr) {
        if (index >= 0) {
            if (close && m_tab_drag_index == -1) {
                if (down) {
                    m_close_index_pushed = index;
                } else if (m_close_index == m_close_index_pushed) {
                    remove_tab(tab_id(index));
                    mouse_motion_event(p, Vector2i(0), 0, 0);
                }
            } else {
                if (down) {
                    bool tab_changed = m_active_tab != index;
                    m_active_tab = index;
                    m_tab_drag_index = m_tabs_draggable ? index : -1;
                    m_tab_drag_start = m_tab_drag_end = p.x();
                    m_tab_drag_min = m_tab_offsets[index];
                    m_tab_drag_max = m_tab_offsets[index + 1];
                    m_close_index_pushed = -1;
                    if (tab_changed) {
                        if (m_callback)
                            m_callback(selected_id());
                        update_visibility();
                    }
                } else if (m_tab_drag_index != -1) {
                    m_tab_drag_index = -1;
                    mouse_motion_event(p, Vector2i(0), 0, 0);
                }
            }
            handled = true;
        }

        if (!down) {
            handled = m_close_index_pushed != -1 || m_tab_drag_index != -1;
            m_close_index_pushed = -1;
            m_tab_drag_index = -1;
        }

        handled = true;
    }

    handled |= Widget::mouse_button_event(p, button, down, modifiers);

    return handled;
}

bool TabWidgetBase::mouse_enter_event(const Vector2i &/* p */, bool enter) {
    if (!enter)
        set_tooltip_tab(-1);
    if (m_tabs_closeable && m_close_index >= 0) {
        m_close_index = -1;
        m_close_index_pushed = -1;
        return true;
    }
    return false;
}

bool TabWidgetBase::mouse_motion_event(const Vector2i &p, const Vector2i &rel,
                                       int button, int modifiers) {
    auto [index, close] = tab_at_position(p, false);
    if (m_compact_captions)
        set_tooltip_tab(tab_at_position(p).first);

    if (m_tab_drag_index != -1) {
        m_tab_drag_end = p.x();
        if (index != -1 && m_tab_drag_index != index) {
            int i0 = std::min(m_tab_drag_index, index),
                i1 = std::max(m_tab_drag_index, index);
            int mid = (m_tab_offsets[i0] + m_tab_offsets[i1 + 1]) / 2;
            if ((m_tab_drag_index < index && p.x() - m_pos.x() + m_tab_scroll > mid) ||
                (m_tab_drag_index > index && p.x() - m_pos.x() + m_tab_scroll < mid)) {
                std::swap(m_tab_captions[index], m_tab_captions[m_tab_drag_index]);
                std::swap(m_tab_ids[index], m_tab_ids[m_tab_drag_index]);
                std::swap(m_tab_icons[index], m_tab_icons[m_tab_drag_index]);
                m_tab_drag_index = index;
                m_active_tab = index;
                TabWidgetBase::perform_layout(screen()->nvg_context());
            }
        }
        return true;
    }

    if (!close)
        index = -1;

    if (index != m_close_index) {
        m_close_index = index;
        m_close_index_pushed = -1;
        return true;
    }

    return Widget::mouse_motion_event(p, rel, button, modifiers);
}

TabWidget::TabWidget(Widget *parent, const std::string &font)
    : TabWidgetBase(parent, font) { set_flexible_height(true); }

void TabWidget::perform_layout(NVGcontext* ctx) {
    const int padding = int(std::lround(m_padding * m_theme->m_margin_scale));
    TabWidgetBase::perform_layout(ctx);

    const int tab_height = this->tab_height();

    for (const auto &[id, child] : m_widgets) {
        if (!child->visible()) continue;
        child->set_bounded(m_theme->m_bounded_windows);
        child->set_layout_width(std::max(1, width() - 2*padding));
        child->set_position(Vector2i(padding, padding + tab_height + 1));
        child->set_size(max(Vector2i(1), m_size - Vector2i(2*padding, 2*padding + tab_height + 1)));
        child->perform_layout(ctx);
    }
}

void TabWidget::update_visibility() {
    TabWidgetBase::update_visibility();
    for (Widget *child : m_children)
        child->set_visible(false);
    auto it = m_widgets.find(selected_id());
    if (it != m_widgets.end())
        it->second->set_visible(true);
}

Vector2i TabWidget::preferred_size(NVGcontext* ctx) const {
    const int padding = int(std::lround(m_padding * m_theme->m_margin_scale));
    Vector2i base_size = TabWidgetBase::preferred_size(ctx),
             content_size = Vector2i(0);
    for (const auto &[id, child] : m_widgets) {
        if (m_theme->m_content_scale < 1.f && id != selected_id()) continue;
        child->set_layout_width(layout_width() > 0 ? std::max(1, layout_width() - 2*padding) : 0);
        content_size = max(content_size, child->preferred_size(ctx));
    }
    if (m_theme->m_content_scale < 1.f) base_size.x() = 0;

    return Vector2i(
        std::max(base_size.x(), content_size.x() + 2 * padding),
        base_size.y() + content_size.y() + 2 * padding
    );
}

int TabWidget::insert_tab(int index, const std::string &caption, Widget *widget) {
    int id = TabWidgetBase::insert_tab(index, caption);
    m_widgets[id] = widget;
    update_visibility();
    return id;
}

int TabWidget::append_tab(const std::string &caption, Widget *widget) {
    if (widget == this)
        throw std::runtime_error("TabWidget::append_tab(): cannot append the TabWidget to itself!");
    widget->set_visible(false);
    int id = TabWidgetBase::append_tab(caption);
    m_widgets[id] = widget;
    update_visibility();
    return id;
}

void TabWidget::remove_tab(int id) {
    TabWidgetBase::remove_tab(id);
    Widget *widget = m_widgets[id];
    m_widgets.erase(id);
    if (m_remove_children)
        remove_child(widget);
}

NAMESPACE_END(nanogui)
