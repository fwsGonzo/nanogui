/*
    src/layout.cpp -- A collection of useful layout managers

    The grid layout was contributed by Christian Schueller.

    NanoGUI was developed by Wenzel Jakob <wenzel.jakob@epfl.ch>.
    The widget drawing code is based on the NanoVG demo application
    by Mikko Mononen.

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include <nanogui/layout.h>
#include <nanogui/widget.h>
#include <nanogui/window.h>
#include <nanogui/theme.h>
#include <nanogui/label.h>
#include <nanogui/button.h>
#include <nanogui/checkbox.h>
#include <nanogui/textbox.h>
#include <nanogui/slider.h>
#include <numeric>

NAMESPACE_BEGIN(nanogui)

int Layout::layout_margin(const Widget *widget, int margin) {
    return int(std::lround(margin * widget->theme()->m_margin_scale));
}

Vector2i Layout::target_size(NVGcontext *ctx, Widget *child, int available_width) {
    child->refresh_presentation();
    child->set_layout_width(available_width);
    Vector2i ps = child->preferred_size(ctx), fs = child->fixed_size();
    if (child->theme()->m_bounded_windows) {
        if (child->layout()) {
            if (fs.x()) fs.x() = std::max(fs.x(), ps.x());
            if (fs.y()) fs.y() = std::max(fs.y(), ps.y());
        }
        if (dynamic_cast<Button *>(child) || dynamic_cast<CheckBox *>(child)) {
            if (fs.x()) fs.x() = std::max(fs.x(), ps.x());
            if (fs.y()) fs.y() = std::max(fs.y(), ps.y());
        } else if (dynamic_cast<Label *>(child)) {
            if (fs.y()) fs.y() = std::max(fs.y(), ps.y());
        } else if (dynamic_cast<TextBox *>(child)) {
            if (fs.x()) fs.x() = std::max(fs.x(), 100);
            if (fs.y()) fs.y() = std::max(fs.y(), ps.y());
        } else if (dynamic_cast<Slider *>(child) && fs.x()) fs.x() = std::max(fs.x(), 100);
    }
    return max(child->minimum_size(ctx), Vector2i(fs.x() ? fs.x() : ps.x(), fs.y() ? fs.y() : ps.y()));
}

BoxLayout::BoxLayout(Orientation orientation, Alignment alignment,
          int margin, int spacing)
    : m_orientation(orientation), m_alignment(alignment), m_margin(margin),
      m_spacing(spacing) {
}

Vector2i BoxLayout::preferred_size(NVGcontext *ctx, const Widget *widget) const {
    const int margin = layout_margin(widget, m_margin), gap = layout_margin(widget, m_spacing);
    const auto *window = dynamic_cast<const Window *>(widget);
    const int header = window && !window->title().empty() ? widget->theme()->m_window_header_height : 0;
    const int limit = widget->layout_width() > 0 ? std::max(1, widget->layout_width() - 2 * margin) : 0;
    const bool horizontal = m_orientation == Orientation::Horizontal;
    int x = 0, y = 0, row_height = 0, width = 0;
    for (auto child : widget->children()) {
        if (!child->visible()) continue;
        Vector2i size = target_size(ctx, child, limit);
        if (horizontal) {
            if (limit && x && x + gap + size.x() > limit) { y += row_height + gap; x = 0; row_height = 0; }
            x += (x ? gap : 0) + size.x();
            width = std::max(width, x); row_height = std::max(row_height, size.y());
        } else {
            y += (y ? gap : 0) + size.y(); width = std::max(width, size.x());
        }
    }
    return Vector2i(width + 2 * margin, y + row_height + 2 * margin + header - (!horizontal && header ? margin / 2 : 0));
}

void BoxLayout::perform_layout(NVGcontext *ctx, Widget *widget) const {
    const int margin = layout_margin(widget, m_margin), gap = layout_margin(widget, m_spacing);
    const auto *window = dynamic_cast<const Window *>(widget);
    const int header = window && !window->title().empty() ? widget->theme()->m_window_header_height : 0;
    const bool horizontal = m_orientation == Orientation::Horizontal;
    const int available = std::max(1, widget->width() - 2 * margin);
    std::vector<std::pair<Widget *, Vector2i>> children;
    int total_height = 0, total_width = 0, flexible = 0;
    for (auto child : widget->children()) {
        if (!child->visible()) continue;
        auto size = target_size(ctx, child, widget->theme()->m_bounded_windows ? available : 0);
        total_height += size.y() + (children.empty() ? 0 : gap);
        total_width += size.x() + (children.empty() ? 0 : gap);
        flexible += child->flexible_height() ? 1 : 0;
        children.emplace_back(child, size);
    }
    const int slack = widget->height() - header + (header ? margin / 2 : 0) - 2 * margin - total_height;
    int x = margin, y = margin + header - (!horizontal && header ? margin / 2 : 0), row_height = 0;
    for (auto &[child, size] : children) {
        Vector2i pos(x, y);
        if (horizontal) {
            if (widget->theme()->m_bounded_windows && x > margin && x + size.x() > widget->width() - margin) {
                x = margin; y += row_height + gap; row_height = 0;
            }
            pos = Vector2i(x, y);
            if (!widget->theme()->m_bounded_windows || total_width <= available) {
                if (m_alignment == Alignment::Middle) pos.y() = header + (widget->height() - header - size.y()) / 2;
                else if (m_alignment == Alignment::Maximum) pos.y() = widget->height() - size.y() - 2 * margin;
                else if (m_alignment == Alignment::Fill && !child->fixed_height()) size.y() = std::max(1, widget->height() - header - 2 * margin);
            }
            x += size.x() + gap; row_height = std::max(row_height, size.y());
        } else {
            if (child->flexible_height() && flexible) size.y() = std::max(32, size.y() + slack / flexible);
            if (m_alignment == Alignment::Middle) pos.x() = (widget->width() - size.x()) / 2;
            else if (m_alignment == Alignment::Maximum) pos.x() = widget->width() - size.x() - 2 * margin;
            else if (m_alignment == Alignment::Fill && !child->fixed_width()) size.x() = std::max(available, child->minimum_size(ctx).x());
            y += size.y() + gap;
        }
        child->set_position(pos); child->set_size(size); child->perform_layout(ctx);
    }
}

Vector2i GroupLayout::preferred_size(NVGcontext *ctx, const Widget *widget) const {
    const int margin = layout_margin(widget, m_margin);
    int height = margin, width = 2*margin;

    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        height += widget->theme()->m_window_header_height - margin/2;

    bool first = true, indent = false;
    for (auto c : widget->children()) {
        if (!c->visible())
            continue;
        const Label *label = dynamic_cast<const Label *>(c);
        if (!first)
            height += (label == nullptr) ? layout_margin(widget, m_spacing) : layout_margin(widget, m_group_spacing);
        first = false;

        Vector2i target_size = Layout::target_size(ctx, c, widget->layout_width() > 0 ? std::max(1, widget->layout_width() - 2*margin) : 0);

        bool indent_cur = indent && label == nullptr;
        height += target_size.y();
        width = std::max(width, target_size.x() + 2*margin + (indent_cur ? layout_margin(widget, m_group_indent) : 0));

        if (label)
            indent = !label->caption().empty();
    }
    height += margin;
    return Vector2i(width, height);
}

void GroupLayout::perform_layout(NVGcontext *ctx, Widget *widget) const {
    const int margin = layout_margin(widget, m_margin);
    int height = margin, available_width =
        widget->width() - 2*margin;

    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        height += widget->theme()->m_window_header_height - margin/2;

    int flexible = 0;
    for (auto* child : widget->children()) if (child->visible() && child->flexible_height()) ++flexible;
    const int slack = widget->height() - preferred_size(ctx, widget).y();
    bool first = true, indent = false;
    for (auto c : widget->children()) {
        if (!c->visible())
            continue;
        const Label *label = dynamic_cast<const Label *>(c);
        if (!first)
            height += (label == nullptr) ? layout_margin(widget, m_spacing) : layout_margin(widget, m_group_spacing);
        first = false;

        bool indent_cur = indent && label == nullptr;
        const int width = std::max(1, available_width - (indent_cur ? layout_margin(widget, m_group_indent) : 0));
        Vector2i target_size = Layout::target_size(ctx, c, widget->theme()->m_bounded_windows ? width : 0);
        if (!c->fixed_width()) target_size.x() = c->flexible_height() ? std::max(width, c->minimum_size(ctx).x()) : std::max(width, target_size.x());
        if (c->flexible_height() && flexible) target_size.y() = std::max(32, target_size.y() + slack / flexible);

        c->set_position(Vector2i(margin + (indent_cur ? layout_margin(widget, m_group_indent) : 0), height));
        c->set_size(target_size);
        c->perform_layout(ctx);

        height += target_size.y();

        if (label)
            indent = !label->caption().empty();
    }
}

Vector2i GridLayout::preferred_size(NVGcontext *ctx,
                                   const Widget *widget) const {
    const int margin = layout_margin(widget, m_margin);
    /* Compute minimum row / column sizes */
    std::vector<int> grid[2];
    compute_layout(ctx, widget, grid);

    Vector2i size(
        2*margin + std::accumulate(grid[0].begin(), grid[0].end(), 0)
         + std::max((int) grid[0].size() - 1, 0) * layout_margin(widget, m_spacing[0]),
        2*margin + std::accumulate(grid[1].begin(), grid[1].end(), 0)
         + std::max((int) grid[1].size() - 1, 0) * layout_margin(widget, m_spacing[1])
    );

    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        size[1] += widget->theme()->m_window_header_height - margin/2;

    return size;
}

void GridLayout::compute_layout(NVGcontext *ctx, const Widget *widget, std::vector<int> *grid) const {
    int axis1 = (int) m_orientation, axis2 = (axis1 + 1) % 2;
    size_t num_children = widget->children().size(), visible_children = 0;
    for (auto w : widget->children())
        visible_children += w->visible() ? 1 : 0;

    Vector2i dim;
    dim[axis1] = m_resolution;
    dim[axis2] = (int) ((visible_children + m_resolution - 1) / m_resolution);

    grid[axis1].clear(); grid[axis1].resize(dim[axis1], 0);
    grid[axis2].clear(); grid[axis2].resize(dim[axis2], 0);

    size_t child = 0;
    for (int i2 = 0; i2 < dim[axis2]; i2++) {
        for (int i1 = 0; i1 < dim[axis1]; i1++) {
            Widget *w = nullptr;
            do {
                if (child >= num_children)
                    return;
                w = widget->children()[child++];
            } while (!w->visible());

            Vector2i ps = w->preferred_size(ctx);
            Vector2i fs = w->fixed_size();
            Vector2i target_size(
                fs[0] ? fs[0] : ps[0],
                fs[1] ? fs[1] : ps[1]
            );

            grid[axis1][i1] = std::max(grid[axis1][i1], target_size[axis1]);
            grid[axis2][i2] = std::max(grid[axis2][i2], target_size[axis2]);
        }
    }
}

void GridLayout::perform_layout(NVGcontext *ctx, Widget *widget) const {
    const int margin = layout_margin(widget, m_margin);
    Vector2i fs_w = widget->fixed_size();
    Vector2i container_size(
        widget->bounded() ? widget->width() : (fs_w[0] ? fs_w[0] : widget->width()),
        widget->bounded() ? widget->height() : (fs_w[1] ? fs_w[1] : widget->height())
    );

    /* Compute minimum row / column sizes */
    std::vector<int> grid[2];
    compute_layout(ctx, widget, grid);
    int dim[2] = { (int) grid[0].size(), (int) grid[1].size() };

    Vector2i extra(0);
    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        extra[1] += widget->theme()->m_window_header_height - margin / 2;

    /* Strech to size provided by \c widget */
    for (int i = 0; i < 2; i++) {
        int grid_size = 2 * margin + extra[i];
        for (int s : grid[i]) grid_size += s;
        grid_size += std::max(0, dim[i] - 1) * layout_margin(widget, m_spacing[i]);

        if (dim[i] > 0 && grid_size < container_size[i]) {
            /* Re-distribute remaining space evenly */
            int gap = container_size[i] - grid_size;
            int g = gap / dim[i];
            int rest = gap - g * dim[i];
            for (int j = 0; j < dim[i]; ++j)
                grid[i][j] += g;
            for (int j = 0; rest > 0 && j < dim[i]; --rest, ++j)
                grid[i][j] += 1;
        }
    }

    int axis1 = (int) m_orientation, axis2 = (axis1 + 1) % 2;
    Vector2i start = margin + extra;

    size_t num_children = widget->children().size();
    size_t child = 0;

    Vector2i pos = start;
    for (int i2 = 0; i2 < dim[axis2]; i2++) {
        pos[axis1] = start[axis1];
        for (int i1 = 0; i1 < dim[axis1]; i1++) {
            Widget *w = nullptr;
            do {
                if (child >= num_children)
                    return;
                w = widget->children()[child++];
            } while (!w->visible());

            Vector2i ps = w->preferred_size(ctx);
            Vector2i fs = w->fixed_size();
            Vector2i target_size(
                fs[0] ? fs[0] : ps[0],
                fs[1] ? fs[1] : ps[1]
            );

            Vector2i item_pos(pos);
            for (int j = 0; j < 2; j++) {
                int axis = (axis1 + j) % 2;
                int item = j == 0 ? i1 : i2;
                Alignment align = alignment(axis, item);

                switch (align) {
                    case Alignment::Minimum:
                        break;
                    case Alignment::Middle:
                        item_pos[axis] += (grid[axis][item] - target_size[axis]) / 2;
                        break;
                    case Alignment::Maximum:
                        item_pos[axis] += grid[axis][item] - target_size[axis];
                        break;
                    case Alignment::Fill:
                        target_size[axis] = fs[axis] ? fs[axis] : grid[axis][item];
                        break;
                }
            }
            w->set_position(item_pos);
            w->set_size(target_size);
            w->perform_layout(ctx);
            pos[axis1] += grid[axis1][i1] + layout_margin(widget, m_spacing[axis1]);
        }
        pos[axis2] += grid[axis2][i2] + layout_margin(widget, m_spacing[axis2]);
    }
}

AdvancedGridLayout::AdvancedGridLayout(const std::vector<int> &cols, const std::vector<int> &rows, int margin)
 : m_cols(cols), m_rows(rows), m_margin(margin) {
    m_col_stretch.resize(m_cols.size(), 0);
    m_row_stretch.resize(m_rows.size(), 0);
}

Vector2i AdvancedGridLayout::preferred_size(NVGcontext *ctx, const Widget *widget) const {
    const int margin = layout_margin(widget, m_margin);
    /* Compute minimum row / column sizes */
    std::vector<int> grid[2];
    compute_layout(ctx, widget, grid);

    Vector2i size(
        std::accumulate(grid[0].begin(), grid[0].end(), 0),
        std::accumulate(grid[1].begin(), grid[1].end(), 0));

    Vector2i extra(2 * margin);
    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        extra[1] += widget->theme()->m_window_header_height - margin/2;

    return size+extra;
}

void AdvancedGridLayout::perform_layout(NVGcontext *ctx, Widget *widget) const {
    const int margin = layout_margin(widget, m_margin);
    std::vector<int> grid[2];
    compute_layout(ctx, widget, grid);

    grid[0].insert(grid[0].begin(), margin);
    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        grid[1].insert(grid[1].begin(), widget->theme()->m_window_header_height + margin/2);
    else
        grid[1].insert(grid[1].begin(), margin);

    for (int axis=0; axis<2; ++axis) {
        for (size_t i=1; i<grid[axis].size(); ++i)
            grid[axis][i] += grid[axis][i-1];

        for (Widget *w : widget->children()) {
            if (!w->visible() || dynamic_cast<const Window *>(w) != nullptr)
                continue;
            Anchor anchor = this->anchor(w);

            int item_pos = grid[axis][anchor.pos[axis]];
            int cell_size  = grid[axis][anchor.pos[axis] + anchor.size[axis]] - item_pos;
            int ps = w->preferred_size(ctx)[axis], fs = w->fixed_size()[axis];
            int target_size = fs ? fs : ps;

            switch (anchor.align[axis]) {
                case Alignment::Minimum:
                    break;
                case Alignment::Middle:
                    item_pos += (cell_size - target_size) / 2;
                    break;
                case Alignment::Maximum:
                    item_pos += cell_size - target_size;
                    break;
                case Alignment::Fill:
                    target_size = fs ? fs : cell_size;
                    break;
            }

            Vector2i pos = w->position(), size = w->size();
            pos[axis] = item_pos;
            size[axis] = target_size;
            w->set_position(pos);
            w->set_size(size);
            w->perform_layout(ctx);
        }
    }
}

void AdvancedGridLayout::compute_layout(NVGcontext *ctx, const Widget *widget,
                                       std::vector<int> *_grid) const {
    const int margin = layout_margin(widget, m_margin);
    Vector2i fs_w = widget->fixed_size();
    Vector2i container_size(
        widget->bounded() ? widget->width() : (fs_w[0] ? fs_w[0] : widget->width()),
        widget->bounded() ? widget->height() : (fs_w[1] ? fs_w[1] : widget->height())
    );

    Vector2i extra(2 * margin);
    const Window *window = dynamic_cast<const Window *>(widget);
    if (window && !window->title().empty())
        extra[1] += widget->theme()->m_window_header_height - margin/2;

    container_size -= extra;

    for (int axis = 0; axis < 2; ++axis) {
        std::vector<int> &grid = _grid[axis];
        const std::vector<int> &sizes = axis == 0 ? m_cols : m_rows;
        const std::vector<float> &stretch = axis == 0 ? m_col_stretch : m_row_stretch;
        grid = sizes;
        for (int &size : grid) size = layout_margin(widget, size);

        for (int phase = 0; phase < 2; ++phase) {
            for (auto pair : m_anchor) {
                const Widget *w = pair.first;
                if (!w->visible() || dynamic_cast<const Window *>(w) != nullptr)
                    continue;
                const Anchor &anchor = pair.second;
                if ((anchor.size[axis] == 1) != (phase == 0))
                    continue;
                int ps = w->preferred_size(ctx)[axis], fs = w->fixed_size()[axis];
                int target_size = fs ? fs : ps;

                if (anchor.pos[axis] + anchor.size[axis] > (int) grid.size())
                    throw std::runtime_error(
                        "Advanced grid layout: widget is out of bounds: " +
                        (std::string) anchor);

                int current_size = 0;
                float total_stretch = 0;
                for (int i = anchor.pos[axis];
                     i < anchor.pos[axis] + anchor.size[axis]; ++i) {
                    if (sizes[i] == 0 && anchor.size[axis] == 1)
                        grid[i] = std::max(grid[i], target_size);
                    current_size += grid[i];
                    total_stretch += stretch[i];
                }
                if (target_size <= current_size)
                    continue;
                if (total_stretch == 0)
                    throw std::runtime_error(
                        "Advanced grid layout: no space to place widget: " +
                        (std::string) anchor);
                float amt = (target_size - current_size) / total_stretch;
                for (int i = anchor.pos[axis];
                     i < anchor.pos[axis] + anchor.size[axis]; ++i)
                    grid[i] += (int) std::round(amt * stretch[i]);
            }
        }

        int current_size = std::accumulate(grid.begin(), grid.end(), 0);
        float total_stretch = std::accumulate(stretch.begin(), stretch.end(), 0.0f);
        if (current_size >= container_size[axis] || total_stretch == 0)
            continue;
        float amt = (container_size[axis] - current_size) / total_stretch;
        for (size_t i = 0; i<grid.size(); ++i)
            grid[i] += (int) std::round(amt * stretch[i]);
    }
}

NAMESPACE_END(nanogui)
