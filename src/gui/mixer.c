/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * mixer.c - the INPUT MONITOR and OUTPUT MONITOR strips and the MONITOR OUT
 *
 * Faders are GtkScales over the vendor's 1023 fader positions, so they move
 * through the same gains as the vendor's panel and get keyboard, scroll and
 * accessibility support from GTK.  They show their value in dB.
 */
#include "app.h"

#include <math.h>

/* Where a strip's widgets point back to. */
struct strip_ref {
	struct app *app;
	enum ua1000_mixer mixer;
	unsigned int channel;
};

/* ---- Faders and pan knobs ---- */

/* What a screen reader announces, and the tooltip: "Input 3 gain". */
static void describe(GtkWidget *widget, const char *what)
{
	gtk_widget_set_tooltip_text(widget, what);
	gtk_accessible_update_property(GTK_ACCESSIBLE(widget),
				       GTK_ACCESSIBLE_PROPERTY_LABEL, what, -1);
}

static void describe_channel(GtkWidget *widget, enum ua1000_mixer mixer,
			     const char *channels, const char *what)
{
	char text[48];

	snprintf(text, sizeof(text), "%s %s %s",
		 mixer == UA1000_MIXER_INPUT ? "Input" : "Output", channels,
		 what);
	describe(widget, text);
}

static char *format_gain(GtkScale *scale, double position, gpointer data)
{
	double db;

	(void)scale;
	(void)data;
	db = ua1000_gain_to_db(ua1000_fader_to_gain((unsigned int)position));
	if (isinf(db))
		return g_strdup("-inf");
	return g_strdup_printf("%+.1f", fabs(db) < 0.05 ? 0.0 : db);
}

static void add_mark(GtkWidget *fader, double db, const char *text)
{
	unsigned int position = ua1000_gain_to_fader(ua1000_gain_from_db(db));

	gtk_scale_add_mark(GTK_SCALE(fader), position, GTK_POS_LEFT, text);
}

/*
 * A double click returns a control to its default, as on most software
 * mixers: a fader to 0 dB, the state's default
 * for every strip and the monitor, a pan to the centre.
 */
bool app_double_press(GObject *control)
{
	gint64 now = g_get_monotonic_time();
	gint64 *last = g_object_get_data(control, "last-press-us");
	int ms = 400;
	bool twice;

	g_object_get(gtk_settings_get_default(), "gtk-double-click-time", &ms,
		     NULL);
	if (!last) {
		last = g_new0(gint64, 1);
		g_object_set_data_full(control, "last-press-us", last, g_free);
	}
	twice = *last != 0 && now - *last <= (gint64)ms * 1000;
	*last = twice ? 0 : now; /* a third press starts a new pair */
	return twice;
}

static void on_double_click(GtkGestureClick *gesture, int n, double x, double y,
			    gpointer data)
{
	GtkWidget *range =
		gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

	(void)n;
	(void)data;
	if (!app_double_press(G_OBJECT(range))) {
		int start, end;
		double along =
			gtk_orientable_get_orientation(GTK_ORIENTABLE(range)) ==
					GTK_ORIENTATION_VERTICAL
				? y
				: x;

		/* A press on the travel, off the slider, would jump the
		 * value there (GTK's default): on a mixer that is a level
		 * jump, and in a double click it sent the jump before the
		 * reset.  The slider moves only when it is dragged. */
		gtk_range_get_slider_range(GTK_RANGE(range), &start, &end);
		/* (on the desk the slider is not drawn, 0 px: the cap's own
		 * drag gesture decides, desk.c) */
		if (start < end && (along < start || along > end))
			gtk_gesture_set_state(GTK_GESTURE(gesture),
					      GTK_EVENT_SEQUENCE_CLAIMED);
		return;
	}
	gtk_range_set_value(GTK_RANGE(range),
			    GPOINTER_TO_INT(g_object_get_data(
				    G_OBJECT(range), "default-value")));
	/* the press is ours: the range must not move the value again */
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void reset_on_double_click(GtkWidget *range, int value)
{
	GtkGesture *click = gtk_gesture_click_new();

	g_object_set_data(G_OBJECT(range), "default-value",
			  GINT_TO_POINTER(value));
	/* before the scale's own gesture, which claims the press */
	gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click),
						   GTK_PHASE_CAPTURE);
	gtk_event_controller_set_name(GTK_EVENT_CONTROLLER(click),
				      "reset-on-double-click");
	g_signal_connect(click, "pressed", G_CALLBACK(on_double_click), NULL);
	gtk_widget_add_controller(range, GTK_EVENT_CONTROLLER(click));
}

static GtkWidget *fader_new(void)
{
	GtkWidget *fader = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0,
						    UA1000_FADER_MAX, 1);

	gtk_range_set_inverted(GTK_RANGE(fader), TRUE);
	gtk_range_set_increments(GTK_RANGE(fader), 1, 16);
	gtk_scale_set_draw_value(GTK_SCALE(fader), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(fader), GTK_POS_BOTTOM);
	gtk_scale_set_format_value_func(GTK_SCALE(fader), format_gain, NULL,
					NULL);
	add_mark(fader, 12.0, NULL);
	add_mark(fader, 6.0, NULL);
	add_mark(fader, 0.0, "0");
	add_mark(fader, -12.0, NULL);
	add_mark(fader, -24.0, NULL);
	add_mark(fader, -48.0, NULL);
	gtk_widget_set_vexpand(fader, TRUE);
	gtk_widget_add_css_class(fader, "small");
	reset_on_double_click(fader, UA1000_FADER_UNITY);
	return fader;
}

static void fader_set_gain(GtkWidget *fader, uint32_t gain)
{
	gtk_range_set_value(GTK_RANGE(fader), ua1000_gain_to_fader(gain));
}

static uint32_t fader_get_gain(GtkWidget *fader)
{
	double position = gtk_range_get_value(GTK_RANGE(fader));

	return ua1000_fader_to_gain((unsigned int)lround(position));
}

char *mixer_pan_text(double position)
{
	long p = lround(position);

	if (p == 0)
		return g_strdup("C");
	return g_strdup_printf("%c%ld", p < 0 ? 'L' : 'R', labs(p));
}

static char *format_pan(GtkScale *scale, double position, gpointer data)
{
	(void)scale;
	(void)data;
	return mixer_pan_text(position);
}

static GtkWidget *pan_new(void)
{
	GtkWidget *pan = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
						  -UA1000_PAN_STEPS,
						  UA1000_PAN_STEPS, 1);

	gtk_scale_set_draw_value(GTK_SCALE(pan), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(pan), GTK_POS_BOTTOM);
	gtk_scale_set_format_value_func(GTK_SCALE(pan), format_pan, NULL, NULL);
	gtk_scale_add_mark(GTK_SCALE(pan), 0, GTK_POS_TOP, NULL);
	gtk_widget_set_size_request(pan, 56, -1);
	gtk_widget_add_css_class(pan, "small");
	reset_on_double_click(pan, 0);
	return pan;
}

/* ---- Callbacks ---- */

static void on_fader(GtkRange *range, gpointer data)
{
	struct strip_ref *ref = data;
	struct app *app = ref->app;
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_gain(&app->state, &edit, ref->mixer, ref->channel,
			       fader_get_gain(GTK_WIDGET(range)));
	app_apply(app, ret, &edit, true);
}

static void on_pan(GtkRange *range, gpointer data)
{
	struct strip_ref *ref = data;
	struct app *app = ref->app;
	struct ua1000_edit edit;
	int position = (int)lround(gtk_range_get_value(range));
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_pan(&app->state, &edit, ref->mixer, ref->channel,
			      ua1000_pan_from_position(position));
	app_apply(app, ret, &edit, true);
}

static void on_mute(GtkToggleButton *button, gpointer data)
{
	struct strip_ref *ref = data;
	struct app *app = ref->app;
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_mute(&app->state, &edit, ref->mixer, ref->channel,
			       gtk_toggle_button_get_active(button));
	app_apply(app, ret, &edit, false);
}

static void on_solo(GtkToggleButton *button, gpointer data)
{
	struct strip_ref *ref = data;
	struct app *app = ref->app;
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_solo(&app->state, &edit, ref->mixer, ref->channel,
			       gtk_toggle_button_get_active(button));
	app_apply(app, ret, &edit, false);
}

/* For a pair, ref->channel is the pair's first channel. */
static void on_link(GtkToggleButton *button, gpointer data)
{
	struct strip_ref *ref = data;
	struct app *app = ref->app;
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_link(&app->state, &edit, ref->mixer, ref->channel / 2,
			       gtk_toggle_button_get_active(button));
	app_apply(app, ret, &edit, false);
}

static void on_monitor_fader(GtkRange *range, gpointer data)
{
	struct app *app = data;
	enum ua1000_side side = GTK_WIDGET(range) == app->monitor_fader[0]
					? UA1000_LEFT
					: UA1000_RIGHT;
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_monitor_gain(&app->state, &edit, side,
				       fader_get_gain(GTK_WIDGET(range)));
	app_apply(app, ret, &edit, true);
}

static void on_monitor_link(GtkToggleButton *button, gpointer data)
{
	struct app *app = data;
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_monitor_link(&app->state, &edit,
				       gtk_toggle_button_get_active(button));
	app_apply(app, ret, &edit, false);
}

/* ---- Construction ---- */

static struct strip_ref *ref_new(GtkWidget *holder, struct app *app,
				 enum ua1000_mixer mixer, unsigned int channel)
{
	struct strip_ref *ref = g_new(struct strip_ref, 1);

	ref->app = app;
	ref->mixer = mixer;
	ref->channel = channel;
	/* Freed with the widget that owns it. */
	g_object_set_data_full(G_OBJECT(holder), "strip-ref", ref, g_free);
	return ref;
}

static GtkWidget *toggle_new(const char *label, const char *css)
{
	GtkWidget *button = gtk_toggle_button_new_with_label(label);

	gtk_widget_add_css_class(button, css);
	gtk_widget_add_css_class(button, "small");
	return button;
}

/* A channel's controls, wired to the state; laid out by the caller. */
static void strip_create(struct app *app, enum ua1000_mixer mixer,
			 unsigned int channel)
{
	struct strip_widgets *w = &app->strip[mixer][channel];
	char name[4];

	snprintf(name, sizeof(name), "%u", channel + 1);
	/* The vendor's panel shows the inverse, "ON"; this one has a mute that
	 * lights when the channel is silenced. */
	w->mute = toggle_new("M", "channel-mute");
	w->solo = toggle_new("S", "channel-solo");
	w->pan = pan_new();
	w->fader = fader_new();

	struct strip_ref *ref = ref_new(w->fader, app, mixer, channel);

	describe_channel(w->mute, mixer, name, "mute");
	describe_channel(w->solo, mixer, name, "solo");
	describe_channel(w->pan, mixer, name, "pan");
	describe_channel(w->fader, mixer, name, "gain");
	g_signal_connect(w->mute, "toggled", G_CALLBACK(on_mute), ref);
	g_signal_connect(w->solo, "toggled", G_CALLBACK(on_solo), ref);
	g_signal_connect(w->pan, "value-changed", G_CALLBACK(on_pan), ref);
	g_signal_connect(w->fader, "value-changed", G_CALLBACK(on_fader), ref);
}

static void link_create(struct app *app, enum ua1000_mixer mixer,
			unsigned int pair)
{
	GtkWidget *link = toggle_new("LINK", "channel-link");
	struct strip_ref *ref = ref_new(link, app, mixer, 2 * pair);
	char pair_name[8];

	snprintf(pair_name, sizeof(pair_name), "%u-%u", 2 * pair + 1,
		 2 * pair + 2);
	describe_channel(link, mixer, pair_name, "link");
	app->link[mixer][pair] = link;
	g_signal_connect(link, "toggled", G_CALLBACK(on_link), ref);
}

void mixer_create(struct app *app)
{
	for (int m = UA1000_MIXER_INPUT; m <= UA1000_MIXER_OUTPUT; m++) {
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++)
			strip_create(app, m, ch);
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			link_create(app, m, pair);
	}
	for (int side = 0; side < 2; side++) {
		app->monitor_fader[side] = fader_new();
		describe(app->monitor_fader[side],
			 side == UA1000_LEFT ? "Monitor left gain"
					     : "Monitor right gain");
		g_signal_connect(app->monitor_fader[side], "value-changed",
				 G_CALLBACK(on_monitor_fader), app);
	}
	app->monitor_link = toggle_new("LINK", "channel-link");
	describe(app->monitor_link, "Monitor link");
	g_signal_connect(app->monitor_link, "toggled",
			 G_CALLBACK(on_monitor_link), app);
}

/* ---- The plain layout: GTK's own widgets in boxes ---- */

static GtkWidget *strip_box(struct app *app, enum ua1000_mixer mixer,
			    unsigned int channel)
{
	const struct strip_widgets *w = &app->strip[mixer][channel];
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	char name[4];

	snprintf(name, sizeof(name), "%u", channel + 1);

	GtkWidget *label = gtk_label_new(name);

	gtk_widget_add_css_class(label, "channel-name");
	gtk_box_append(GTK_BOX(box), label);
	gtk_box_append(GTK_BOX(box), w->mute);
	gtk_box_append(GTK_BOX(box), w->solo);
	gtk_box_append(GTK_BOX(box), w->pan);
	gtk_box_append(GTK_BOX(box), w->fader);
	return box;
}

static GtkWidget *section_box(struct app *app, enum ua1000_mixer mixer)
{
	GtkWidget *frame = gtk_frame_new(mixer == UA1000_MIXER_INPUT
						 ? "INPUT MONITOR"
						 : "OUTPUT MONITOR");
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
		GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		GtkWidget *strips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

		gtk_widget_set_vexpand(strips, TRUE);
		gtk_box_append(GTK_BOX(strips),
			       strip_box(app, mixer, 2 * pair));
		gtk_box_append(GTK_BOX(strips),
			       strip_box(app, mixer, 2 * pair + 1));
		gtk_box_append(GTK_BOX(column), strips);
		gtk_box_append(GTK_BOX(column), app->link[mixer][pair]);
		gtk_box_append(GTK_BOX(row), column);
	}
	gtk_widget_set_margin_start(row, 4);
	gtk_widget_set_margin_end(row, 4);
	gtk_widget_set_margin_bottom(row, 4);
	gtk_frame_set_child(GTK_FRAME(frame), row);
	return frame;
}

static GtkWidget *monitor_box(struct app *app)
{
	GtkWidget *frame = gtk_frame_new("MONITOR OUT");
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *faders = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	static const char *const names[] = { "L", "R" };

	for (int side = 0; side < 2; side++) {
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		GtkWidget *label = gtk_label_new(names[side]);

		gtk_widget_add_css_class(label, "channel-name");
		gtk_box_append(GTK_BOX(box), label);
		gtk_box_append(GTK_BOX(box), app->monitor_fader[side]);
		gtk_box_append(GTK_BOX(faders), box);
	}
	gtk_widget_set_vexpand(faders, TRUE);
	gtk_box_append(GTK_BOX(column), faders);
	gtk_box_append(GTK_BOX(column), app->monitor_link);
	gtk_widget_set_margin_start(column, 4);
	gtk_widget_set_margin_end(column, 4);
	gtk_widget_set_margin_bottom(column, 4);
	gtk_frame_set_child(GTK_FRAME(frame), column);
	return frame;
}

GtkWidget *mixer_new(struct app *app)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	mixer_create(app);
	gtk_widget_set_vexpand(row, TRUE);
	gtk_widget_set_margin_start(row, 6);
	gtk_widget_set_margin_end(row, 6);
	gtk_widget_set_margin_top(row, 6);
	gtk_box_append(GTK_BOX(row), section_box(app, UA1000_MIXER_INPUT));
	gtk_box_append(GTK_BOX(row), section_box(app, UA1000_MIXER_OUTPUT));
	gtk_box_append(GTK_BOX(row), monitor_box(app));
	return row;
}

void mixer_refresh(struct app *app)
{
	const struct ua1000_state *st = &app->state;

	for (int m = UA1000_MIXER_INPUT; m <= UA1000_MIXER_OUTPUT; m++) {
		for (unsigned int ch = 0; ch < UA1000_CHANNELS; ch++) {
			const struct ua1000_strip *s = &st->mixer[m].strip[ch];
			struct strip_widgets *w = &app->strip[m][ch];

			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->mute),
						     s->mute);
			gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->solo),
						     s->solo);
			gtk_range_set_value(GTK_RANGE(w->pan),
					    ua1000_pan_to_position(s->pan));
			fader_set_gain(w->fader, s->gain);
		}
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			gtk_toggle_button_set_active(
				GTK_TOGGLE_BUTTON(app->link[m][pair]),
				st->mixer[m].link[pair]);
	}
	for (int side = 0; side < 2; side++)
		fader_set_gain(app->monitor_fader[side],
			       st->monitor_gain[side]);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->monitor_link),
				     st->monitor_link);
}
