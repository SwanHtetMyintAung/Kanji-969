/**
 * Raven - widgets/label.c
 * Label (Slate STextBlock): one or several lines of text, optionally
 * wrapped on the width it receives; or, with Markdown turned on, formatted
 * text (widgets/markdown.c).
 */

#include "common.h"

typedef struct rvn_label_t {
	rvn_widget_t base;
	char* text;
	Rid font;
	float size;
	RTextAlign align;
	int wrap;
	/** R_TRUE once the application chose whether to wrap: Markdown then stops deciding. */
	int wrap_chosen;
	/** Width received at the last arrange, used to wrap; 0 before the first one. */
	float wrap_width;

	int markdown;
	/** The text, parsed and laid out; made when first measured, forgotten when the text changes. */
	rvn_md_t* md;
	/** Link under the pointer, link the button went down on; -1 = none. */
	int hot_link;
	int pressed_link;
	RLabelLinkCallback link_callback;
	void* link_user;
	/** Result of Rlabel_get_plain_text. */
	char* plain;

	/** Selectable text (Rlabel_set_selectable): the selection is [min, max) of the two ends, in bytes of the text. */
	int selectable;
	int sel_anchor;
	int sel_caret;
	int selecting;
	/** Direction of the last paint, so that a click lands where the text was drawn. */
	int rtl;
} rvn_label_t;

static void rvn_label_init(rvn_widget_t* widget) {
	rvn_label_t* label = (rvn_label_t*)widget;
	label->font = R_INVALID_ID;
	label->align = R_TEXT_ALIGN_LEFT;
	label->hot_link = -1;
	label->pressed_link = -1;
}

/* Only plain text is selectable: formatted (Markdown) text keeps its links. */
static int rvn_label_can_select(const rvn_label_t* label) {
	return label->selectable && !label->markdown;
}

static RCursor rvn_label_cursor(const rvn_label_t* label) {
	return rvn_label_can_select(label) ? R_CURSOR_IBEAM : (RCursor)0;
}

/* The mouse goes to the label when it has links to click or text to select. */
static void rvn_label_update_hit(rvn_label_t* label) {
	int hit = label->markdown ? (label->md && rvn_md_link_count(label->md) > 0) : label->selectable;
	if (hit) label->base.flags &= ~RVN_WF_NO_HIT;
	else label->base.flags |= RVN_WF_NO_HIT;
}

static void rvn_label_clear_selection(rvn_label_t* label) {
	label->sel_anchor = label->sel_caret = 0;
	label->selecting = R_FALSE;
}

static void rvn_label_forget(rvn_label_t* label) {
	rvn_md_free(label->md);
	label->md = NULL;
	label->hot_link = -1;
	label->pressed_link = -1;
	label->base.cursor = rvn_label_cursor(label);
	/* (the offsets of the selection belong to the old text) */
	rvn_label_clear_selection(label);
}

static void rvn_label_destroy(rvn_widget_t* widget) {
	rvn_label_t* label = (rvn_label_t*)widget;
	rvn_md_free(label->md);
	rvn_free(label->text);
	rvn_free(label->plain);
}

static int rvn_label_wraps(const rvn_label_t* label) {
	/* formatted text is made of paragraphs: it wraps unless told not to */
	return label->wrap_chosen ? label->wrap : (label->wrap || label->markdown);
}

static rvn_md_t* rvn_label_md(rvn_label_t* label) {
	if (!label->md) {
		label->md = rvn_md_parse(label->text);
		/* a label does not take the mouse, unless it has links to click */
		rvn_label_update_hit(label);
	}
	return label->md;
}

typedef void (*rvn_label_line_fn)(const char* line, int length, float width, int index, void* context);

/**
 * Splits the text into lines (explicit '\n' and, when `wrap_width` is
 * positive, word wrapping) and calls `visit` for each. Returns the number of
 * lines and the width of the widest one.
 */
static int rvn_label_lines(const rvn_label_t* label, rvn_font_t* font, float size, float wrap_width,
	float* max_width, rvn_label_line_fn visit, void* context) {
	const char* text = label->text ? label->text : "";
	int remaining = (int)strlen(text);
	int count = 0;
	float widest = 0.0f;

	do {
		float width = 0.0f;
		int consumed = rvn_text_wrap(font, size, text, remaining, wrap_width, &width);
		int length = consumed;
		/* the line itself excludes the newline and the spaces it was cut on */
		while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == ' ' || text[length - 1] == '\t')) length--;
		if (visit) visit(text, length, width, count, context);
		widest = rvn_maxf(widest, width);
		count++;
		/* a text ending with '\n' has one more, empty, line */
		if (consumed == remaining && consumed > 0 && text[consumed - 1] == '\n') {
			if (visit) visit(text + consumed, 0, 0.0f, count, context);
			count++;
		}
		text += consumed;
		remaining -= consumed;
	} while (remaining > 0);

	if (max_width) *max_width = widest;
	return count;
}

/* ---------------------------------------------------------------------------
 *  Selection
 * ------------------------------------------------------------------------- */

static int rvn_label_sel_min(const rvn_label_t* label) { return rvn_mini(label->sel_anchor, label->sel_caret); }
static int rvn_label_sel_max(const rvn_label_t* label) { return rvn_maxi(label->sel_anchor, label->sel_caret); }
static int rvn_label_has_selection(const rvn_label_t* label) { return label->sel_anchor != label->sel_caret; }

static float rvn_label_align(const rvn_label_t* label, int rtl) {
	float align = label->align == R_TEXT_ALIGN_CENTER ? 0.5f : (label->align == R_TEXT_ALIGN_RIGHT ? 1.0f : 0.0f);
	return rtl ? 1.0f - align : align;
}

typedef struct rvn_label_pick_t {
	const rvn_label_t* label;
	rvn_font_t* font;
	float size;
	float align;
	float x;
	int target;
	int offset;
} rvn_label_pick_t;

static void rvn_label_pick_line(const char* line, int length, float width, int index, void* context) {
	rvn_label_pick_t* pick = (rvn_label_pick_t*)context;
	float x = (pick->label->base.rect.w - width) * pick->align;
	int start = (int)(line - pick->label->text);
	/* the lines after the target are ignored: a point below the text lands in the last line */
	if (index > pick->target) return;
	pick->offset = length > 0 ? start + rvn_text_offset_at(pick->font, pick->size, line, length, pick->x - x) : start;
}

/* The offset in the text (a boundary between two characters) nearest to a point of the label. */
static int rvn_label_offset_at(const rvn_label_t* label, float x, float y) {
	rvn_font_t* font = rvn_font_resolve(label->font);
	rvn_font_metrics_t metrics;
	rvn_label_pick_t pick;
	float size;
	if (!font || !label->text) return 0;
	size = rvn_widget_font_size(&label->base, label->size);
	rvn_font_metrics(font, size, &metrics);
	pick.label = label;
	pick.font = font;
	pick.size = size;
	pick.align = rvn_label_align(label, label->rtl);
	pick.x = x;
	pick.target = metrics.line_height > 0.0f ? rvn_maxi(0, (int)floorf(y / metrics.line_height)) : 0;
	pick.offset = 0;
	rvn_label_lines(label, font, size, rvn_label_wraps(label) ? label->base.rect.w : 0.0f, NULL, rvn_label_pick_line, &pick);
	return rvn_clampi(pick.offset, 0, (int)strlen(label->text));
}

typedef struct rvn_label_highlight_t {
	const rvn_label_t* label;
	RPainter* painter;
	rvn_font_t* font;
	float size;
	float line_height;
	float align;
	int from;
	int to;
	rvn_color_t color;
} rvn_label_highlight_t;

static void rvn_label_highlight_line(const char* line, int length, float width, int index, void* context) {
	rvn_label_highlight_t* highlight = (rvn_label_highlight_t*)context;
	int start = (int)(line - highlight->label->text);
	int begin = rvn_maxi(highlight->from, start) - start;
	int end = rvn_mini(highlight->to, start + length) - start;
	float x, x0, x1;
	if (end <= begin) return;
	x = (highlight->label->base.rect.w - width) * highlight->align;
	x0 = rvn_text_width(highlight->font, highlight->size, line, begin);
	x1 = rvn_text_width(highlight->font, highlight->size, line, end);
	rvn_paint_rect(highlight->painter, rvn_rect(x + x0, highlight->line_height * (float)index, x1 - x0, highlight->line_height),
		highlight->color);
}

/* A double click selects the word under the pointer: the spaces and line breaks separate words. */
static int rvn_label_is_break(char c) {
	return c == ' ' || c == '\t' || c == '\n';
}

static void rvn_label_select_word(rvn_label_t* label, int offset) {
	const char* text = label->text ? label->text : "";
	int length = (int)strlen(text);
	int begin = rvn_mini(offset, length), end = begin;
	while (begin > 0 && !rvn_label_is_break(text[begin - 1])) begin--;
	while (end < length && !rvn_label_is_break(text[end])) end++;
	label->sel_anchor = begin;
	label->sel_caret = end;
}

static void rvn_label_copy(const rvn_label_t* label) {
	char* copy;
	if (!label->text || !rvn_label_has_selection(label)) return;
	copy = rvn_strndup(label->text + rvn_label_sel_min(label), (size_t)(rvn_label_sel_max(label) - rvn_label_sel_min(label)));
	Rclipboard_set_text(copy);
	rvn_free(copy);
}

static rvn_vec2_t rvn_label_measure(rvn_widget_t* widget) {
	rvn_label_t* label = (rvn_label_t*)widget;
	rvn_font_t* font = rvn_font_resolve(label->font);
	float size = rvn_widget_font_size(widget, label->size);
	rvn_font_metrics_t metrics;
	float width = 0.0f;
	int lines, wraps = rvn_label_wraps(label);

	if (!font) return rvn_vec2(0.0f, 0.0f);
	if (label->markdown) return rvn_md_layout(rvn_label_md(label), widget, label->font, size, wraps ? label->wrap_width : 0.0f);
	rvn_font_metrics(font, size, &metrics);
	lines = rvn_label_lines(label, font, size, wraps ? label->wrap_width : 0.0f, &width, NULL, NULL);
	/* a wrapped label keeps the width it was given, or it would be re-wrapped
	 * narrower and narrower by a parent that sizes it to its content */
	if (wraps && label->wrap_width > 0.0f && lines > 1) width = rvn_maxf(width, label->wrap_width);
	return rvn_vec2(ceilf(width), ceilf(metrics.line_height * (float)lines));
}

static void rvn_label_arrange(rvn_widget_t* widget) {
	rvn_label_t* label = (rvn_label_t*)widget;
	if (rvn_label_wraps(label) && fabsf(label->wrap_width - widget->rect.w) > 0.01f) {
		label->wrap_width = widget->rect.w;
		/* the height we announced was computed for another width */
		rvn_widget_invalidate_layout(widget);
	}
}

typedef struct rvn_label_paint_t {
	rvn_label_t* label;
	RPainter* painter;
	float size;
	float line_height;
	float align;
	rvn_color_t color;
} rvn_label_paint_t;

static void rvn_label_paint_line(const char* line, int length, float width, int index, void* context) {
	rvn_label_paint_t* paint = (rvn_label_paint_t*)context;
	float x = (paint->label->base.rect.w - width) * paint->align;
	if (length <= 0) return;
	rvn_paint_text(paint->painter, paint->label->font, paint->size, x, paint->line_height * (float)index,
		line, length, paint->color);
}

/* Where formatted text starts across, when it is narrower than the label. */
static float rvn_label_md_x(const rvn_label_t* label) {
	float align = label->align == R_TEXT_ALIGN_CENTER ? 0.5f : (label->align == R_TEXT_ALIGN_RIGHT ? 1.0f : 0.0f);
	return floorf(rvn_maxf(0.0f, label->base.rect.w - label->base.desired.x) * align);
}

static void rvn_label_paint(rvn_widget_t* widget, RPainter* painter) {
	rvn_label_t* label = (rvn_label_t*)widget;
	rvn_font_t* font = rvn_font_resolve(label->font);
	rvn_font_metrics_t metrics;
	rvn_label_paint_t paint;
	int clipped;

	if (!font || !label->text || !label->text[0]) return;
	/* text larger than the space it received is cut at the bounds of the label */
	clipped = widget->desired.x > widget->rect.w + 0.5f || widget->desired.y > widget->rect.h + 0.5f;
	if (label->markdown) {
		/* (laid out when it was measured) */
		if (!label->md) return;
		if (clipped) rvn_paint_push_clip(painter, rvn_rect(0.0f, 0.0f, widget->rect.w, widget->rect.h));
		rvn_md_paint(label->md, widget, painter,
			painter->rtl ? rvn_maxf(0.0f, widget->rect.w - widget->desired.x) - rvn_label_md_x(label) : rvn_label_md_x(label), 0.0f, rvn_widget_text_color(widget, painter),
			painter->enabled ? label->hot_link : -1);
		if (clipped) rvn_paint_pop_clip(painter);
		return;
	}
	paint.label = label;
	paint.painter = painter;
	paint.size = rvn_widget_font_size(widget, label->size);
	rvn_font_metrics(font, paint.size, &metrics);
	paint.line_height = metrics.line_height;
	paint.align = label->align == R_TEXT_ALIGN_CENTER ? 0.5f : (label->align == R_TEXT_ALIGN_RIGHT ? 1.0f : 0.0f);
	if (painter->rtl) paint.align = 1.0f - paint.align;
	paint.color = rvn_widget_text_color(widget, painter);
	label->rtl = painter->rtl;

	if (clipped) rvn_paint_push_clip(painter, rvn_rect(0.0f, 0.0f, widget->rect.w, widget->rect.h));
	/* the selection is drawn under the text, only while the label has the focus (the mouse gave it) */
	if (rvn_label_can_select(label) && rvn_label_has_selection(label) && (widget->flags & RVN_WF_FOCUSED)) {
		rvn_label_highlight_t highlight;
		highlight.label = label;
		highlight.painter = painter;
		highlight.font = font;
		highlight.size = paint.size;
		highlight.line_height = paint.line_height;
		highlight.align = paint.align;
		highlight.from = rvn_label_sel_min(label);
		highlight.to = rvn_label_sel_max(label);
		highlight.color = rvn_widget_color(widget, R_COLOR_SELECTION);
		rvn_label_lines(label, font, paint.size, rvn_label_wraps(label) ? widget->rect.w : 0.0f, NULL, rvn_label_highlight_line, &highlight);
	}
	rvn_label_lines(label, font, paint.size, rvn_label_wraps(label) ? widget->rect.w : 0.0f, NULL, rvn_label_paint_line, &paint);
	if (clipped) rvn_paint_pop_clip(painter);
}

static void rvn_label_set_hot(rvn_widget_t* widget, int link) {
	rvn_label_t* label = (rvn_label_t*)widget;
	if (label->hot_link == link) return;
	label->hot_link = link;
	widget->cursor = link >= 0 ? R_CURSOR_HAND : (RCursor)0;
	rvn_widget_invalidate_paint(widget);
}

static void rvn_label_follow(rvn_widget_t* widget, int link) {
	rvn_label_t* label = (rvn_label_t*)widget;
	const char* address = rvn_md_link(label->md, link);
	char* url;
	if (!address) return;
	/* (the callback may change the text, and the links with it) */
	url = rvn_strdup(address);
	if (label->link_callback) {
		label->link_callback(widget->id, url, label->link_user);
	} else if (rvn_strnicmp(url, "action:", 7) == 0) {
		if (url[7]) rvn_fire_action(widget->id, url + 7);
	} else if (rvn_strnicmp(url, "http://", 7) == 0 || rvn_strnicmp(url, "https://", 8) == 0 || rvn_strnicmp(url, "mailto:", 7) == 0) {
		Ropen_url(url);
	}
	rvn_free(url);
}

/* Formatted text: the links. */
static int rvn_label_link_event(rvn_widget_t* widget, const REvent* event) {
	rvn_label_t* label = (rvn_label_t*)widget;
	int link;
	if (!label->markdown || !label->md) return R_FALSE;
	switch (event->type) {
		case R_EVENT_MOUSE_MOVE:
			rvn_label_set_hot(widget, rvn_md_link_at(label->md, event->local_x - rvn_label_md_x(label), event->local_y));
			return R_FALSE;
		case R_EVENT_MOUSE_LEAVE:
			rvn_label_set_hot(widget, -1);
			return R_FALSE;
		case R_EVENT_MOUSE_DOWN:
			if (event->button != R_MOUSE_LEFT) return R_FALSE;
			link = rvn_md_link_at(label->md, event->local_x - rvn_label_md_x(label), event->local_y);
			label->pressed_link = link;
			/* (text that is not a link lets the click go to what holds the label) */
			return link >= 0;
		case R_EVENT_MOUSE_UP:
			if (event->button != R_MOUSE_LEFT) return R_FALSE;
			link = rvn_md_link_at(label->md, event->local_x - rvn_label_md_x(label), event->local_y);
			if (link >= 0 && link == label->pressed_link) {
				label->pressed_link = -1;
				rvn_label_follow(widget, link);
				return R_TRUE;
			}
			label->pressed_link = -1;
			return R_FALSE;
		default:
			return R_FALSE;
	}
}

/* Plain text that can be selected: the mouse, then Ctrl+C (Cmd+C) copies it. */
static int rvn_label_select_event(rvn_widget_t* widget, const REvent* event) {
	rvn_label_t* label = (rvn_label_t*)widget;
	int offset;
	switch (event->type) {
		case R_EVENT_MOUSE_DOWN:
			if (event->button != R_MOUSE_LEFT) return R_FALSE;
			offset = rvn_label_offset_at(label, event->local_x, event->local_y);
			if (event->clicks >= 3) {
				label->sel_anchor = 0;
				label->sel_caret = label->text ? (int)strlen(label->text) : 0;
			} else if (event->clicks == 2) {
				rvn_label_select_word(label, offset);
			} else {
				label->sel_caret = offset;
				if (!(event->modifiers & R_MOD_SHIFT)) label->sel_anchor = offset;
				label->selecting = R_TRUE;
			}
			/* a label is not a stop of the Tab key: it takes the focus when it is clicked, to receive Ctrl+C */
			widget->flags |= RVN_WF_FOCUSABLE;
			rvn_widget_focus(widget);
			rvn_widget_capture_mouse(widget);
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		case R_EVENT_MOUSE_MOVE:
			if (!label->selecting) return R_FALSE;
			label->sel_caret = rvn_label_offset_at(label, event->local_x, event->local_y);
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		case R_EVENT_MOUSE_UP:
			if (event->button != R_MOUSE_LEFT) return R_FALSE;
			label->selecting = R_FALSE;
			rvn_widget_release_mouse(widget);
			return R_TRUE;
		case R_EVENT_FOCUS_LOST:
			rvn_label_clear_selection(label);
			widget->flags &= ~RVN_WF_FOCUSABLE;
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		case R_EVENT_KEY_DOWN:
			if (!(widget->flags & RVN_WF_FOCUSED) || !(event->modifiers & (R_MOD_CTRL | R_MOD_SUPER))) return R_FALSE;
			if (event->key == R_KEY_C) {
				rvn_label_copy(label);
				return R_TRUE;
			}
			if (event->key == R_KEY_A) {
				label->sel_anchor = 0;
				label->sel_caret = label->text ? (int)strlen(label->text) : 0;
				rvn_widget_invalidate_paint(widget);
				return R_TRUE;
			}
			return R_FALSE;
		default:
			return R_FALSE;
	}
}

static int rvn_label_event(rvn_widget_t* widget, const REvent* event) {
	if (rvn_label_can_select((rvn_label_t*)widget)) return rvn_label_select_event(widget, event);
	return rvn_label_link_event(widget, event);
}

static void rvn_label_set_markdown(rvn_widget_t* widget, int enabled) {
	rvn_label_t* label = (rvn_label_t*)widget;
	enabled = enabled != 0;
	if (label->markdown == enabled) return;
	label->markdown = enabled;
	rvn_label_forget(label);
	if (!enabled) rvn_label_update_hit(label);
	rvn_widget_invalidate_layout(widget);
}

static void rvn_label_set_selectable(rvn_widget_t* widget, int enabled) {
	rvn_label_t* label = (rvn_label_t*)widget;
	enabled = enabled != 0;
	if (label->selectable == enabled) return;
	label->selectable = enabled;
	if (!enabled) {
		/* (the focus the label took when clicked goes with it) */
		Rwidget_set_focusable(widget->id, R_FALSE);
		rvn_widget_release_mouse(widget);
	}
	rvn_label_clear_selection(label);
	/* Markdown text is not selectable: the flag is kept for when it is turned off */
	if (!label->markdown) {
		rvn_label_update_hit(label);
		widget->cursor = rvn_label_cursor(label);
	}
	rvn_widget_invalidate_paint(widget);
}

static int rvn_label_set_attribute(rvn_widget_t* widget, const char* name, const char* value) {
	rvn_label_t* label = (rvn_label_t*)widget;
	rvn_color_t color;
	int flag;
	if (RVN_ATTR("Text")) {
		if (rvn_str_set(&label->text, value)) rvn_label_forget(label);
	} else if (RVN_ATTR("Font")) {
		Rid font = rvn_xml_font(value);
		if (font == R_INVALID_ID) return R_FALSE;
		label->font = font;
	} else if (RVN_ATTR("FontSize") || RVN_ATTR("TextSize")) {
		if (!rvn_parse_float(value, &label->size)) return R_FALSE;
	} else if (RVN_ATTR("Align") || RVN_ATTR("TextAlign")) {
		if (rvn_stricmp(value, "Left") == 0) label->align = R_TEXT_ALIGN_LEFT;
		else if (rvn_stricmp(value, "Center") == 0) label->align = R_TEXT_ALIGN_CENTER;
		else if (rvn_stricmp(value, "Right") == 0) label->align = R_TEXT_ALIGN_RIGHT;
		else return R_FALSE;
	} else if (RVN_ATTR("Wrap")) {
		if (!rvn_parse_bool(value, &label->wrap)) return R_FALSE;
		label->wrap_chosen = R_TRUE;
	} else if (RVN_ATTR("Markdown")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		rvn_label_set_markdown(widget, flag);
	} else if (RVN_ATTR("Selectable")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		rvn_label_set_selectable(widget, flag);
	} else if (RVN_ATTR("Color")) {
		if (!rvn_parse_color(value, &color)) return R_FALSE;
		rvn_widget_set_color(widget, R_COLOR_TEXT, color);
	} else {
		return R_FALSE;
	}
	rvn_widget_invalidate_layout(widget);
	return R_TRUE;
}

const rvn_widget_class_t rvn_class_label = {
	.type = R_WIDGET_LABEL,
	.name = "Label",
	.size = sizeof(rvn_label_t),
	.flags = RVN_CF_NO_HIT | RVN_CF_NO_CHILDREN,
	.init = rvn_label_init,
	.destroy = rvn_label_destroy,
	.measure = rvn_label_measure,
	.arrange = rvn_label_arrange,
	.paint = rvn_label_paint,
	.event = rvn_label_event,
	.set_attribute = rvn_label_set_attribute
};

/* ---------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------- */

Rid Rlabel_create(const char* text) {
	rvn_widget_t* widget;
	RVN_API_GUARD(R_INVALID_ID);
	widget = rvn_widget_create(R_WIDGET_LABEL);
	if (!widget) return R_INVALID_ID;
	((rvn_label_t*)widget)->text = rvn_strdup(text);
	return widget->id;
}

void Rlabel_set_text(Rid id, const char* text) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	if (rvn_str_set(&((rvn_label_t*)widget)->text, text)) {
		rvn_label_forget((rvn_label_t*)widget);
		rvn_widget_invalidate_layout(widget);
	}
}

const char* Rlabel_get_text(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_LABEL, "");
	return ((rvn_label_t*)widget)->text ? ((rvn_label_t*)widget)->text : "";
}

void Rlabel_set_font(Rid id, Rid font) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	if (font != R_INVALID_ID && !Rfont_is_valid(font)) {
		rvn_error(R_ERROR_INVALID_ID, R_SEVERITY_ERROR, "Rlabel_set_font: invalid font");
		return;
	}
	((rvn_label_t*)widget)->font = font;
	rvn_widget_invalidate_layout(widget);
}

void Rlabel_set_text_size(Rid id, float size) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	((rvn_label_t*)widget)->size = size > 0.0f ? size : 0.0f;
	rvn_widget_invalidate_layout(widget);
}

void Rlabel_set_text_align(Rid id, RTextAlign align) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	if (align < R_TEXT_ALIGN_LEFT || align > R_TEXT_ALIGN_RIGHT) {
		rvn_error(R_ERROR_INVALID_ENUM, R_SEVERITY_ERROR, "Rlabel_set_text_align: invalid alignment");
		return;
	}
	((rvn_label_t*)widget)->align = align;
	rvn_widget_invalidate_paint(widget);
}

void Rlabel_set_wrap(Rid id, int wrap) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	((rvn_label_t*)widget)->wrap = wrap != 0;
	((rvn_label_t*)widget)->wrap_chosen = R_TRUE;
	rvn_widget_invalidate_layout(widget);
}

void Rlabel_set_color(Rid id, float r, float g, float b, float a) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	rvn_widget_set_color(widget, R_COLOR_TEXT, rvn_rgba(r, g, b, a));
}

void Rlabel_set_markdown(Rid id, int enabled) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	rvn_label_set_markdown(widget, enabled);
}

int Rlabel_get_markdown(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_LABEL, R_FALSE);
	return ((rvn_label_t*)widget)->markdown;
}

void Rlabel_set_selectable(Rid id, int selectable) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	rvn_label_set_selectable(widget, selectable);
}

int Rlabel_is_selectable(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_LABEL, R_FALSE);
	return ((rvn_label_t*)widget)->selectable;
}

void Rlabel_set_link_callback(Rid id, RLabelLinkCallback callback, void* user_data) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_LABEL);
	((rvn_label_t*)widget)->link_callback = callback;
	((rvn_label_t*)widget)->link_user = user_data;
}

const char* Rlabel_get_plain_text(Rid id) {
	rvn_label_t* label;
	rvn_text_t out;
	RVN_WIDGET(widget, id, R_WIDGET_LABEL, "");
	label = (rvn_label_t*)widget;
	if (!label->markdown) return label->text ? label->text : "";
	memset(&out, 0, sizeof(out));
	rvn_md_plain_text(rvn_label_md(label), &out);
	rvn_free(label->plain);
	label->plain = out.data;
	return label->plain ? label->plain : "";
}
