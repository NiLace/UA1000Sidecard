/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * desk.c - the panel as the renders of gui/scene draw it
 *
 * The controls are the plain layout's GTK widgets (mixer.c), so keyboard
 * handling, accessibility and the wiring to the state do not change: here
 * they are made transparent and placed over a render of the panel.  One
 * drawing area on top, which takes no input, paints the panel: the plate,
 * the box of every lit button from a second render with everything lit, the
 * fader caps at their scales' values, the pan pointers, the rate display and
 * the keyboard focus.
 *
 * Every position comes from desk.layout, written by gui/scene/make_assets.sh
 * from the render's own projection; nothing is measured off an image.
 *
 * Data: one struct desk per window (app->desk): the parsed layout, the
 * images, the controls placed over them and the meters' state, all in the
 * panel's 1x pixels.  The panel is one GtkFixed of that size, scaled to the
 * window as a whole (scaler_new()), so nothing else here depends on the
 * window's size.
 *
 * Invariants: desk_new() refuses a layout without every fader, knob, cap,
 * button and the rate display, so the code that places and paints them
 * never checks for one missing; meters and the status zone are optional.
 * The controls hold no settings: the paint reads the widgets' values, and
 * the desk's own gestures (fader caps, knobs, the rate display) only set
 * those values or call the toolbar's and dialogs' functions.
 *
 * It does not talk to the device or edit the state directly; meter levels
 * come from meters.c, through JACK.  Used by app.c (desk_new(),
 * desk_refresh(), desk_detach(), desk_free(), desk_natural_size()) and
 * toolbar.c (desk_redraw()).
 */
#include "app.h"
#include "meters.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Room for desk.layout's buttons; has_all_buttons() lists those required. */
#define DESK_MAX_BUTTONS 64
/* Cells of the rate display. */
#define DESK_MAX_DIGITS 4

/* The fader cap images: input mixer, output mixer, monitor. */
enum desk_group { GROUP_IN, GROUP_OUT, GROUP_MON, GROUP_COUNT };

/* A rectangle in the panel's 1x pixels: top-left, bottom-right. */
struct desk_box {
	double x0, y0, x1, y1;
};

/* A fader cap: one image per group, placed by its index line. */
struct desk_cap {
	double w, h, ax, ay; /* size and the index line's centre in it */
	cairo_surface_t *image;
};

/* A fader's travel: the index line's centre at the bottom and the top. */
struct desk_fader {
	bool found;
	double xb, yb, xt, yt;
};

/* A pan knob on the panel: its centre and outer radius. */
struct desk_knob {
	bool found;
	double cx, cy, r;
	GtkWidget *hit; /* takes drags, scrolls and keys for the pan scale */
};

/*
 * A button's box, by its desk.layout id, and the control placed over it
 * (NULL until place_button() or place_device() runs).
 */
struct desk_button {
	char id[24];
	struct desk_box box;
	GtkWidget *widget;
};

/* A fader's meter: its well, and what it shows (dBFS). */
struct desk_meter {
	bool found;
	struct desk_box box;
	GtkWidget *area;
	double level; /* the bar, falling at METER_FALL_DB_S */
	double hold; /* the held peak, for METER_HOLD_US */
	gint64 hold_until, clip_until;
	bool available;
};

/* The meters' scale runs from METER_FLOOR_DB to 0 dBFS. */
#define METER_FLOOR_DB (-60.0)
#define METER_FALL_DB_S 12.0 /* a peak programme meter's fall, about */
#define METER_HOLD_US G_GINT64_CONSTANT(1500000)
#define METER_CLIP_US G_GINT64_CONSTANT(2000000)
#define METER_PERIOD_MS 33

/*
 * The seven-segment rate display: a digit's size (dw, dh), the segments'
 * thickness t and slant (skew), their lit colour, the decimal point, and
 * the n cells' positions.
 */
struct desk_digits {
	bool found;
	double dw, dh, t, skew;
	double rgb[3];
	double dp[3]; /* the decimal point: x, y, radius */
	size_t n;
	double x[DESK_MAX_DIGITS], y[DESK_MAX_DIGITS]; /* bottom-left corners */
};

/*
 * One window's desk: the layout and images, the controls placed over them
 * and the meters' state.  All coordinates are the panel's 1x pixels.
 */
struct desk {
	struct app *app;
	double width, height;
	cairo_surface_t *plate, *lit;
	struct desk_cap cap[GROUP_COUNT];
	struct desk_fader fader[2][UA1000_CHANNELS];
	struct desk_fader monitor[2];
	struct desk_knob knob[2][UA1000_CHANNELS];
	struct desk_button button[DESK_MAX_BUTTONS];
	size_t n_buttons;
	struct desk_digits digits;
	struct desk_meter meter[2][UA1000_CHANNELS];
	struct desk_meter monitor_meter[2];
	struct meters *meters; /* NULL: no audio server to read */
	guint meter_source;
	gint64 meter_last;
	bool meters_shown_live; /* the wells' state as last shown */
	unsigned int meter_ticks;
	double pointer_rgb[3]; /* the knobs' index line, as the render has it */
	GtkWidget *paint;
	GtkWidget *clock[3], *dout[2], *mono, *soft, *all;
	GtkWidget
		*rate; /* the display's button: the USER SET needs the device */
	struct desk_box
		status; /* the zone the connection status is written in */
	bool have_status;
	double backdrop[3]; /* the render's backdrop round the panel */
};

/* ---- Loading ---- */

/*
 * Parse "<prefix>N", N = 1..UA1000_CHANNELS, at the start of id: *channel
 * gets N - 1 and *rest points past the number.  False when id does not
 * start that way.
 */
static bool parse_channel(const char *id, const char *prefix,
			  unsigned int *channel, const char **rest)
{
	size_t len = strlen(prefix);
	char *end;
	unsigned long n;

	if (strncmp(id, prefix, len) != 0)
		return false;
	n = strtoul(id + len, &end, 10);
	if (end == id + len || n < 1 || n > UA1000_CHANNELS)
		return false;
	*channel = (unsigned int)n - 1;
	*rest = end;
	return true;
}

/* "RRGGBB" (six hex digits, no '#') as cairo's 0..1 components; false and
 * rgb untouched on anything else. */
static bool hex_rgb(const char *hex, double rgb[3])
{
	char *end;
	unsigned long v;

	if (strlen(hex) != 6)
		return false;
	errno = 0;
	v = strtoul(hex, &end, 16);
	if (errno || *end)
		return false;
	rgb[0] = (double)((v >> 16) & 0xff) / 255.0;
	rgb[1] = (double)((v >> 8) & 0xff) / 255.0;
	rgb[2] = (double)(v & 0xff) / 255.0;
	return true;
}

/* Every token from tok[0] on is a finite number. */
static bool numbers(char *const *tok, size_t count, double *out)
{
	for (size_t i = 0; i < count; i++) {
		char *end;

		errno = 0;
		out[i] = strtod(tok[i], &end);
		if (errno || end == tok[i] || *end || !isfinite(out[i]))
			return false;
	}
	return true;
}

/* The longest line: a digits line with DESK_MAX_DIGITS cells. */
#define DESK_MAX_TOKENS (12 + 2 * DESK_MAX_DIGITS)

/*
 * Split line in place on blanks into tok[]: the number of tokens, or
 * DESK_MAX_TOKENS + 1 for a line with more than tok holds (no line
 * kind accepts that count, so the line is refused).
 */
static size_t split(char *line, char **tok)
{
	char *save = NULL;
	size_t n = 0;

	for (char *t = strtok_r(line, " \t\n", &save); t;
	     t = strtok_r(NULL, " \t\n", &save)) {
		if (n == DESK_MAX_TOKENS)
			return DESK_MAX_TOKENS + 1; /* too many: refused */
		tok[n++] = t;
	}
	return n;
}

/*
 * digits ID DW DH T SKEW SEG OFF DPX DPY DPR N X Y ...
 *
 * A digit's size, the segments' thickness and slant, the lit colour, the
 * decimal point (centre, radius) and N cells' bottom-left corners; ID and
 * OFF are not read.  The token count must agree with N.
 */
static bool parse_digits(struct desk *d, char **tok, size_t n)
{
	struct desk_digits *g = &d->digits;
	double v[4], cells[2 * DESK_MAX_DIGITS], count;

	if (n < 12 || !numbers(tok + 2, 4, v) || !hex_rgb(tok[6], g->rgb) ||
	    !numbers(tok + 8, 3, g->dp) || !numbers(tok + 11, 1, &count) ||
	    count < 1 || count > DESK_MAX_DIGITS ||
	    n != 12 + 2 * (size_t)count ||
	    !numbers(tok + 12, 2 * (size_t)count, cells))
		return false;
	g->dw = v[0];
	g->dh = v[1];
	g->t = v[2];
	g->skew = v[3];
	g->n = (size_t)count;
	for (size_t i = 0; i < g->n; i++) {
		g->x[i] = cells[2 * i];
		g->y[i] = cells[2 * i + 1];
	}
	g->found = true;
	return true;
}

/* A fader by its id: "in.N", "out.N", "mon.1" or "mon.2"; else NULL. */
static struct desk_fader *fader_of(struct desk *d, const char *id)
{
	unsigned int ch;
	const char *rest;

	if (parse_channel(id, "in.", &ch, &rest) && !*rest)
		return &d->fader[UA1000_MIXER_INPUT][ch];
	if (parse_channel(id, "out.", &ch, &rest) && !*rest)
		return &d->fader[UA1000_MIXER_OUTPUT][ch];
	if (parse_channel(id, "mon.", &ch, &rest) && !*rest && ch < 2)
		return &d->monitor[ch];
	return NULL;
}

/* A fader's meter, by the fader's id. */
static struct desk_meter *meter_of(struct desk *d, const char *id)
{
	unsigned int ch;
	const char *rest;

	if (parse_channel(id, "in.", &ch, &rest) && !*rest)
		return &d->meter[UA1000_MIXER_INPUT][ch];
	if (parse_channel(id, "out.", &ch, &rest) && !*rest)
		return &d->meter[UA1000_MIXER_OUTPUT][ch];
	if (parse_channel(id, "mon.", &ch, &rest) && !*rest && ch < 2)
		return &d->monitor_meter[ch];
	return NULL;
}

/* A pan knob by its id, "in.N.pan" or "out.N.pan"; else NULL. */
static struct desk_knob *knob_of(struct desk *d, const char *id)
{
	unsigned int ch;
	const char *rest;

	if (parse_channel(id, "in.", &ch, &rest) && strcmp(rest, ".pan") == 0)
		return &d->knob[UA1000_MIXER_INPUT][ch];
	if (parse_channel(id, "out.", &ch, &rest) && strcmp(rest, ".pan") == 0)
		return &d->knob[UA1000_MIXER_OUTPUT][ch];
	return NULL;
}

/*
 * One desk.layout line into d.  A blank line is accepted.  False refuses
 * the whole layout: an unknown kind, a wrong token count, a bad number, an
 * id that names nothing, or a button past DESK_MAX_BUTTONS.  Only zones
 * of an unknown name are let pass.
 */
static bool parse_line(struct desk *d, char *line)
{
	static const char *const groups[] = { "in", "out", "mon" };
	char *tok[DESK_MAX_TOKENS];
	size_t n = split(line, tok);
	double v[4];

	if (n == 0)
		return true;
	if (strcmp(tok[0], "size") == 0 && n == 3 && numbers(tok + 1, 2, v)) {
		d->width = v[0];
		d->height = v[1];
		return true;
	}
	if (strcmp(tok[0], "fader") == 0 && n == 6 && numbers(tok + 2, 4, v)) {
		struct desk_fader *f = fader_of(d, tok[1]);

		if (!f)
			return false;
		*f = (struct desk_fader){ true, v[0], v[1], v[2], v[3] };
		return true;
	}
	if (strcmp(tok[0], "cap") == 0 && n == 6 && numbers(tok + 2, 4, v)) {
		for (int g = 0; g < GROUP_COUNT; g++) {
			if (strcmp(tok[1], groups[g]) == 0) {
				d->cap[g].w = v[0];
				d->cap[g].h = v[1];
				d->cap[g].ax = v[2];
				d->cap[g].ay = v[3];
				return true;
			}
		}
		return false;
	}
	if (strcmp(tok[0], "button") == 0 && n == 6 && numbers(tok + 2, 4, v) &&
	    d->n_buttons < DESK_MAX_BUTTONS &&
	    strlen(tok[1]) < sizeof(d->button[0].id)) {
		struct desk_button *b = &d->button[d->n_buttons++];

		snprintf(b->id, sizeof(b->id), "%s", tok[1]);
		b->box = (struct desk_box){ v[0], v[1], v[2], v[3] };
		return true;
	}
	if (strcmp(tok[0], "knob") == 0 && n == 5 && numbers(tok + 2, 3, v)) {
		struct desk_knob *k = knob_of(d, tok[1]);

		if (!k)
			return false;
		k->found = true;
		k->cx = v[0];
		k->cy = v[1];
		k->r = v[2];
		return true;
	}
	if (strcmp(tok[0], "meter") == 0 && n == 6 && numbers(tok + 2, 4, v)) {
		struct desk_meter *mt = meter_of(d, tok[1]);

		if (!mt)
			return false;
		mt->found = true;
		mt->box = (struct desk_box){ v[0], v[1], v[2], v[3] };
		return true;
	}
	if (strcmp(tok[0], "zone") == 0 && n == 6 && numbers(tok + 2, 4, v)) {
		if (strcmp(tok[1], "status") == 0) {
			d->status = (struct desk_box){ v[0], v[1], v[2], v[3] };
			d->have_status = true;
		}
		return true; /* a zone this GUI does not know: left bare */
	}
	if (strcmp(tok[0], "pointer") == 0 && n == 2)
		return hex_rgb(tok[1], d->pointer_rgb);
	if (strcmp(tok[0], "digits") == 0)
		return parse_digits(d, tok, n);
	return false;
}

/* The button with this id, or NULL. */
static struct desk_button *button_box(struct desk *d, const char *id)
{
	for (size_t i = 0; i < d->n_buttons; i++)
		if (strcmp(d->button[i].id, id) == 0)
			return &d->button[i];
	return NULL;
}

/* Every button the desk places, so a layout that lacks one is refused
 * before any control is made. */
static bool has_all_buttons(struct desk *d)
{
	static const char *const master[] = {
		"clock.int",  "clock.spdif", "clock.adat", "dout.adat",
		"dout.spdif", "panel.mono",  "panel.soft", "panel.all",
		"mem.load",   "mem.store",   "patchbay",   "mon.link1",
		"notes",      "device"
	};
	static const char *const mixers[] = { "in", "out" };
	char id[24];

	for (size_t i = 0; i < G_N_ELEMENTS(master); i++)
		if (!button_box(d, master[i]))
			return false;
	for (int m = 0; m < 2; m++) {
		for (unsigned int ch = 1; ch <= UA1000_CHANNELS; ch++) {
			snprintf(id, sizeof(id), "%s.%u.mute", mixers[m], ch);
			if (!button_box(d, id))
				return false;
			snprintf(id, sizeof(id), "%s.%u.solo", mixers[m], ch);
			if (!button_box(d, id))
				return false;
		}
		for (unsigned int pair = 1; pair <= UA1000_PAIRS; pair++) {
			snprintf(id, sizeof(id), "%s.link%u", mixers[m], pair);
			if (!button_box(d, id))
				return false;
		}
	}
	return true;
}

/*
 * Read desk.layout into d.  True only when every line parses and the
 * size, the rate display, every strip's fader and knob, both monitor
 * faders, the caps' sizes and every button are present: the placing and
 * painting code relies on that and checks none of them.  Meters and the
 * status zone are optional.
 */
static bool load_layout(struct desk *d, const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	bool ok = true;

	if (!f)
		return false;
	while (ok && fgets(line, sizeof(line), f))
		ok = parse_line(d, line);
	fclose(f);
	if (!ok || d->width <= 0 || d->height <= 0 || !d->digits.found)
		return false;
	for (int m = 0; m < 2; m++)
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
			if (!d->fader[m][ch].found || !d->knob[m][ch].found)
				return false;
	for (int g = 0; g < GROUP_COUNT; g++)
		if (d->cap[g].w <= 0 || d->cap[g].h <= 0)
			return false; /* a 0-wide fader would be placed */
	return d->monitor[0].found && d->monitor[1].found && has_all_buttons(d);
}

/* dir/name through app_load_image(); NULL when missing or unreadable. */
static cairo_surface_t *load_image(const char *dir, const char *name,
				   double scale)
{
	char *path = g_build_filename(dir, name, NULL);
	cairo_surface_t *s = app_load_image(path, scale);

	g_free(path);
	return s;
}

/*
 * The plate, the all-lit render and the three caps, from dir/2x or else
 * dir/1x.  False when any is missing; what was loaded stays in d for
 * desk_free().
 */
static bool load_images(struct desk *d, const char *dir)
{
	static const char *const caps[] = { "cap_in.png", "cap_out.png",
					    "cap_mon.png" };
	char sub[4096];
	double scale = 2.0;

	/* The 2x set looks right on every screen; the 1x set is the
	 * fallback. */
	snprintf(sub, sizeof(sub), "%s/2x", dir);
	if (!g_file_test(sub, G_FILE_TEST_IS_DIR)) {
		snprintf(sub, sizeof(sub), "%s/1x", dir);
		scale = 1.0;
	}
	d->plate = load_image(sub, "plate.jpg", scale);
	d->lit = load_image(sub, "lit.jpg", scale);
	if (!d->plate || !d->lit)
		return false;
	for (int g = 0; g < GROUP_COUNT; g++) {
		d->cap[g].image = load_image(sub, caps[g], scale);
		if (!d->cap[g].image)
			return false;
	}
	return true;
}

/* ---- Painting ---- */

/* The cap image of a strip's mixer. */
static enum desk_group group_of(enum ua1000_mixer m)
{
	return m == UA1000_MIXER_INPUT ? GROUP_IN : GROUP_OUT;
}

/* The index line's centre for a fader at value 0..UA1000_FADER_MAX. */
static void fader_point(const struct desk_fader *f, double value, double *x,
			double *y)
{
	double frac = value / UA1000_FADER_MAX;

	*x = f->xb + (f->xt - f->xb) * frac;
	*y = f->yb + (f->yt - f->yb) * frac;
}

/*
 * A fader's cap at its scale's value, on whole pixels, and the amber focus
 * ring round the cap while the scale has visible focus (paint_focus()
 * leaves scales to this function: their widget spans the whole travel).
 */
static void paint_cap(cairo_t *cr, const struct desk_cap *cap,
		      const struct desk_fader *f, GtkWidget *scale)
{
	double x, y;

	fader_point(f, gtk_range_get_value(GTK_RANGE(scale)), &x, &y);
	cairo_set_source_surface(cr, cap->image, round(x - cap->ax),
				 round(y - cap->ay));
	cairo_paint(cr);
	if (gtk_widget_has_visible_focus(scale)) {
		cairo_set_source_rgba(cr, 0.85, 0.54, 0.23, 0.9);
		cairo_set_line_width(cr, 2.0);
		cairo_rectangle(cr, round(x - cap->ax) - 2,
				round(y - cap->ay) - 2, cap->w + 4, cap->h + 4);
		cairo_stroke(cr);
	}
}

/* The knob's index: a line on the cap, 0.10..0.92 of its radius, in the
 * render's colour.  It turns from -135 degrees (hard left) through the top
 * (centre) to +135 degrees (hard right). */
static void paint_pointer(cairo_t *cr, const struct desk *d,
			  const struct desk_knob *k, GtkWidget *pan)
{
	double pos = gtk_range_get_value(GTK_RANGE(pan));
	double norm = (pos + UA1000_PAN_STEPS) / (2.0 * UA1000_PAN_STEPS);
	double a = (-135.0 + 270.0 * norm) * G_PI / 180.0;
	double cap_r =
		k->r / 1.12 * 0.80; /* desk_scene.py: skirt 1.12, cap 0.80 */

	cairo_set_source_rgba(cr, d->pointer_rgb[0], d->pointer_rgb[1],
			      d->pointer_rgb[2], 0.95);
	cairo_set_line_width(cr, cap_r * 0.19);
	cairo_move_to(cr, k->cx + sin(a) * cap_r * 0.10,
		      k->cy - cos(a) * cap_r * 0.10);
	cairo_line_to(cr, k->cx + sin(a) * cap_r * 0.92,
		      k->cy - cos(a) * cap_r * 0.92);
	cairo_stroke(cr);
}

/*
 * desk_scene.py's seven_seg(): a bar from (ax, az) to (bx, bz) with pointed
 * ends, in the cell's own coordinates (z up), slanted by the skew.  It is
 * horizontal when az == bz, else vertical with a below b.  The path is
 * added to cr, for paint_digit() to fill.
 */
static void seg_bar(cairo_t *cr, const struct desk_digits *g, double x0,
		    double y0, double ax, double az, double bx, double bz)
{
	double t = g->t;
	double p[6][2];

	if (az == bz) {
		const double h[6][2] = {
			{ ax + t / 2, az },	{ ax + t, az - t / 2 },
			{ bx - t, bz - t / 2 }, { bx - t / 2, bz },
			{ bx - t, bz + t / 2 }, { ax + t, az + t / 2 }
		};

		memcpy(p, h, sizeof(p));
	} else {
		const double v[6][2] = {
			{ ax, az + t / 2 },	{ ax + t / 2, az + t },
			{ bx + t / 2, bz - t }, { bx, bz - t / 2 },
			{ bx - t / 2, bz - t }, { ax - t / 2, az + t }
		};

		memcpy(p, v, sizeof(p));
	}
	for (int i = 0; i < 6; i++) {
		double x = x0 + p[i][0] + g->skew * p[i][1];
		double y = y0 - p[i][1]; /* z up in the scene, y down here */

		if (i == 0)
			cairo_move_to(cr, x, y);
		else
			cairo_line_to(cr, x, y);
	}
	cairo_close_path(cr);
}

/*
 * The segments lit for c: a top, b and c right, d bottom, e and f left,
 * g middle.  '-' lights g; any other character, none.
 */
static const char *segments_of(char c)
{
	static const char *const digit[] = { "abcdef", "bc",   "abged",
					     "abgcd",  "fgbc", "afgcd",
					     "afgedc", "abc",  "abcdefg",
					     "abcfgd" };

	if (c >= '0' && c <= '9')
		return digit[c - '0'];
	return c == '-' ? "g" : "";
}

/* Fill c's segments in the cell whose bottom-left corner is (x0, y0), in
 * the source colour the caller set. */
static void paint_digit(cairo_t *cr, const struct desk_digits *g, double x0,
			double y0, char c)
{
	double w = g->dw, h = g->dh, hz = g->dh / 2;
	const char *on = segments_of(c);

	for (const char *s = on; *s; s++) {
		switch (*s) {
		case 'a':
			seg_bar(cr, g, x0, y0, 0, h, w, h);
			break;
		case 'g':
			seg_bar(cr, g, x0, y0, 0, hz, w, hz);
			break;
		case 'd':
			seg_bar(cr, g, x0, y0, 0, 0, w, 0);
			break;
		case 'f':
			seg_bar(cr, g, x0, y0, 0, hz, 0, h);
			break;
		case 'b':
			seg_bar(cr, g, x0, y0, w, hz, w, h);
			break;
		case 'e':
			seg_bar(cr, g, x0, y0, 0, 0, 0, hz);
			break;
		case 'c':
			seg_bar(cr, g, x0, y0, w, 0, w, hz);
			break;
		default:
			break;
		}
	}
	cairo_fill(cr);
}

/*
 * The rate display: the state's rate as three digits and the decimal
 * point, or "---" without the point for a rate ua1000_rate_hz() does not
 * know.  Characters past the layout's cells are not drawn.
 */
static void paint_rate(cairo_t *cr, const struct desk *d)
{
	const struct desk_digits *g = &d->digits;
	unsigned int hz = ua1000_rate_hz(d->app->state.panel.rate);
	char text[16]; /* room for any unsigned, as the compiler sees it */
	size_t k = 0;

	/* Always two digits, the point, one digit: 44.1, 48.0, 88.2, 96.0. */
	if (hz)
		snprintf(text, sizeof(text), "%2u%u", hz / 1000,
			 (hz % 1000) / 100);
	else
		snprintf(text, sizeof(text), "---");
	cairo_set_source_rgb(cr, g->rgb[0], g->rgb[1], g->rgb[2]);
	for (size_t i = 0; text[i] && k < g->n; i++, k++)
		paint_digit(cr, g, g->x[k], g->y[k], text[i]);
	if (hz) {
		cairo_arc(cr, g->dp[0], g->dp[1], g->dp[2], 0, 2 * G_PI);
		cairo_fill(cr);
	}
}

/* The amber ring round the focused control, in the coordinates of the
 * desk's fixed; nothing for a focus outside the desk. */
static void paint_focus(cairo_t *cr, GtkWidget *focus, GtkWidget *fixed)
{
	graphene_rect_t r;

	/* a fader's ring is drawn round its cap, in paint_cap() */
	if (!focus || GTK_IS_SCALE(focus) ||
	    !gtk_widget_is_ancestor(focus, fixed) ||
	    !gtk_widget_compute_bounds(focus, fixed, &r))
		return;
	cairo_set_source_rgba(cr, 0.85, 0.54, 0.23, 0.9); /* the house amber */
	cairo_set_line_width(cr, 2.0);
	cairo_rectangle(cr, r.origin.x - 2, r.origin.y - 2, r.size.width + 4,
			r.size.height + 4);
	cairo_stroke(cr);
}

/* A toggle is lit while on; a push button while it is held down. */
static bool button_lit(GtkWidget *w)
{
	if (GTK_IS_TOGGLE_BUTTON(w))
		return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
	if (GTK_IS_MENU_BUTTON(w)) /* lit while its menu is open */
		return gtk_menu_button_get_active(GTK_MENU_BUTTON(w));
	return gtk_widget_get_state_flags(w) & GTK_STATE_FLAG_ACTIVE;
}

/*
 * The connection status, written on the panel in its legends' face, in the
 * colour of the unit's own LEDs: green when connected, red when not.
 */
static void paint_status(cairo_t *cr, const struct desk *d)
{
	const struct desk_box *z = &d->status;
	bool on = app_connected(d->app);
	gchar *text = g_utf8_strup(toolbar_status_text(d->app), -1);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *font =
		pango_font_description_from_string("Inter, Sans Semi-Bold");
	int tw, th;

	pango_font_description_set_absolute_size(font, 0.42 * (z->y1 - z->y0) *
							       PANGO_SCALE);
	pango_layout_set_font_description(layout, font);
	pango_layout_set_text(layout, text, -1);
	pango_layout_get_pixel_size(layout, &tw, &th);
	if (on)
		cairo_set_source_rgb(cr, 0x2b / 255.0, 1.0, 0x7a / 255.0);
	else
		cairo_set_source_rgb(cr, 1.0, 0x1a / 255.0, 0x35 / 255.0);
	cairo_move_to(cr, (z->x0 + z->x1 - tw) / 2, (z->y0 + z->y1 - th) / 2);
	pango_cairo_show_layout(cr, layout);
	pango_font_description_free(font);
	g_object_unref(layout);
	g_free(text);
}

/*
 * The draw function of the desk's top drawing area (and of
 * desk_shot_paint(), with a NULL area): the plate, then each lit control's
 * box copied from the all-lit render, the knob pointers and fader caps, the
 * rate, the status and the focus ring, each over the ones before.  The
 * drawing area is as large as the panel, so width and height are not used.
 */
static void paint(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		  gpointer data)
{
	struct desk *d = data;
	struct app *app = d->app;

	(void)area;
	(void)width;
	(void)height;
	cairo_set_source_surface(cr, d->plate, 0, 0);
	cairo_paint(cr);
	for (size_t i = 0; i < d->n_buttons; i++) {
		const struct desk_button *b = &d->button[i];

		if (!b->widget || !button_lit(b->widget))
			continue;
		cairo_save(cr);
		/* a margin for the lit cap's glow on the panel round it */
		cairo_rectangle(cr, b->box.x0 - 2, b->box.y0 - 2,
				b->box.x1 - b->box.x0 + 4,
				b->box.y1 - b->box.y0 + 4);
		cairo_clip(cr);
		cairo_set_source_surface(cr, d->lit, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	}
	for (int m = 0; m < 2; m++) {
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			paint_pointer(cr, d, &d->knob[m][ch],
				      app->strip[m][ch].pan);
			paint_cap(cr, &d->cap[group_of(m)], &d->fader[m][ch],
				  app->strip[m][ch].fader);
		}
	}
	for (int side = 0; side < 2; side++)
		paint_cap(cr, &d->cap[GROUP_MON], &d->monitor[side],
			  app->monitor_fader[side]);
	paint_rate(cr, d);
	if (d->have_status)
		paint_status(cr, d);
	/* the amber ring only while the keyboard drives the desk: after a
	 * click or a drag it framed every control touched */
	if (app->window && gtk_window_get_focus_visible(app->window))
		paint_focus(cr, gtk_window_get_focus(app->window),
			    gtk_widget_get_parent(d->paint));
}

/* Queue a repaint of the desk; nothing once desk_detach() has run. */
static void redraw(struct desk *d)
{
	if (d->paint)
		gtk_widget_queue_draw(d->paint);
}

/* "value-changed" of a scale, "toggled" of a toggle: repaint the desk. */
static void on_changed(GObject *object, gpointer data)
{
	(void)object;
	redraw(data);
}

/* A "notify::" signal, whose handler takes the GParamSpec: repaint. */
static void on_notify(GObject *object, GParamSpec *pspec, gpointer data)
{
	(void)object;
	(void)pspec;
	redraw(data);
}

/* "state-flags-changed" of a push button: repaint, as it is lit while
 * held down (button_lit()). */
static void on_state_changed(GtkWidget *w, GtkStateFlags previous,
			     gpointer data)
{
	(void)w;
	(void)previous;
	redraw(data);
}

/* ---- Controls ---- */

/*
 * A control that keeps its size and input but draws nothing (the
 * desk-control class of load_desk_css()); mixer.c's "small" class, which
 * sets a font size, is dropped.
 */
static void make_transparent(GtkWidget *w)
{
	gtk_widget_add_css_class(w, "desk-control");
	gtk_widget_remove_css_class(w, "small");
}

/* Put w on the desk's fixed at (x, y), sized width x height, in the
 * panel's pixels rounded to whole ones. */
static void place(GtkWidget *fixed, GtkWidget *w, double x, double y,
		  double width, double height)
{
	gtk_widget_set_size_request(w, (int)lround(width), (int)lround(height));
	gtk_fixed_put(GTK_FIXED(fixed), w, round(x), round(y));
}

/*
 * A desk fader's cap, held by the pointer: only a press on the cap takes it,
 * and it then follows the pointer one to one along the travel.  A press on
 * the travel off the cap does nothing (a double click there still resets
 * the fader, through the mixer's own gesture, which runs first).
 */
struct cap_drag {
	const struct desk_fader *f;
	const struct desk_cap *cap;
	double top; /* the scale's top in the panel: where place() put it */
	double start; /* the value when the cap was taken */
	bool held;
};

/*
 * "drag-begin" of a desk fader's gesture, run in the capture phase, before
 * the scale sees the press.  The press holds the cap when it falls within
 * the cap's rows; y is in the scale's coordinates, whose top is cd->top on
 * the panel.  The width is not checked: the scale is as wide as the cap.
 */
static void on_cap_drag_begin(GtkGestureDrag *g, double x, double y,
			      gpointer data)
{
	struct cap_drag *cd = data;
	GtkWidget *scale =
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	double value = gtk_range_get_value(GTK_RANGE(scale)), lx, ly, cap_top;

	(void)x;
	fader_point(cd->f, value, &lx, &ly);
	cap_top = round(ly - cd->cap->ay) - cd->top; /* in the scale */
	cd->held = y >= cap_top && y <= cap_top + cd->cap->h;
	cd->start = value;
	/* either way GTK's range must not see the press: it would jump */
	gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
	if (cd->held)
		gtk_widget_grab_focus(scale);
}

/*
 * "drag-update": a held cap moves with the pointer's vertical offset,
 * scaled so that the travel in pixels spans the fader's whole range.  The
 * value is rounded to a whole fader position.
 */
static void on_cap_drag_update(GtkGestureDrag *g, double dx, double dy,
			       gpointer data)
{
	struct cap_drag *cd = data;
	GtkWidget *scale =
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
	double travel = cd->f->yb - cd->f->yt; /* pixels, bottom to top */

	(void)dx;
	if (!cd->held || travel <= 0)
		return;
	gtk_range_set_value(GTK_RANGE(scale),
			    round(cd->start - dy * UA1000_FADER_MAX / travel));
}

/*
 * Lay mixer.c's fader scale over its travel on the panel: one cap wide,
 * from the cap's top at full scale to its bottom at zero, with a cap_drag
 * gesture in front of GTK's own.  The scale's value-changed repaints the
 * desk, which draws the cap.
 */
static void place_fader(struct desk *d, GtkWidget *fixed, GtkWidget *scale,
			const struct desk_fader *f, const struct desk_cap *cap)
{
	struct cap_drag *cd = g_new0(struct cap_drag, 1);
	GtkGesture *drag = gtk_gesture_drag_new();

	/* GTK's own slider is not drawn here (0 px): a press anywhere would
	 * jump the value to it.  The cap is grabbed by cap_drag instead. */
	gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
	gtk_scale_clear_marks(GTK_SCALE(scale));
	gtk_widget_set_vexpand(scale, FALSE);
	make_transparent(scale);
	cd->f = f;
	cd->cap = cap;
	cd->top = round(f->yt - cap->ay);
	g_object_set_data_full(G_OBJECT(scale), "cap-drag", cd, g_free);
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag),
						   GTK_PHASE_CAPTURE);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_cap_drag_begin), cd);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_cap_drag_update),
			 cd);
	gtk_widget_add_controller(scale, GTK_EVENT_CONTROLLER(drag));
	place(fixed, scale, f->xt - cap->ax, f->yt - cap->ay, cap->w,
	      f->yb - f->yt + cap->h);
	g_signal_connect(scale, "value-changed", G_CALLBACK(on_changed), d);
}

/* Page Up/Down: a quarter of one side's travel. */
#define PAN_PAGE (UA1000_PAN_STEPS / 4.0)

/* Move a pan scale by steps; GtkRange keeps it within its range. */
static void pan_step(GtkWidget *pan, double steps)
{
	gtk_range_set_value(GTK_RANGE(pan),
			    gtk_range_get_value(GTK_RANGE(pan)) + steps);
}

/* A knob drag: the pan scale it turns, and its value at the press. */
struct knob_drag {
	GtkWidget *pan;
	double start;
};

/* "drag-begin" on a knob: the drag counts from the value at the press. */
static void on_knob_drag_begin(GtkGestureDrag *g, double x, double y,
			       gpointer data)
{
	struct knob_drag *k = data;

	(void)g;
	(void)x;
	(void)y;
	k->start = gtk_range_get_value(GTK_RANGE(k->pan));
}

/* "drag-update" on a knob: up or right turns clockwise, a step per 4
 * pixels, counted from the press so the knob does not jump. */
static void on_knob_drag_update(GtkGestureDrag *g, double dx, double dy,
				gpointer data)
{
	struct knob_drag *k = data;

	(void)g;
	gtk_range_set_value(GTK_RANGE(k->pan),
			    round(k->start + (dx - dy) / 4.0));
}

/* "scroll" on a knob: one step per event, up turning clockwise. */
static gboolean on_knob_scroll(GtkEventControllerScroll *c, double dx,
			       double dy, gpointer data)
{
	(void)c;
	(void)dx;
	pan_step(data, dy < 0 ? 1 : -1);
	return TRUE;
}

/*
 * "key-pressed" on a focused knob: arrows move a step, Page Up/Down
 * PAN_PAGE, Home centres.  Other keys are left to GTK (Tab moves the
 * focus on).
 */
static gboolean on_knob_key(GtkEventControllerKey *c, guint key, guint code,
			    GdkModifierType mods, gpointer data)
{
	(void)c;
	(void)code;
	(void)mods;
	switch (key) {
	case GDK_KEY_Right:
	case GDK_KEY_Up:
		pan_step(data, 1);
		return TRUE;
	case GDK_KEY_Left:
	case GDK_KEY_Down:
		pan_step(data, -1);
		return TRUE;
	case GDK_KEY_Page_Up:
		pan_step(data, PAN_PAGE);
		return TRUE;
	case GDK_KEY_Page_Down:
		pan_step(data, -PAN_PAGE);
		return TRUE;
	/* A pan has no one end to go to: Home centres it, as the guide says. */
	case GDK_KEY_Home:
		gtk_range_set_value(GTK_RANGE(data), 0);
		return TRUE;
	default:
		return FALSE;
	}
}

/*
 * "pressed" on a knob: a double click centres the pan, as on most desks'
 * software panels, and claims the sequence.  A single press is left to the
 * drag gesture.
 */
static void on_knob_pressed(GtkGestureClick *g, int n, double x, double y,
			    gpointer data)
{
	(void)n;
	(void)x;
	(void)y;
	if (!app_double_press(G_OBJECT(data)))
		return;
	gtk_range_set_value(GTK_RANGE(data), 0);
	gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* What a screen reader reads off the knob: its value, as the plain
 * layout's scale shows it (L5, C, R12). */
static void knob_value_to_accessible(GtkWidget *hit, GtkWidget *pan)
{
	double position = gtk_range_get_value(GTK_RANGE(pan));
	char *text = mixer_pan_text(position);

	gtk_accessible_update_property(
		GTK_ACCESSIBLE(hit), GTK_ACCESSIBLE_PROPERTY_VALUE_NOW,
		position, GTK_ACCESSIBLE_PROPERTY_VALUE_TEXT, text, -1);
	g_free(text);
}

/* "value-changed" of a pan scale: its knob's accessible value follows. */
static void on_knob_value(GtkRange *pan, gpointer hit)
{
	knob_value_to_accessible(hit, GTK_WIDGET(pan));
}

/*
 * Stand a focusable drawing area (the hit) over a pan knob.  mixer.c's pan
 * scale is not placed: the hit keeps a reference to it and its drags,
 * clicks, scrolls and keys set its value, so the scale's own handler still
 * edits the state.  The scale's value-changed repaints the desk (the
 * pointer is painted) and updates the hit's accessible value.
 */
static void place_knob(struct desk *d, GtkWidget *fixed, struct desk_knob *k,
		       GtkWidget *pan)
{
	/* The role is fixed when the widget is made: a slider, as the
	 * scale it stands for. */
	GtkWidget *hit = g_object_new(GTK_TYPE_DRAWING_AREA, "accessible-role",
				      GTK_ACCESSIBLE_ROLE_SLIDER, NULL);
	GtkGesture *drag = gtk_gesture_drag_new();
	GtkGesture *click = gtk_gesture_click_new();
	GtkEventController *scroll = gtk_event_controller_scroll_new(
		GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
	GtkEventController *key = gtk_event_controller_key_new();
	struct knob_drag *kd = g_new0(struct knob_drag, 1);
	const char *label = gtk_widget_get_tooltip_text(pan);

	/* The pan scale is not shown; it keeps the value and its handler. */
	g_object_ref_sink(pan);
	g_object_set_data_full(G_OBJECT(hit), "pan", pan, g_object_unref);
	kd->pan = pan;
	g_object_set_data_full(G_OBJECT(hit), "drag", kd, g_free);
	gtk_widget_set_focusable(hit, TRUE);
	gtk_widget_set_tooltip_text(hit, label);
	gtk_accessible_update_property(
		GTK_ACCESSIBLE(hit), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
		GTK_ACCESSIBLE_PROPERTY_ORIENTATION, GTK_ORIENTATION_HORIZONTAL,
		GTK_ACCESSIBLE_PROPERTY_VALUE_MIN, (double)-UA1000_PAN_STEPS,
		GTK_ACCESSIBLE_PROPERTY_VALUE_MAX, (double)UA1000_PAN_STEPS,
		-1);
	knob_value_to_accessible(hit, pan);
	g_signal_connect(drag, "drag-begin", G_CALLBACK(on_knob_drag_begin),
			 kd);
	g_signal_connect(drag, "drag-update", G_CALLBACK(on_knob_drag_update),
			 kd);
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(click),
				      "reset-on-double-click");
	g_signal_connect(click, "pressed", G_CALLBACK(on_knob_pressed), pan);
	g_signal_connect(scroll, "scroll", G_CALLBACK(on_knob_scroll), pan);
	g_signal_connect(key, "key-pressed", G_CALLBACK(on_knob_key), pan);
	gtk_widget_add_controller(hit, GTK_EVENT_CONTROLLER(drag));
	gtk_widget_add_controller(hit, GTK_EVENT_CONTROLLER(click));
	gtk_widget_add_controller(hit, scroll);
	gtk_widget_add_controller(hit, key);
	g_signal_connect(pan, "value-changed", G_CALLBACK(on_changed), d);
	g_signal_connect_object(pan, "value-changed", G_CALLBACK(on_knob_value),
				hit, 0);
	k->hit = hit;
	place(fixed, hit, k->cx - k->r, k->cy - k->r, 2 * k->r, 2 * k->r);
}

/*
 * Put a control over its button's box (has_all_buttons() checked it).  Its
 * child is removed: the render draws the legend.  A toggle repaints the
 * desk when toggled, a push button when its state flags change.
 */
static void place_button(struct desk *d, GtkWidget *fixed, const char *id,
			 GtkWidget *w)
{
	struct desk_button *b = button_box(d, id);

	gtk_button_set_child(GTK_BUTTON(w), NULL);
	make_transparent(w);
	b->widget = w;
	place(fixed, w, b->box.x0, b->box.y0, b->box.x1 - b->box.x0,
	      b->box.y1 - b->box.y0);
	if (GTK_IS_TOGGLE_BUTTON(w))
		g_signal_connect(w, "toggled", G_CALLBACK(on_changed), d);
	else /* a push button: lit while held down */
		g_signal_connect(w, "state-flags-changed",
				 G_CALLBACK(on_state_changed), d);
}

/*
 * "toggled" of the three clock buttons, one radio group: the one turned
 * on sets the clock.  Ignored while app_refresh() sets the buttons.
 */
static void on_clock_button(GtkToggleButton *b, gpointer data)
{
	struct app *app = data;

	if (app->refreshing || !gtk_toggle_button_get_active(b))
		return;
	for (int i = 0; i < 3; i++)
		if (GTK_WIDGET(b) == app->desk->clock[i])
			toolbar_set_clock(app, (enum ua1000_clock)i);
}

/*
 * "toggled" of the digital output pair (a radio group) and of the MONO,
 * SOFT CTRL and OUTPUT ALL switches: the panel settings with this one
 * changed go through toolbar_edit_panel().  Ignored while refreshing.
 */
static void on_panel_button(GtkToggleButton *b, gpointer data)
{
	struct app *app = data;
	const struct desk *d = app->desk;
	struct ua1000_panel panel = app->state.panel;
	bool active = gtk_toggle_button_get_active(b);
	const GtkWidget *w = GTK_WIDGET(b);

	if (app->refreshing)
		return;
	if (w == d->dout[0] || w == d->dout[1]) {
		if (!active)
			return; /* the other one of the pair turned on */
		panel.digital_out_adat = w == d->dout[0];
	} else if (w == d->mono) {
		panel.mono = active;
	} else if (w == d->soft) {
		panel.soft_ctrl = active;
	} else if (w == d->all) {
		panel.output_all = active;
	}
	toolbar_edit_panel(app, &panel);
}

/*
 * A desk toggle: its tooltip is also its accessible label, it joins
 * group's radio group when group is not NULL, and handler gets app.
 */
static GtkWidget *toggle(struct app *app, const char *tooltip,
			 GCallback handler, GtkWidget *group)
{
	GtkWidget *b = gtk_toggle_button_new();

	gtk_widget_set_tooltip_text(b, tooltip);
	gtk_accessible_update_property(
		GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
	if (group)
		gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(b),
					    GTK_TOGGLE_BUTTON(group));
	g_signal_connect(b, "toggled", handler, app);
	return b;
}

/* A push button for an application action (toolbar.c installs them),
 * labelled by its tooltip. */
static GtkWidget *action_button(const char *tooltip, const char *action)
{
	GtkWidget *b = gtk_button_new();

	gtk_widget_set_tooltip_text(b, tooltip);
	gtk_accessible_update_property(
		GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
	return b;
}

/* DEVICE: the Device menu, opening upwards from the panel's foot. */
static void place_device(struct desk *d, GtkWidget *fixed)
{
	struct desk_button *b = button_box(d, "device");
	GtkWidget *mb = gtk_menu_button_new();
	GMenuModel *menu = toolbar_device_menu();

	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), menu);
	g_object_unref(menu);
	gtk_menu_button_set_direction(GTK_MENU_BUTTON(mb), GTK_ARROW_UP);
	/* no arrow, no label: the render draws the button */
	gtk_menu_button_set_child(GTK_MENU_BUTTON(mb),
				  gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
	gtk_widget_set_tooltip_text(mb, "Device menu");
	gtk_accessible_update_property(GTK_ACCESSIBLE(mb),
				       GTK_ACCESSIBLE_PROPERTY_LABEL,
				       "Device menu", -1);
	make_transparent(mb);
	b->widget = mb;
	place(fixed, mb, b->box.x0, b->box.y0, b->box.x1 - b->box.x0,
	      b->box.y1 - b->box.y0);
	/* a notify handler takes the GParamSpec too: on_changed() does not,
	 * and connected here it would take the GParamSpec for the desk */
	g_signal_connect(mb, "notify::active", G_CALLBACK(on_notify), d);
}

/*
 * The master section, made here (the plain layout has the toolbar's
 * instead): clock, digital output, MONO, SOFT CTRL, OUTPUT ALL, the
 * memory, patch bay and notes buttons, and DEVICE.
 */
static void place_master(struct desk *d, GtkWidget *fixed)
{
	struct app *app = d->app;
	GCallback panel_cb = G_CALLBACK(on_panel_button);

	d->clock[0] = toggle(app, "Clock: internal",
			     G_CALLBACK(on_clock_button), NULL);
	d->clock[1] = toggle(app, "Clock: external S/PDIF",
			     G_CALLBACK(on_clock_button), d->clock[0]);
	d->clock[2] = toggle(app, "Clock: external ADAT",
			     G_CALLBACK(on_clock_button), d->clock[0]);
	d->dout[0] = toggle(app, "Digital output: ADAT", panel_cb, NULL);
	d->dout[1] =
		toggle(app, "Digital output: S/PDIF", panel_cb, d->dout[0]);
	d->mono = toggle(app, "Direct monitor in mono", panel_cb, NULL);
	d->soft = toggle(app, "Direct monitor follows this mixer (SOFT CTRL)",
			 panel_cb, NULL);
	d->all = toggle(
		app, "OUTPUT VOLUME knob acts on all outputs (off: MAIN only)",
		panel_cb, NULL);
	place_button(d, fixed, "clock.int", d->clock[0]);
	place_button(d, fixed, "clock.spdif", d->clock[1]);
	place_button(d, fixed, "clock.adat", d->clock[2]);
	place_button(d, fixed, "dout.adat", d->dout[0]);
	place_button(d, fixed, "dout.spdif", d->dout[1]);
	place_button(d, fixed, "panel.mono", d->mono);
	place_button(d, fixed, "panel.soft", d->soft);
	place_button(d, fixed, "panel.all", d->all);
	place_button(d, fixed, "mem.load",
		     action_button("Load memory…", "app.load-memory"));
	place_button(d, fixed, "mem.store",
		     action_button("Store memory…", "app.store-memory"));
	place_button(d, fixed, "patchbay",
		     action_button("Patch bay…", "app.patchbay"));
	place_button(d, fixed, "notes", action_button("Notes…", "app.notes"));
	place_device(d, fixed);
}

/* ---- The rate display: a click offers the rates for the USER SET ---- */

/* "clicked" of a rate in the rate popover: close it and store that rate
 * in the device's USER SET. */
static void on_rate_chosen(GtkButton *button, gpointer data)
{
	struct app *app = data;
	GtkWidget *popover =
		gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
	unsigned int hz = GPOINTER_TO_UINT(
		g_object_get_data(G_OBJECT(button), "rate-hz"));

	if (popover)
		gtk_popover_popdown(GTK_POPOVER(popover));
	app_store_user_set(app, hz);
}

/* "clicked" of the rate display: open its popover. */
static void on_rate_clicked(GtkButton *button, gpointer data)
{
	GtkWidget *popover = data;

	(void)button;
	gtk_popover_popup(GTK_POPOVER(popover));
}

/*
 * A transparent button over the rate display: the digit cells, the last
 * one's slant included, and a 6-pixel margin.  It opens a popover of the
 * four rates; desk_redraw() makes it sensitive only while connected.
 */
static void place_rate(struct desk *d, GtkWidget *fixed)
{
	static const unsigned int rates[] = { 44100, 48000, 88200, 96000 };
	const struct desk_digits *g = &d->digits;
	GtkWidget *button = gtk_button_new();
	GtkWidget *popover = gtk_popover_new();
	GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	const char *tip = "Sample rate: click to choose the one the device "
			  "starts at (stored in its USER SET)";
	double x0 = g->x[0], x1 = g->x[g->n - 1] + g->dw + g->skew * g->dh;
	double y1 = g->y[0], y0 = y1 - g->dh;

	gtk_popover_set_child(GTK_POPOVER(popover), list);
	gtk_box_append(GTK_BOX(list), gtk_label_new("Start the device at"));
	for (size_t i = 0; i < G_N_ELEMENTS(rates); i++) {
		gchar *label = g_strdup_printf("%.1f kHz", rates[i] / 1000.0);
		GtkWidget *item = gtk_button_new_with_label(label);

		g_free(label);
		g_object_set_data(G_OBJECT(item), "rate-hz",
				  GUINT_TO_POINTER(rates[i]));
		g_signal_connect(item, "clicked", G_CALLBACK(on_rate_chosen),
				 d->app);
		gtk_box_append(GTK_BOX(list), item);
	}
	gtk_widget_set_parent(popover, button);
	/* a GtkButton disposes its own child only: the popover is ours */
	g_signal_connect_swapped(button, "destroy",
				 G_CALLBACK(gtk_widget_unparent), popover);
	gtk_widget_set_tooltip_text(button, tip);
	gtk_accessible_update_property(GTK_ACCESSIBLE(button),
				       GTK_ACCESSIBLE_PROPERTY_LABEL, tip, -1);
	g_signal_connect(button, "clicked", G_CALLBACK(on_rate_clicked),
			 popover);
	make_transparent(button);
	d->rate = button;
	place(fixed, button, x0 - 6, y0 - 6, x1 - x0 + 12, y1 - y0 + 12);
}

/*
 * Place every control on the panel: both mixers' strips and links (the
 * widgets mixer_create() made), the monitor faders and link, the master
 * section and the rate display.
 */
static void place_controls(struct desk *d, GtkWidget *fixed)
{
	struct app *app = d->app;
	static const char *const mixers[] = { "in", "out" };
	char id[24];

	for (int m = 0; m < 2; m++) {
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			struct strip_widgets *w = &app->strip[m][ch];

			snprintf(id, sizeof(id), "%s.%u.mute", mixers[m],
				 ch + 1);
			place_button(d, fixed, id, w->mute);
			snprintf(id, sizeof(id), "%s.%u.solo", mixers[m],
				 ch + 1);
			place_button(d, fixed, id, w->solo);
			place_knob(d, fixed, &d->knob[m][ch], w->pan);
			place_fader(d, fixed, w->fader, &d->fader[m][ch],
				    &d->cap[group_of(m)]);
		}
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
			snprintf(id, sizeof(id), "%s.link%u", mixers[m],
				 pair + 1);
			place_button(d, fixed, id, app->link[m][pair]);
		}
	}
	for (int side = 0; side < 2; side++)
		place_fader(d, fixed, app->monitor_fader[side],
			    &d->monitor[side], &d->cap[GROUP_MON]);
	place_button(d, fixed, "mon.link1", app->monitor_link);
	place_master(d, fixed);
	place_rate(d, fixed);
}

/*
 * The desk's CSS, for the whole display: the desk-control class, and the
 * window's background in the render's backdrop colour.  Its priority is
 * one above app.c's, whose window.desk-mode rule it overrides.
 */
static void load_desk_css(const struct desk *d)
{
	/* The controls keep their size and input, and draw nothing. */
	static const char css[] =
		/* the control and the nodes a scale draws, by name: a popover
		 * hanging from a control (the rate's) must keep its look */
		".desk-control, scale.desk-control trough, "
		"scale.desk-control highlight, scale.desk-control slider, "
		"scale.desk-control fill, menubutton.desk-control > button {"
		" background: none; border: none; box-shadow: none;"
		" outline: none; padding: 0; margin: 0; min-width: 0;"
		" min-height: 0; color: transparent; }\n";
	/* The whole window is the desk: round the panel,
	 * when the window's shape is not the panel's, the render's backdrop */
	gchar *all =
		g_strdup_printf("%swindow.desk-mode, window.desk-mode > box "
				"{ background: rgb(%d, %d, %d); }\n",
				css, (int)lround(d->backdrop[0] * 255),
				(int)lround(d->backdrop[1] * 255),
				(int)lround(d->backdrop[2] * 255));
	GtkCssProvider *provider = gtk_css_provider_new();

	gtk_css_provider_load_from_string(provider, all);
	g_free(all);
	gtk_style_context_add_provider_for_display(
		gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	g_object_unref(provider);
}

/* ---- Meters ---- */

/*
 * Where a strip's meter reads.  An input strip: the WAVE IN pair the patch
 * bay feeds from that input (none: no meter, rather than one that lies);
 * an output strip: what the computer plays to it.  Both are pre-fader: the
 * device sends no levels, and no strip is tapped after its fader.  The
 * routing is the state's: until the state is sent to the device, it may
 * differ from the device's.
 *
 * MONITOR OUT: the card's capture channels 11-12, "MAIN" (the UA-1000
 * Owner's Manual, block diagram p. 52): the monitor mix itself, after every
 * fader, mute, solo, pan and MONO of both mixers, sent back to the computer by
 * the device.  A MEASURED post-fader level, taken before the front panel's
 * volume knob (read off the diagram, and checked on the device: the knob does
 * not move it, the faders and mute do).
 *
 * False: the strip has no source (an input no WAVE IN pair takes).
 */
#define MAIN_CAPTURE_CHANNEL 10 /* 0-based: MAIN L; MAIN R is 11 */

static bool meter_source(const struct app *app, int mixer, unsigned int ch,
			 bool monitor, enum meter_source_kind *kind,
			 unsigned int *channel)
{
	const uint8_t *wave_in =
		app->state.patchbay.route[UA1000_ROUTE_WAVE_IN];

	if (monitor) {
		*kind = METER_CAPTURE;
		*channel = MAIN_CAPTURE_CHANNEL + ch;
		return true;
	}
	if (mixer == UA1000_MIXER_OUTPUT) {
		*kind = METER_PLAYBACK;
		*channel = ch;
		return true;
	}
	for (unsigned int p = 0; p < UA1000_PAIRS; p++) {
		if (wave_in[p] == UA1000_SRC_INPUT_1_2 + ch / 2) {
			*kind = METER_CAPTURE;
			*channel = 2 * p + ch % 2;
			return true;
		}
	}
	return false;
}

/*
 * One tick of a meter, dt seconds after the last: take the peak since then
 * (none when not routed), let the bar fall at METER_FALL_DB_S, hold a new
 * peak for METER_HOLD_US, light the clip lamp for METER_CLIP_US at full
 * scale.  The well is repainted only when what it shows has changed.
 */
static void meter_update(struct desk_meter *mt, struct meters *meters,
			 bool routed, enum meter_source_kind kind,
			 unsigned int channel, gint64 now, double dt)
{
	bool available = false;
	float peak =
		routed ? meters_take_peak(meters, kind, channel, &available)
		       : 0.0f;
	double db = peak > 0.0f ? 20.0 * log10((double)peak) : METER_FLOOR_DB;
	double level = fmax(fmax(db, mt->level - METER_FALL_DB_S * dt),
			    METER_FLOOR_DB);
	bool changed = available != mt->available ||
		       fabs(level - mt->level) > 0.05;

	mt->available = available;
	mt->level = level;
	if (db >= mt->hold || now > mt->hold_until) {
		changed |= fabs(db - mt->hold) > 0.05;
		mt->hold = fmax(db, METER_FLOOR_DB);
		mt->hold_until = now + METER_HOLD_US;
	}
	if (peak >= 0.999f) { /* within 0.01 dB of full scale */
		mt->clip_until = now + METER_CLIP_US;
		changed = true;
	}
	if (changed && mt->area)
		gtk_widget_queue_draw(mt->area);
}

static void show_meters_live(struct desk *d, bool live);

/*
 * The meters' timeout, every METER_PERIOD_MS.  Each tick asks
 * meter_source() again, so a change of routing shows at once.
 */
static gboolean on_meter_tick(gpointer data)
{
	struct desk *d = data;
	gint64 now = g_get_monotonic_time();
	double dt = d->meter_last ? (double)(now - d->meter_last) / 1e6 : 0.0;
	enum meter_source_kind kind = METER_CAPTURE;
	unsigned int channel = 0;

	d->meter_last = now;
	/* every 2 s: a server that went away is let go, one that is not
	 * there is looked for, and meters the server disconnected (the
	 * device was off) are connected again */
	if (++d->meter_ticks % (2000 / METER_PERIOD_MS) == 0) {
		if (d->meters && !meters_alive(d->meters)) {
			meters_close(d->meters);
			d->meters = NULL;
		}
		if (!d->meters)
			d->meters = meters_open();
		else
			meters_reconnect(d->meters);
		show_meters_live(d, d->meters != NULL);
	}
	for (int m = 0; m < 2; m++)
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			bool routed = meter_source(d->app, m, ch, false, &kind,
						   &channel);

			meter_update(&d->meter[m][ch], d->meters, routed, kind,
				     channel, now, dt);
		}
	for (unsigned int side = 0; side < 2; side++) {
		bool routed =
			meter_source(d->app, 0, side, true, &kind, &channel);

		meter_update(&d->monitor_meter[side], d->meters, routed, kind,
			     channel, now, dt);
	}
	return G_SOURCE_CONTINUE;
}

/* A level as a fraction of the well's height: 0 at METER_FLOOR_DB, 1 at
 * 0 dBFS, clamped. */
static double meter_frac(double db)
{
	return fmin(fmax((db - METER_FLOOR_DB) / -METER_FLOOR_DB, 0.0), 1.0);
}

/*
 * The bar in one continuous colour scale that colour-blind readers tell
 * apart: Okabe and Ito's blue (#0072B2) through sky blue (#56B4E9) to
 * yellow (#F0E442), along the blue-yellow axis that every common colour
 * vision deficiency keeps, its lightness rising with the level (it reads
 * in grey too).  Clipping: their vermilion (#D55E00) at the top, held.
 * The level also reads by height, and the held peak by a line.
 */
static void paint_meter(GtkDrawingArea *area, cairo_t *cr, int width,
			int height, gpointer data)
{
	const struct desk_meter *mt = data;
	cairo_pattern_t *ramp;
	double top, h = height, w = width;

	(void)area;
	if (!mt->available)
		return;
	ramp = cairo_pattern_create_linear(0, h, 0, 0);
	cairo_pattern_add_color_stop_rgb(ramp, 0.0, 0x00 / 255.0, 0x72 / 255.0,
					 0xb2 / 255.0);
	cairo_pattern_add_color_stop_rgb(ramp, 0.55, 0x56 / 255.0, 0xb4 / 255.0,
					 0xe9 / 255.0);
	cairo_pattern_add_color_stop_rgb(ramp, 1.0, 0xf0 / 255.0, 0xe4 / 255.0,
					 0x42 / 255.0);
	top = h * (1.0 - meter_frac(mt->level));
	cairo_rectangle(cr, 1, top, w - 2, h - top);
	cairo_set_source(cr, ramp);
	cairo_fill(cr);
	cairo_pattern_destroy(ramp);
	if (mt->hold > METER_FLOOR_DB) {
		double y = h * (1.0 - meter_frac(mt->hold));

		cairo_set_source_rgb(cr, 0.906, 0.898, 0.937); /* the ink */
		cairo_rectangle(cr, 1, fmax(y - 1, 0), w - 2, 2);
		cairo_fill(cr);
	}
	if (g_get_monotonic_time() < mt->clip_until) {
		cairo_set_source_rgb(cr, 0xd5 / 255.0, 0x5e / 255.0,
				     0x00 / 255.0);
		cairo_rectangle(cr, 0, 0, w, fmin(5.0, h));
		cairo_fill(cr);
	}
}

/* Why a meter well stays dark: the meters read the audio from a sound
 * server. */
#define METER_OFF_TIP                                                          \
	"No meters: they read the audio through a JACK client. Start "         \
	"PipeWire (with pipewire-jack) or JACK on the UA-1000: the meters "    \
	"connect by themselves."

/* A dark well answers the pointer only to say why it is dark. */
static void show_meter_live(struct desk_meter *mt, bool live)
{
	if (!mt->area)
		return;
	gtk_widget_set_can_target(mt->area, !live);
	gtk_widget_set_tooltip_text(mt->area, live ? NULL : METER_OFF_TIP);
}

/* Every well live or dark, when that changes. */
static void show_meters_live(struct desk *d, bool live)
{
	if (live == d->meters_shown_live)
		return;
	d->meters_shown_live = live;
	for (int m = 0; m < 2; m++)
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
			show_meter_live(&d->meter[m][ch], live);
	for (int side = 0; side < 2; side++)
		show_meter_live(&d->monitor_meter[side], live);
}

/*
 * The meter's drawing area over its well, when the layout has one.  The
 * areas are placed after the panel's drawing area, so they are on top of
 * it.
 */
static void place_meter(GtkWidget *fixed, struct desk_meter *mt, bool live)
{
	if (!mt->found)
		return;
	mt->level = mt->hold = METER_FLOOR_DB;
	mt->area = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mt->area), paint_meter,
				       mt, NULL);
	show_meter_live(mt, live);
	place(fixed, mt->area, mt->box.x0, mt->box.y0, mt->box.x1 - mt->box.x0,
	      mt->box.y1 - mt->box.y0);
}

/* Open the meters, place every well and start the tick. */
static void start_meters(struct desk *d, GtkWidget *fixed)
{
	d->meters = meters_open();
	d->meters_shown_live = d->meters != NULL;
	for (int m = 0; m < 2; m++)
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
			place_meter(fixed, &d->meter[m][ch], d->meters != NULL);
	for (int side = 0; side < 2; side++)
		place_meter(fixed, &d->monitor_meter[side], d->meters != NULL);
	/* also without a server: the tick looks for one every 2 s */
	d->meter_source = g_timeout_add(METER_PERIOD_MS, on_meter_tick, d);
}

/*
 * The main window is being destroyed, and the desk's widgets with it: stop
 * the meter tick and forget them now.  desk_free() runs later, at the
 * application's shutdown, and a tick due in between would draw on
 * finalised widgets: a Gtk-CRITICAL per meter per tick, and a crash once
 * the memory is reused.
 */
void desk_detach(struct app *app)
{
	struct desk *d = app->desk;

	if (!d)
		return;
	g_clear_handle_id(&d->meter_source, g_source_remove);
	for (int m = 0; m < 2; m++)
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
			d->meter[m][ch].area = NULL;
	for (int side = 0; side < 2; side++)
		d->monitor_meter[side].area = NULL;
	d->paint = NULL;
	d->rate = NULL;
}

/* Free the desk: the tick, the meters' JACK client and the images. */
void desk_free(struct app *app)
{
	struct desk *d = app->desk;

	if (!d)
		return;
	g_clear_handle_id(&d->meter_source, g_source_remove);
	meters_close(d->meters);
	if (d->plate)
		cairo_surface_destroy(d->plate);
	if (d->lit)
		cairo_surface_destroy(d->lit);
	for (int g = 0; g < GROUP_COUNT; g++)
		if (d->cap[g].image)
			cairo_surface_destroy(d->cap[g].image);
	g_free(d);
	app->desk = NULL;
}

/* ---- Any size ---- */

/*
 * The desk keeps its proportions and scales to the window: the panel and
 * every control in it are one GtkFixed under a scale transform, centred.
 * GTK carries pointer and keyboard events through the transform, so the
 * controls behave the same at any size.
 */
struct scaler {
	struct desk *desk;
	GtkWidget *outer, *inner;
};

/* "resize" of the sizer: the panel scaled to fit, its proportions kept,
 * and centred. */
static void on_scaler_resize(GtkDrawingArea *area, int width, int height,
			     gpointer data)
{
	const struct scaler *sc = data;
	double w = sc->desk->width, h = sc->desk->height;
	double scale = fmin(width / w, height / h);
	graphene_point_t at =
		GRAPHENE_POINT_INIT((float)((width - w * scale) / 2),
				    (float)((height - h * scale) / 2));
	GskTransform *t = gsk_transform_translate(NULL, &at);

	(void)area;
	t = gsk_transform_scale(t, (float)scale, (float)scale);
	gtk_fixed_set_child_transform(GTK_FIXED(sc->outer), sc->inner, t);
	gsk_transform_unref(t);
}

/*
 * An overlay: under it, an expanding drawing area that draws nothing and
 * reports the space the window gives; over it, a GtkFixed holding the
 * panel (inner) under the transform that on_scaler_resize() sets.
 */
static GtkWidget *scaler_new(struct desk *d, GtkWidget *inner)
{
	struct scaler *sc = g_new0(struct scaler, 1);
	GtkWidget *overlay = gtk_overlay_new();
	GtkWidget *sizer = gtk_drawing_area_new();

	sc->desk = d;
	sc->inner = inner;
	sc->outer = gtk_fixed_new();
	g_object_set_data_full(G_OBJECT(overlay), "scaler", sc, g_free);
	/* the window may shrink to a quarter of the panel's size */
	gtk_widget_set_size_request(sizer, (int)lround(d->width / 4),
				    (int)lround(d->height / 4));
	gtk_widget_set_hexpand(sizer, TRUE);
	gtk_widget_set_vexpand(sizer, TRUE);
	g_signal_connect(sizer, "resize", G_CALLBACK(on_scaler_resize), sc);
	gtk_fixed_put(GTK_FIXED(sc->outer), inner, 0, 0);
	gtk_overlay_set_child(GTK_OVERLAY(overlay), sizer);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), sc->outer);
	return overlay;
}

/* The plate's top-left pixel: the backdrop the panel is rendered on. */
static void backdrop_of(struct desk *d)
{
	cairo_format_t format = cairo_image_surface_get_format(d->plate);
	const unsigned char *px;
	guint32 v;

	cairo_surface_flush(d->plate);
	px = cairo_image_surface_get_data(d->plate);
	if (!px ||
	    (format != CAIRO_FORMAT_ARGB32 && format != CAIRO_FORMAT_RGB24)) {
		d->backdrop[0] = d->backdrop[1] = d->backdrop[2] = 0.06;
		return;
	}
	/* native-endian 0xAARRGGBB, opaque at the corner */
	memcpy(&v, px, sizeof(v));
	d->backdrop[0] = (double)((v >> 16) & 0xff) / 255.0;
	d->backdrop[1] = (double)((v >> 8) & 0xff) / 255.0;
	d->backdrop[2] = (double)(v & 0xff) / 255.0;
}

/* Build the desk from the assets in dir; NULL, nothing kept, if they fail. */
GtkWidget *desk_new(struct app *app, const char *dir)
{
	struct desk *d = g_new0(struct desk, 1);
	char path[4096];
	GtkWidget *fixed;

	d->app = app;
	/* one layout for both image sets: its coordinates are the 1x pixels */
	snprintf(path, sizeof(path), "%s/1x/desk.layout", dir);
	if (!load_layout(d, path) || !load_images(d, dir)) {
		app->desk = d;
		desk_free(app);
		return NULL;
	}
	/* the controls are made only now: on failure the caller's plain
	 * layout makes them itself (mixer_new()) */
	app->desk = d;
	backdrop_of(d);
	load_desk_css(d);
	mixer_create(app);
	fixed = gtk_fixed_new();
	gtk_widget_set_size_request(fixed, (int)lround(d->width),
				    (int)lround(d->height));
	place_controls(d, fixed);
	/* the panel's drawing area goes over the controls and takes no input:
	 * every event reaches the transparent controls under it */
	d->paint = gtk_drawing_area_new();
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(d->paint), paint, d,
				       NULL);
	gtk_widget_set_can_target(d->paint, FALSE);
	place(fixed, d->paint, 0, 0, d->width, d->height);
	start_meters(d, fixed);
	/* the focus ring is painted by the desk: repaint when it moves */
	if (app->window) {
		g_signal_connect_object(app->window, "notify::focus-widget",
					G_CALLBACK(gtk_widget_queue_draw),
					d->paint, G_CONNECT_SWAPPED);
		g_signal_connect_object(app->window, "notify::focus-visible",
					G_CALLBACK(gtk_widget_queue_draw),
					d->paint, G_CONNECT_SWAPPED);
	}
	return scaler_new(d, fixed);
}

/* The panel's size in desk.layout's 1x pixels. */
void desk_natural_size(const struct app *app, int *width, int *height)
{
	*width = (int)lround(app->desk->width);
	*height = (int)lround(app->desk->height);
}


/* Repaint the desk, and offer the rate display only while connected. */
void desk_redraw(struct app *app)
{
	if (!app->desk)
		return;
	/* the USER SET is stored in the device: offered only while it is
	 * connected, as the DEVICE menu's item */
	if (app->desk->rate)
		gtk_widget_set_sensitive(app->desk->rate, app_connected(app));
	redraw(app->desk);
}

/* Show the state's panel settings on the master section's buttons. */
void desk_refresh(struct app *app)
{
	struct desk *d = app->desk;
	const struct ua1000_panel *p = &app->state.panel;

	if (!d)
		return;
	/* app_refresh() calls this with app->refreshing set: the toggled
	 * handlers return without editing the state */
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->clock[p->clock]),
				     TRUE);
	gtk_toggle_button_set_active(
		GTK_TOGGLE_BUTTON(d->dout[p->digital_out_adat ? 0 : 1]), TRUE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->mono), p->mono);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->soft), p->soft_ctrl);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->all), p->output_all);
	/* ADAT works at 44.1 / 48 kHz only: elsewhere its buttons do nothing,
	 * unless ADAT is already on (then S/PDIF and INT turn it off) */
	gtk_widget_set_sensitive(d->dout[0], ua1000_rate_allows_adat(p->rate) ||
						     p->digital_out_adat);
	gtk_widget_set_sensitive(d->clock[2],
				 ua1000_rate_allows_adat(p->rate) ||
					 p->clock == UA1000_CLOCK_EXT_ADAT);
	redraw(d);
}
