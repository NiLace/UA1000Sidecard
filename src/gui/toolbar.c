/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * toolbar.c - the panel switches, the connection status and the Device menu
 *
 * The plain GTK layout's bar: the sample rate (read only), CLOCK, the
 * digital output, MONO, SOFT CTRL, OUTPUT VOLUME, PATCH BAY, the Device
 * menu and the connection status.  Its widgets live in struct app (rate_label
 * set means the bar exists); with the desk layout they stay NULL and the
 * desk draws the same controls itself.
 *
 * Shared by both layouts: toolbar_edit_panel() and toolbar_set_clock()
 * (what a switch change does: the ADAT rule, the notices, sending), the
 * Device menu model and the application actions it names, and the
 * connection status, which also enables or disables the actions that need
 * the device.  The actions only open windows or dialogs (patchbay.c,
 * notes.c, dialogs.c) or call app.c.
 *
 * Used by app.c (building, refreshing, status) and desk.c.
 */
#include "app.h"

#include <errno.h>

/* The clock dropdown's entries, in the order of enum ua1000_clock. */
static const char *const clock_labels[] = { "Internal", "External S/PDIF",
					    "External ADAT", NULL };

/* Apply a whole panel, telling the user when ADAT is refused at this rate. */
void toolbar_edit_panel(struct app *app, const struct ua1000_panel *panel)
{
	struct ua1000_edit edit;
	int ret;

	if (app->refreshing)
		return;
	ret = ua1000_edit_panel(&app->state, &edit, panel);
	if (ret == -ERANGE)
		app_notice(app, "ADAT works only at 44.1 or 48 kHz. The device "
				"runs at another rate, and would ignore it "
				"(Owner's Manual, p. 41).");
	app_apply(app, ret, &edit, false);
}

/*
 * The plain layout's switches.  The desk has its own widgets for the same
 * switches (a radio pair for the digital output, three buttons for the
 * clock: desk.c, on_panel_button and on_clock_button), so each layout maps
 * its widgets to the panel's fields; what an edit does (the ADAT rule, the
 * notices, sending) is here once, in toolbar_edit_panel and
 * toolbar_set_clock.
 *
 * This is the "toggled" handler of the four toggle buttons; the button
 * itself tells which field it sets.
 */
static void on_switch(GtkToggleButton *button, gpointer data)
{
	struct app *app = data;
	struct ua1000_panel panel = app->state.panel;
	bool active = gtk_toggle_button_get_active(button);
	const GtkWidget *w = GTK_WIDGET(button);

	if (w == app->mono)
		panel.mono = active;
	else if (w == app->soft_ctrl)
		panel.soft_ctrl = active;
	else if (w == app->output_all)
		panel.output_all = active;
	else if (w == app->digital_out)
		panel.digital_out_adat = active;
	toolbar_edit_panel(app, &panel);
}

/*
 * Select a clock source, with ADAT output for the ADAT clock, and warn on
 * changing to an external clock.
 */
void toolbar_set_clock(struct app *app, enum ua1000_clock clock)
{
	struct ua1000_panel panel = app->state.panel;
	bool external = clock != UA1000_CLOCK_INTERNAL &&
			clock != app->state.panel.clock;
	bool was_connected = app_connected(app);

	panel.clock = clock;
	/* The vendor's panel selects ADAT output with the ADAT clock. */
	if (panel.clock == UA1000_CLOCK_EXT_ADAT)
		panel.digital_out_adat = true;
	toolbar_edit_panel(app, &panel);
	/* The device cannot say whether it has locked (README, Known
	 * limitations): say what an external clock means, since with nothing
	 * on that input the device may stop answering.  Only when the clock
	 * did change, and not when the send just lost the device: its own
	 * notice stays. */
	if (external && app->state.panel.clock == clock &&
	    !(was_connected && !app_connected(app)))
		app_notice(app,
			   clock == UA1000_CLOCK_EXT_ADAT
				   ? "The UA-1000 now takes its clock from its "
				     "ADAT input.\n\nWith nothing sending "
				     "ADAT there, it has no clock: audio "
				     "stops, and it may stop answering this "
				     "panel. Choose INT if nothing is "
				     "connected."
				   : "The UA-1000 now takes its clock from its "
				     "S/PDIF input.\n\nWith nothing sending "
				     "S/PDIF there, it has no clock: audio "
				     "stops, and it may stop answering this "
				     "panel. Choose INT if nothing is "
				     "connected.");
}

/*
 * "notify::selected" of the clock dropdown; its positions are the enum's
 * values (clock_labels).
 */
static void on_clock(GtkDropDown *dropdown, GParamSpec *pspec, gpointer data)
{
	(void)pspec;
	toolbar_set_clock(
		data, (enum ua1000_clock)gtk_drop_down_get_selected(dropdown));
}

/* A panel switch as a toggle button, wired to on_switch(). */
static GtkWidget *switch_new(struct app *app, const char *label,
			     const char *tooltip)
{
	GtkWidget *button = gtk_toggle_button_new_with_label(label);

	gtk_widget_set_tooltip_text(button, tooltip);
	g_signal_connect(button, "toggled", G_CALLBACK(on_switch), app);
	return button;
}

/*
 * A caption beside a control, in a box; a screen reader names the control
 * by the caption.
 */
static GtkWidget *captioned(const char *caption, GtkWidget *control)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *label = gtk_label_new(caption);

	gtk_accessible_update_relation(GTK_ACCESSIBLE(control),
				       GTK_ACCESSIBLE_RELATION_LABELLED_BY,
				       label, NULL, -1);
	gtk_box_append(GTK_BOX(box), label);
	gtk_box_append(GTK_BOX(box), control);
	return box;
}

/* Build the Device menu; the caller owns the returned reference. */
GMenuModel *toolbar_device_menu(void)
{
	GMenu *menu = g_menu_new();
	GMenu *device = g_menu_new();
	GMenu *memory = g_menu_new();
	GMenu *app = g_menu_new();

	g_menu_append(device, "Patch Bay…", "app.patchbay");
	g_menu_append(device, "Notes…", "app.notes");
	g_menu_append(device, "Send All Settings", "app.sync");
	g_menu_append(device, "Reconnect", "app.connect");
	g_menu_append(memory, "Store Memory…", "app.store-memory");
	g_menu_append(memory, "Load Memory…", "app.load-memory");
	g_menu_append(memory, "Store USER SET…", "app.user-set");
	g_menu_append(app, "Reset to Defaults…", "app.reset");
	g_menu_append(app, "Quit", "app.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(device));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(memory));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(app));
	g_object_unref(device);
	g_object_unref(memory);
	g_object_unref(app);
	return G_MENU_MODEL(menu);
}

/* The panel switches and the rate, for the plain layout. */
static void switches_new(struct app *app)
{
	app->rate_label = gtk_label_new(NULL);
	gtk_widget_set_tooltip_text(app->rate_label,
				    "Sample rate the device runs at.  It is "
				    "set at power-on, from the USER SET.");
	app->clock = gtk_drop_down_new_from_strings(clock_labels);
	g_signal_connect(app->clock, "notify::selected", G_CALLBACK(on_clock),
			 app);
	app->digital_out = switch_new(app, "ADAT",
				      "Digital output format: ADAT when on, "
				      "S/PDIF when off");
	app->mono = switch_new(app, "MONO", "Direct monitor in mono");
	app->soft_ctrl = switch_new(app, "SOFT CTRL",
				    "Direct monitor follows this mixer");
	app->output_all = switch_new(app, "ALL",
				     "OUTPUT VOLUME knob acts on all outputs "
				     "when on, on MAIN only when off");
}

/* Build the plain layout's bar and show the current status on it. */
GtkWidget *toolbar_new(struct app *app)
{
	GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *menu_button = gtk_menu_button_new();
	GMenuModel *menu = toolbar_device_menu();

	GtkWidget *patchbay = gtk_button_new_with_label("Patch Bay…");

	switches_new(app);
	app->status = gtk_label_new(NULL);
	gtk_widget_set_hexpand(app->status, TRUE);
	gtk_widget_set_halign(app->status, GTK_ALIGN_END);

	gtk_menu_button_set_label(GTK_MENU_BUTTON(menu_button), "Device");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), menu);
	g_object_unref(menu);

	gtk_actionable_set_action_name(GTK_ACTIONABLE(patchbay),
				       "app.patchbay");
	gtk_box_append(GTK_BOX(bar), app->rate_label);
	gtk_box_append(GTK_BOX(bar), captioned("Clock", app->clock));
	gtk_box_append(GTK_BOX(bar),
		       captioned("Digital out", app->digital_out));
	gtk_box_append(GTK_BOX(bar), app->mono);
	gtk_box_append(GTK_BOX(bar), app->soft_ctrl);
	gtk_box_append(GTK_BOX(bar),
		       captioned("Output volume", app->output_all));
	gtk_box_append(GTK_BOX(bar), patchbay);
	gtk_box_append(GTK_BOX(bar), menu_button);
	gtk_box_append(GTK_BOX(bar), app->status);
	gtk_widget_set_margin_start(bar, 8);
	gtk_widget_set_margin_end(bar, 8);
	gtk_widget_set_margin_top(bar, 6);
	gtk_widget_set_margin_bottom(bar, 6);
	toolbar_set_status(app);
	return bar;
}

/*
 * Make the bar's widgets show the panel.  Called by app_refresh() with
 * app->refreshing set, so the handlers this fires send nothing.
 */
void toolbar_refresh(struct app *app)
{
	const struct ua1000_panel *p = &app->state.panel;

	if (!app->rate_label)
		return; /* the desk shows the switches itself */

	gchar *rate =
		g_strdup_printf("%.1f kHz", ua1000_rate_hz(p->rate) / 1000.0);

	gtk_label_set_text(GTK_LABEL(app->rate_label), rate);
	g_free(rate);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(app->clock), (guint)p->clock);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->digital_out),
				     p->digital_out_adat);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mono), p->mono);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->soft_ctrl),
				     p->soft_ctrl);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->output_all),
				     p->output_all);
}

/*
 * What only the device can do (memories, USER SET, sending everything) is
 * offered only while it is connected; other edits are kept in the state and
 * reach the device when it connects.  Called by toolbar_set_status().
 */
static void enable_device_actions(struct app *app, bool on)
{
	static const char *const names[] = { "sync", "store-memory",
					     "load-memory", "user-set" };

	for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
		GAction *action = g_action_map_lookup_action(
			G_ACTION_MAP(app->gtk), names[i]);

		if (action)
			g_simple_action_set_enabled(G_SIMPLE_ACTION(action),
						    on);
	}
}

/* The connection status in words. */
const char *toolbar_status_text(const struct app *app)
{
	return app_connected(app) ? "Connected" : "Not connected";
}

/*
 * Show the connection status (the label's text and its CSS class, or the
 * desk's legend) and enable the actions that need the device.
 */
void toolbar_set_status(struct app *app)
{
	bool on = app_connected(app);

	enable_device_actions(app, on);
	/* Also called from shutdown, after the window has gone. */
	if (!app->window)
		return;
	desk_redraw(app);
	if (!app->status)
		return; /* the desk writes it on its panel */
	gtk_label_set_text(GTK_LABEL(app->status), toolbar_status_text(app));
	gtk_widget_remove_css_class(app->status,
				    on ? "disconnected" : "connected");
	gtk_widget_add_css_class(app->status,
				 on ? "connected" : "disconnected");
}

/*
 * ---- Actions ----
 *
 * The app.* actions of the Device menu, one handler each; data is the
 * struct app.  Installed by toolbar_install_actions().
 */

/* app.patchbay: the PATCH BAY window. */
static void act_patchbay(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	patchbay_present(data);
}

/* app.notes: the NOTES window. */
static void act_notes(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	notes_present(data);
}

/* app.sync: send every setting to the device. */
static void act_sync(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	app_sync(data);
}

/* app.connect: connect to the device again. */
static void act_connect(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	app_connect(data);
}

/* app.store-memory: the Store Memory dialog. */
static void act_store_memory(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	dialog_memory(data, UA1000_MEMORY_STORE);
}

/* app.load-memory: the Load Memory dialog. */
static void act_load_memory(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	dialog_memory(data, UA1000_MEMORY_LOAD);
}

/* app.user-set: the Store USER SET dialog. */
static void act_user_set(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	dialog_user_set(data);
}

/* app.reset: the Reset to Defaults dialog. */
static void act_reset(GSimpleAction *a, GVariant *p, gpointer data)
{
	(void)a;
	(void)p;
	dialog_reset(data);
}

/* Close the main window (and with it the application); also Ctrl+Q. */
static void act_quit(GSimpleAction *a, GVariant *p, gpointer data)
{
	struct app *app = data;

	(void)a;
	(void)p;
	gtk_window_close(app->window);
}

/* Register the Device menu's actions on the application, and Ctrl+Q. */
void toolbar_install_actions(struct app *app)
{
	static const GActionEntry actions[] = {
		{ .name = "patchbay", .activate = act_patchbay },
		{ .name = "notes", .activate = act_notes },
		{ .name = "sync", .activate = act_sync },
		{ .name = "connect", .activate = act_connect },
		{ .name = "store-memory", .activate = act_store_memory },
		{ .name = "load-memory", .activate = act_load_memory },
		{ .name = "user-set", .activate = act_user_set },
		{ .name = "reset", .activate = act_reset },
		{ .name = "quit", .activate = act_quit },
	};
	static const char *const quit_keys[] = { "<Control>q", NULL };

	g_action_map_add_action_entries(G_ACTION_MAP(app->gtk), actions,
					G_N_ELEMENTS(actions), app);
	gtk_application_set_accels_for_action(app->gtk, "app.quit", quit_keys);
}
