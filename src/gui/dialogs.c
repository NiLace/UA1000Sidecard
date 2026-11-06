/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dialogs.c - Store/Load Memory, Store USER SET and Reset to Defaults
 *
 * The Device menu's modal dialogs (opened by toolbar.c's actions), built
 * on one frame, struct dialog: a content box, Cancel and an accept button
 * whose handler decides whether the dialog closes.  A dialog's memory is
 * tied to its window and freed when the window is destroyed.
 *
 * The dialogs collect a choice and hand it to the state module and app.c
 * (ua1000_edit_store/load(), ua1000_state_reset(), app_apply(), app_sync(),
 * app_send()); they keep no setting of their own.  app_store_user_set() is
 * also called by desk.c, which offers the USER SET on its own panel.
 */
#include "app.h"

/* In the order of the bits of enum ua1000_section. */
static const char *const section_labels[UA1000_SECTIONS] = {
	"Panel switches", "WAVE IN", "OUTPUT", "Mixer (MONITOR)"
};

/* One open dialog; freed with its window. */
struct dialog {
	struct app *app;
	GtkWidget *window;
	GtkWidget *content; /* the dialog's own widgets go here */
	/* Returns false to keep the dialog open. */
	bool (*accept)(struct dialog *dialog);
	/* What the particular dialog needs. */
	enum ua1000_memory_op op;
	GtkWidget *slot;
	GtkWidget *section[UA1000_SECTIONS];
	GtkWidget *rate;
};

/* Cancel's "clicked" handler: close without doing anything. */
static void on_cancel(GtkButton *button, gpointer data)
{
	struct dialog *d = data;

	(void)button;
	gtk_window_destroy(GTK_WINDOW(d->window));
}

/* The accept button's "clicked" handler: close once accept() agrees. */
static void on_accept(GtkButton *button, gpointer data)
{
	struct dialog *d = data;

	(void)button;
	if (d->accept(d))
		gtk_window_destroy(GTK_WINDOW(d->window));
}

/* Let Escape close the window, as Cancel does. */
static void close_on_escape(GtkWidget *window)
{
	GtkEventController *keys = gtk_shortcut_controller_new();

	gtk_shortcut_controller_add_shortcut(
		GTK_SHORTCUT_CONTROLLER(keys),
		gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
				 gtk_named_action_new("window.close")));
	gtk_widget_add_controller(window, keys);
}

/*
 * A modal window over the main one, with a content box and Cancel / accept
 * buttons; accept_label takes a mnemonic ("_Store").  The caller fills
 * d->content and presents d->window.
 */
static struct dialog *dialog_new(struct app *app, const char *title,
				 const char *accept_label,
				 bool (*accept)(struct dialog *))
{
	struct dialog *d = g_new0(struct dialog, 1);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *cancel = gtk_button_new_with_mnemonic("_Cancel");
	GtkWidget *ok = gtk_button_new_with_mnemonic(accept_label);

	d->app = app;
	d->accept = accept;
	d->window = gtk_window_new();
	d->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	/* The dialog's memory goes with its window. */
	g_object_set_data_full(G_OBJECT(d->window), "dialog", d, g_free);

	gtk_window_set_title(GTK_WINDOW(d->window), title);
	gtk_window_set_modal(GTK_WINDOW(d->window), TRUE);
	gtk_window_set_transient_for(GTK_WINDOW(d->window), app->window);
	gtk_window_set_destroy_with_parent(GTK_WINDOW(d->window), TRUE);
	close_on_escape(d->window);
	gtk_window_set_resizable(GTK_WINDOW(d->window), FALSE);
	gtk_widget_add_css_class(ok, "suggested-action");
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), d);
	g_signal_connect(ok, "clicked", G_CALLBACK(on_accept), d);
	gtk_box_append(GTK_BOX(buttons), cancel);
	gtk_box_append(GTK_BOX(buttons), ok);
	gtk_box_append(GTK_BOX(box), d->content);
	gtk_box_append(GTK_BOX(box), buttons);
	gtk_widget_set_margin_start(box, 16);
	gtk_widget_set_margin_end(box, 16);
	gtk_widget_set_margin_top(box, 16);
	gtk_widget_set_margin_bottom(box, 16);
	gtk_window_set_child(GTK_WINDOW(d->window), box);
	gtk_window_set_default_widget(GTK_WINDOW(d->window), ok);
	return d;
}

/* A paragraph of explanation, wrapped, in the content box. */
static void add_text(struct dialog *d, const char *text)
{
	GtkWidget *label = gtk_label_new(text);

	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(label), 50);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	gtk_box_append(GTK_BOX(d->content), label);
}

/* A caption and its dropdown on one row; a screen reader names the dropdown
 * by the caption. */
static void append_labelled(GtkWidget *row, const char *caption,
			    GtkWidget *dropdown)
{
	GtkWidget *label = gtk_label_new(caption);

	gtk_accessible_update_relation(GTK_ACCESSIBLE(dropdown),
				       GTK_ACCESSIBLE_RELATION_LABELLED_BY,
				       label, NULL, -1);
	gtk_box_append(GTK_BOX(row), label);
	gtk_box_append(GTK_BOX(row), dropdown);
}

/* ---- Memory ---- */

/*
 * Store or load the chosen slot with the ticked sections.  With no section
 * ticked the dialog stays open (with a bell); otherwise it closes, also
 * when the device is not connected (a notice says so).  After a load, the
 * device is read again so the panel shows what the slot holds.
 */
static bool accept_memory(struct dialog *d)
{
	struct app *app = d->app;
	unsigned int slot =
		gtk_drop_down_get_selected(GTK_DROP_DOWN(d->slot)) + 1;
	unsigned int sections = 0;
	struct ua1000_edit edit;
	bool known = true;
	int ret;

	if (!app_connected(app)) {
		app_notice(app, "The device is not connected.");
		return true;
	}
	for (unsigned int i = 0; i < UA1000_SECTIONS; i++)
		if (gtk_check_button_get_active(
			    GTK_CHECK_BUTTON(d->section[i])))
			sections |= 1u << i;
	if (sections == 0) {
		gtk_widget_error_bell(d->window);
		return false;
	}
	if (d->op == UA1000_MEMORY_STORE)
		ret = ua1000_edit_store(&app->state, &edit, slot, sections);
	else
		ret = ua1000_edit_load(&app->state, &edit, slot, sections,
				       &known);
	app_apply(app, ret, &edit, false);
	/* the device holds what the memory loaded: show it */
	if (d->op == UA1000_MEMORY_LOAD && ret == 0)
		app_reread_device_later(app, slot);
	if (!known) {
		gchar *text = g_strdup_printf(
			"Memory %u was not stored by this editor: the panel "
			"reads what it loaded from the device in a moment.",
			slot);

		app_notice(app, text);
		g_free(text);
	}
	return true;
}

/* Open the Store Memory or Load Memory dialog, every section ticked. */
void dialog_memory(struct app *app, enum ua1000_memory_op op)
{
	GtkStringList *slots = gtk_string_list_new(NULL);
	bool store = op == UA1000_MEMORY_STORE;
	struct dialog *d =
		dialog_new(app, store ? "Store Memory" : "Load Memory",
			   store ? "_Store" : "_Load", accept_memory);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

	d->op = op;
	for (unsigned int n = 1; n <= UA1000_MEMORY_SLOTS; n++) {
		char name[16];

		snprintf(name, sizeof(name), "Memory %u", n);
		gtk_string_list_append(slots, name);
	}
	d->slot = gtk_drop_down_new(G_LIST_MODEL(slots), NULL);
	append_labelled(row, store ? "Store to" : "Load from", d->slot);
	gtk_box_append(GTK_BOX(d->content), row);
	for (unsigned int i = 0; i < UA1000_SECTIONS; i++) {
		d->section[i] =
			gtk_check_button_new_with_label(section_labels[i]);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(d->section[i]),
					    TRUE);
		gtk_box_append(GTK_BOX(d->content), d->section[i]);
	}
	gtk_window_present(GTK_WINDOW(d->window));
}

/* ---- USER SET ---- */

/* The rates offered, in the order of the dialog's menu. */
static const unsigned int rates[] = { 44100, 48000, 88200, 96000 };

/*
 * Store the USER SET at hz with the panel switches as they are, then tell
 * the user how it takes effect: the device changes rate only at power-on,
 * with its sample-rate switch on USER SET (Owner's Manual, p. 13).
 * Returns false when nothing was stored: not connected, a rate the device
 * does not offer, or ADAT at a rate that does not allow it.
 */
bool app_store_user_set(struct app *app, unsigned int hz)
{
	struct ua1000_panel panel = app->state.panel;
	struct ua1000_msg msg;
	gchar *text;

	if (!app_connected(app)) {
		app_notice(app, "The device is not connected: the sample rate "
				"is stored in the device, so it must be "
				"connected.");
		return false;
	}
	if (!ua1000_rate_from_hz(hz, &panel.rate))
		return false;
	if ((panel.digital_out_adat || panel.clock == UA1000_CLOCK_EXT_ADAT) &&
	    !ua1000_rate_allows_adat(panel.rate)) {
		app_notice(app, "ADAT works only at 44.1 or 48 kHz: turn ADAT "
				"off before storing a USER SET at this rate "
				"(Owner's Manual, p. 41).");
		return false;
	}
	ua1000_build_user_set(&msg, &panel);
	app_send(app, &msg, 1);
	text = g_strdup_printf(
		"Stored: the USER SET now starts the device at %.1f kHz.\n\n"
		"The UA-1000 changes its sample rate only when it is switched "
		"on. To use it:\n"
		"1. Close every program that is using the UA-1000.\n"
		"2. Turn the SAMPLE RATE switch on the front panel to USER "
		"SET.\n"
		"3. Turn the UA-1000 off, and on again.",
		hz / 1000.0);
	app_notice(app, text);
	g_free(text);
	return true;
}

/*
 * Store the chosen rate.  The dialog closes on success, and also when the
 * device is not connected (nothing to retry); it stays open after an ADAT
 * refusal.
 */
static bool accept_user_set(struct dialog *d)
{
	guint index = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->rate));

	/* The menu lists exactly the four rates; an ADAT refusal keeps the
	 * dialog open, so another rate can be chosen. */
	return app_store_user_set(d->app, rates[index]) ||
	       !app_connected(d->app);
}

/*
 * Open the Store USER SET dialog, the running rate preselected (48 kHz when
 * the rate is none of the four).
 */
void dialog_user_set(struct app *app)
{
	static const char *const labels[] = { "44.1 kHz", "48 kHz", "88.2 kHz",
					      "96 kHz", NULL };
	struct dialog *d =
		dialog_new(app, "Store USER SET", "_Store", accept_user_set);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	guint current = 1;

	add_text(d, "The USER SET is what the device powers up with: this "
		    "sample rate and the panel switches as they are now.");
	d->rate = gtk_drop_down_new_from_strings(labels);
	for (guint i = 0; i < G_N_ELEMENTS(rates); i++)
		if (rates[i] == ua1000_rate_hz(app->state.panel.rate))
			current = i;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(d->rate), current);
	append_labelled(row, "Sample rate at power-on", d->rate);
	gtk_box_append(GTK_BOX(d->content), row);
	gtk_window_present(GTK_WINDOW(d->window));
}

/* ---- Reset ---- */

/*
 * Reset the state to the defaults (memories and running rate kept) and
 * send all of it to the device.
 */
static bool accept_reset(struct dialog *d)
{
	ua1000_state_reset(&d->app->state);
	app_sync(d->app);
	return true;
}

/* Open the Reset to Defaults dialog, which says what a reset sets. */
void dialog_reset(struct app *app)
{
	struct dialog *d =
		dialog_new(app, "Reset to Defaults", "_Reset", accept_reset);

	add_text(d, "Set every channel to 0 dB, on and not soloed; OUTPUT "
		    "1-10 and INPUT 9-10 as linked stereo pairs (odd channels "
		    "left, even right), INPUT 1-8 centred and unlinked; the "
		    "monitor to 0 dB and linked; the panel "
		    "switches and the patch bay to the device's initial "
		    "settings.  The device is updated at once.");
	add_text(d, "The memories and the USER SET stored in the device are "
		    "not changed.");
	gtk_window_present(GTK_WINDOW(d->window));
}
