/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * app.c - ua1000gui's core: the window, the device connection, the state file
 *
 * struct app holds the one struct ua1000_state the whole GUI shows.  Every
 * edit goes through app_apply(): an ua1000_edit_*() call has changed the
 * state, its messages are sent, a save is scheduled, and app_refresh() makes
 * every widget show the state again.  While app->refreshing is set, the
 * widgets' handlers must not turn those programmatic changes into edits.
 *
 * The device: app_connect() opens the control port, watches its input from
 * the main loop and asks the device for its current state.  The answer is
 * adopted (app_adopt_device_state()); without one within READ_TIMEOUT_MS the
 * saved state is sent instead.  From then on, every message the device sends
 * is decoded into the state.  Continuous edits (fader and pan drags) are
 * bracketed by the edit-gesture messages the vendor's panel sends.  A send
 * or receive error closes the connection and raises a notice; app_connect()
 * opens it again on request.
 *
 * The state file is written SAVE_DELAY_MS after the last change and watched:
 * what another program (ua1000ctl) writes there is loaded and shown.
 *
 * Notices are how the GUI tells the user about errors and changes it did not
 * make itself; this module prints nothing.  It builds the main window but
 * leaves the controls to mixer.c, toolbar.c, desk.c and patchbay.c.
 * Everything here runs on the GTK main thread.
 *
 * ua1000gui.c calls app_load(), app_build() and app_shutdown(); the other GUI
 * modules call the rest through app.h.
 */
#include "app.h"
#include "ua1000_statefile.h"

#include <errno.h>
#include <glib-unix.h>
#include <math.h>
#include <string.h>

/* Save this long after the last change, so a fader drag is one write. */
#define SAVE_DELAY_MS 300

/* A drag ends when its control has been still this long. */
#define GESTURE_IDLE_MS 250

/*
 * The state as the state file holds it, as a string to g_free(), or NULL
 * when no memory stream could be opened.  Kept in app->saved_text, so that
 * on_state_file_changed() can recognise the GUI's own saves.
 */
static gchar *state_text(const struct ua1000_state *state)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&buf, &len);

	if (!f)
		return NULL;
	ua1000_state_write(state, f);
	fclose(f);

	gchar *text = g_strndup(buf, len);

	free(buf);
	return text;
}

/* ---- Saving, and reloading what ua1000ctl changed ---- */

/*
 * Timeout callback (main loop): write the state file now.  On success the
 * text written is remembered and the state counts as known; on failure a
 * notice says so and the next edit tries again.  Always a one-shot.
 */
static gboolean save_now(gpointer data)
{
	struct app *app = data;
	int err;

	app->save_source = 0;
	err = ua1000_statefile_save(app->state_path, &app->state);
	if (err != 0) {
		gchar *text = g_strdup_printf("Could not save %s: %s",
					      app->state_path, strerror(-err));

		app_notice(app, text);
		g_free(text);
		return G_SOURCE_REMOVE;
	}
	g_free(app->saved_text);
	app->saved_text = state_text(&app->state);
	app->state_known = true;
	return G_SOURCE_REMOVE;
}

/* Restart the save delay: a drag of a control is one write. */
void app_schedule_save(struct app *app)
{
	if (app->save_source)
		g_source_remove(app->save_source);
	app->save_source = g_timeout_add(SAVE_DELAY_MS, save_now, app);
}

/* Write a pending save at once (at shutdown); nothing when none is pending. */
static void flush_save(struct app *app)
{
	if (app->save_source) {
		g_source_remove(app->save_source);
		save_now(app);
	}
}

/*
 * The state file monitor's "changed" handler: when another program has
 * written the state file, load it as the GUI's state and show it.  Only
 * events that end a write are considered; the file's contents, not the
 * event, tell the GUI's own saves from someone else's.
 */
static void on_state_file_changed(GFileMonitor *monitor, GFile *file,
				  GFile *other, GFileMonitorEvent event,
				  gpointer data)
{
	struct app *app = data;
	gchar *contents = NULL;
	struct ua1000_state loaded;
	bool existed;
	char err[256];

	(void)monitor;
	(void)file;
	(void)other;
	if (event != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT &&
	    event != G_FILE_MONITOR_EVENT_CREATED &&
	    event != G_FILE_MONITOR_EVENT_RENAMED &&
	    event != G_FILE_MONITOR_EVENT_MOVED_IN)
		return;
	/*
	 * Our own save: ignore it.  A save renames a temporary file over the
	 * state file, and the event may name the temporary one: read the
	 * state file itself.
	 */
	if (!g_file_get_contents(app->state_path, &contents, NULL, NULL))
		return;

	bool ours = app->saved_text && strcmp(contents, app->saved_text) == 0;

	g_free(contents);
	if (ours)
		return;
	/*
	 * Another program wrote it while an edit of ours waited to be saved.
	 * That program can write only while the panel does not hold the
	 * device (ua1000ctl is refused the port otherwise), so what it wrote
	 * is what it sent: the file wins, and the panel's unsaved edit, which
	 * never reached the device, is dropped and said so.  Saving over it
	 * would lose the device's state without a word.
	 */
	bool dropped = app->save_source != 0;

	if (dropped) {
		g_source_remove(app->save_source);
		app->save_source = 0;
	}
	if (ua1000_statefile_load(app->state_path, &loaded, &existed, err,
				  sizeof(err)) != 0) {
		app_notice(app, err);
		return;
	}
	/* The device rate is the driver's to report, not the file's. */
	loaded.panel.rate = app->state.panel.rate;
	app->state = loaded;
	app->state_known = true;
	g_free(app->saved_text);
	app->saved_text = state_text(&app->state);
	app_refresh(app);
	if (dropped)
		app_notice(app, "Another program changed the saved settings "
				"while the panel's last change waited to be "
				"saved: the panel now shows the saved "
				"settings, and that last change was dropped.");
}

/*
 * Watch the state file for writes by other programs.  When GIO cannot
 * monitor it, such writes are not seen until the next start.
 */
static void watch_state_file(struct app *app)
{
	GFile *file = g_file_new_for_path(app->state_path);

	app->file_monitor = g_file_monitor_file(
		file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
	g_object_unref(file);
	if (app->file_monitor)
		g_signal_connect(app->file_monitor, "changed",
				 G_CALLBACK(on_state_file_changed), app);
}

/* ---- The device ---- */

/* Whether the control port is open. */
bool app_connected(const struct app *app)
{
	return app->midi != NULL;
}

/*
 * Close the control port: cancel the pending reads and the input watch and
 * forget an open gesture (no end is sent).  err is 0 for a deliberate close;
 * otherwise a notice names the error.  The status shown is updated.
 */
static void disconnect(struct app *app, int err)
{
	g_clear_handle_id(&app->read_source, g_source_remove);
	g_clear_handle_id(&app->reread_source, g_source_remove);
	if (app->midi_source) {
		g_source_remove(app->midi_source);
		app->midi_source = 0;
	}
	ua1000_midi_close(app->midi);
	app->midi = NULL;
	app->gesture_open = false;
	if (err != 0) {
		gchar *text = g_strdup_printf("Lost the device: %s",
					      ua1000_midi_strerror(err));

		app_notice(app, text);
		g_free(text);
	}
	toolbar_set_status(app);
}

/* Send to the device, if connected; an error drops the connection. */
void app_send(struct app *app, const struct ua1000_msg *msg, size_t count)
{
	int err;

	if (!app->midi || count == 0)
		return;
	err = ua1000_midi_send(app->midi, msg, count);
	if (err != 0)
		disconnect(app, err);
}

/*
 * Adopt the device's state; when it differs from what was known, a notice
 * lists the differences.
 */
void app_adopt_device_state(struct app *app, const struct ua1000_state *device)
{
	struct ua1000_state before = app->state;
	struct ua1000_sync solos;
	bool differed = ua1000_state_adopt_device(&app->state, device, &solos);

	app_send(app, solos.msg, solos.count);
	if (differed && app->state_known) {
		char changes[1024];
		gchar *text;

		ua1000_state_describe_changes(&before, &app->state, changes,
					      sizeof(changes), 8);
		/* the front knob stops acting on all outputs but the main
		 * one: whatever plays there plays at full level (measured: it
		 * put music at full level on monitors on outputs 1-2) */
		const char *warning =
			before.panel.output_all && !app->state.panel.output_all
				? "WARNING: the front panel's OUTPUT VOLUME "
				  "knob now acts on the main output only "
				  "(ALL is off): the other outputs play at "
				  "full level. Turn ALL on before playing "
				  "anything.\n\n"
				: "";

		if (!app->read_on_connect && app->reread_slot)
			text = g_strdup_printf("%sMemory %u loaded these "
					       "settings, now shown:\n\n%s",
					       warning, app->reread_slot,
					       changes);
		else
			text = g_strdup_printf(
				"%sThe device's settings differed from the "
				"ones "
				"saved when the panel last closed (changed on "
				"the device, or by another program). The panel "
				"now shows the device's:\n\n%s",
				warning, changes);
		app_notice(app, text);
		g_free(text);
	}
	app->reread_slot = 0;
	app->state_known = true;
	app_schedule_save(app);
	app_refresh(app);
}

/*
 * When the device does not answer the request for its state, what the
 * panel did before it could ask: bring the device back to the saved state;
 * on the first run, or after loading a memory it never stored, leave the
 * device alone and say so.  Called by on_read_timeout() for the request made
 * on connecting.
 */
static void connect_without_reading(struct app *app)
{
	if (app->state_known && !app->state.mixer_unknown) {
		app_sync(app); /* it refreshes the widgets */
		return;
	}
	if (app->state_known) {
		/* a memory the editor never stored was loaded: sending the
		 * shown mixer would overwrite the device's without a word */
		app_notice(app,
			   "A memory this editor did not store was loaded: the "
			   "faders do not show the device's mixer, so the "
			   "device is left alone.  Device > Send All Settings "
			   "makes the device match the panel.");
	} else {
		app_notice(app,
			   "The editor does not know this device's settings "
			   "yet, and shows its defaults.  Device > Send All "
			   "Settings makes the device match them.");
	}
	app_refresh(app);
}

/* The device answered in 0.01 s when measured; a second is ample. */
#define READ_TIMEOUT_MS 1000

/*
 * Timeout callback: the device did not answer a state request in time.
 * Only the request made on connecting falls back to the saved state; an
 * unanswered read after a memory load is left at that.
 */
static gboolean on_read_timeout(gpointer data)
{
	struct app *app = data;

	app->read_source = 0;
	if (app->read_on_connect)
		connect_without_reading(app);
	return G_SOURCE_REMOVE;
}

/*
 * Ask the device for its state; on_device_message() takes the answer.
 * on_connect says what happens when no answer comes (on_read_timeout()).
 * A timeout already armed is replaced.
 */
static void request_device_state(struct app *app, bool on_connect)
{
	struct ua1000_msg request;

	ua1000_build_block_request(&request, UA1000_BLOCK_CURRENT);
	app_send(app, &request, 1);
	if (!app->midi)
		return; /* the send lost it */
	app->read_on_connect = on_connect;
	g_clear_handle_id(&app->read_source, g_source_remove);
	app->read_source = g_timeout_add(READ_TIMEOUT_MS, on_read_timeout, app);
}

/*
 * After a load the device dumps its panel block, at the same address as
 * the answer to a request but with no mixer in it (docs/PROTOCOL.md): the
 * request waits until that dump has come (within a second, as ua1000ctl
 * waits for it), so that only the answer is taken as the device's state.
 */
#define REREAD_DELAY_MS 1000

/* Timeout callback: the load's dump has had time to come; read the device. */
static gboolean on_reread(gpointer data)
{
	struct app *app = data;

	app->reread_source = 0;
	if (app->midi)
		request_device_state(app, false);
	return G_SOURCE_REMOVE;
}

/* Arm the delayed read that follows a memory load. */
void app_reread_device_later(struct app *app, unsigned int slot)
{
	app->reread_slot = slot;
	g_clear_handle_id(&app->reread_source, g_source_remove);
	app->reread_source = g_timeout_add(REREAD_DELAY_MS, on_reread, app);
}

/*
 * ua1000_midi_receive()'s handler, called from on_midi_input() for each
 * complete DT1 message from the device.  While a state request is
 * outstanding, a message that parses as the current block is its answer;
 * any other message reports a change made on the device (its front panel,
 * a memory load) and is applied to the state.
 */
static void on_device_message(const struct ua1000_dt1 *dt1, void *data)
{
	struct app *app = data;
	struct ua1000_event event = { 0 };

	if (app->read_source) {
		struct ua1000_state device;

		if (ua1000_state_from_block(&device, dt1) == 0) {
			g_clear_handle_id(&app->read_source, g_source_remove);
			app_adopt_device_state(app, &device);
			return;
		}
	}

	/* the decoder changes only what the message carries */
	event.panel = app->state.panel;
	event.patchbay = app->state.patchbay;
	ua1000_decode(dt1, &event);
	if (event.type == UA1000_EVENT_UNKNOWN)
		return;
	ua1000_state_apply_event(&app->state, &event);
	app_schedule_save(app);
	app_refresh(app);
}

/*
 * g_unix_fd_add() callback for the control port's input: handle every
 * message waiting.  An error, or a hang-up of the descriptor (the device
 * unplugged), disconnects; midi_source is cleared first because returning
 * G_SOURCE_REMOVE removes this source already.
 */
static gboolean on_midi_input(gint fd, GIOCondition condition, gpointer data)
{
	struct app *app = data;
	int err;

	(void)fd;
	if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		app->midi_source = 0;
		disconnect(app, -ENODEV);
		return G_SOURCE_REMOVE;
	}
	err = ua1000_midi_receive(app->midi, on_device_message, app);
	if (err != 0) {
		app->midi_source = 0;
		disconnect(app, err);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/* (Re)open the control port and start reading the device's state. */
void app_connect(struct app *app)
{
	char port[32];
	const char *name = app->port_option;
	struct pollfd fd;
	unsigned int hz;
	int err, n;

	if (app->midi)
		disconnect(app, 0);
	if (!name) {
		err = ua1000_midi_find(port, sizeof(port));
		if (err != 0) {
			app_notice(app, ua1000_midi_strerror(err));
			toolbar_set_status(app);
			return;
		}
		name = port;
	}
	err = ua1000_midi_open(&app->midi, name, true);
	if (err != 0) {
		gchar *text = g_strdup_printf("%s: %s", name,
					      ua1000_midi_strerror(err));

		app_notice(app, text);
		g_free(text);
		app->midi = NULL;
		toolbar_set_status(app);
		return;
	}
	/* A rawmidi input has one descriptor. */
	n = ua1000_midi_poll_descriptors(app->midi, &fd, 1);
	if (n > 0) {
		/* GIOCondition is a set of flags, not one of its values. */
		/* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
		GIOCondition events = G_IO_IN | G_IO_ERR | G_IO_HUP;
		/* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */

		app->midi_source =
			g_unix_fd_add(fd.fd, events, on_midi_input, app);
	}
	if (ua1000_midi_card_rate(&hz) == 0)
		ua1000_rate_from_hz(hz, &app->state.panel.rate);

	/*
	 * Ask the device for its state: it answers with everything but the
	 * solos (docs/PROTOCOL.md, Reading the device), and that is what the
	 * panel shows.  Without an answer, the saved state is sent.
	 */
	toolbar_set_status(app);
	request_device_state(app, true);
}

/* ---- Edits ---- */

/* Send the begin or end of an edit gesture and record whether one is open. */
static void send_gesture(struct app *app, bool begin)
{
	struct ua1000_msg msg;

	ua1000_build_edit_gesture(&msg, begin);
	app_send(app, &msg, 1);
	app->gesture_open = begin;
}

/* End the open drag now: disarm its timeout, then send "end". */
static void end_gesture(struct app *app)
{
	g_clear_handle_id(&app->gesture_source, g_source_remove);
	if (app->gesture_open)
		send_gesture(app, false);
}

/* Timeout callback: the dragged control has been still: the gesture ends. */
static gboolean on_gesture_idle(gpointer data)
{
	struct app *app = data;

	app->gesture_source = 0;
	end_gesture(app);
	return G_SOURCE_REMOVE;
}

/*
 * Finish a control's edit: send it, bracketed in a gesture when continuous,
 * save and refresh.
 */
void app_apply(struct app *app, int ret, const struct ua1000_edit *edit,
	       bool continuous)
{
	if (ret != 0) {
		/* The state refused it: put the widget back. */
		app_refresh(app);
		return;
	}
	if (continuous && app->midi) {
		if (!app->gesture_open)
			send_gesture(app, true);
		g_clear_handle_id(&app->gesture_source, g_source_remove);
		app->gesture_source =
			g_timeout_add(GESTURE_IDLE_MS, on_gesture_idle, app);
	} else {
		end_gesture(app);
	}
	app_send(app, edit->msg, edit->count);
	app_schedule_save(app);
	/* A linked partner, or a preset, changes other widgets too. */
	app_refresh(app);
}

/* Send the whole state, the gesture closed first. */
void app_sync(struct app *app)
{
	struct ua1000_sync sync;

	end_gesture(app);
	ua1000_state_sync(&app->state, &sync);
	/* the device now has the mixer the panel shows */
	app->state.mixer_unknown = false;
	app_send(app, sync.msg, sync.count);
	app_schedule_save(app);
	app_refresh(app);
}

/* ---- The window ---- */

/*
 * A notice is a small window over the panel, in the panel's colours, not
 * a bar across the top.  A newer notice replaces the text
 * of one already shown.  Before the main window is on screen, the notice
 * waits for it, so it never appears on its own.
 */

/* OK's "clicked" handler: close the notice. */
static void on_notice_ok(GtkButton *button, gpointer data)
{
	struct app *app = data;

	(void)button;
	if (app->notice_window)
		gtk_window_destroy(GTK_WINDOW(app->notice_window));
}

/* The notice window's "destroy" handler: the next notice makes a new one. */
static void on_notice_destroy(GtkWidget *window, gpointer data)
{
	struct app *app = data;

	(void)window;
	app->notice_window = NULL;
	app->notice_label = NULL;
}

/*
 * Build the notice window, not yet shown, transient for the main window: a
 * title, the text (wrapped, selectable) and OK.  It is kept in
 * app->notice_window and its text label in app->notice_label.
 */
static void notice_window_new(struct app *app)
{
	GtkWidget *window = gtk_window_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	GtkWidget *title = gtk_label_new("UA-1000 SIDECARD");
	GtkWidget *ok = gtk_button_new_with_label("OK");
	GtkShortcutController *keys =
		GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());

	gtk_window_set_title(GTK_WINDOW(window), "UA-1000 Sidecard");
	gtk_window_set_transient_for(GTK_WINDOW(window), app->window);
	gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
	gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	gtk_widget_add_css_class(window, "notice-card");
	gtk_widget_add_css_class(title, "notice-title");
	gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
	app->notice_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(app->notice_label, "notice-text");
	gtk_label_set_wrap(GTK_LABEL(app->notice_label), TRUE);
	gtk_label_set_max_width_chars(GTK_LABEL(app->notice_label), 52);
	gtk_label_set_xalign(GTK_LABEL(app->notice_label), 0.0f);
	gtk_label_set_selectable(GTK_LABEL(app->notice_label), TRUE);
	gtk_widget_set_halign(ok, GTK_ALIGN_END);
	g_signal_connect(ok, "clicked", G_CALLBACK(on_notice_ok), app);
	/* Escape and Return close it, as a dialog's would */
	gtk_shortcut_controller_add_shortcut(
		keys,
		gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
				 gtk_named_action_new("window.close")));
	gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(keys));
	gtk_window_set_default_widget(GTK_WINDOW(window), ok);
	gtk_box_append(GTK_BOX(box), title);
	gtk_box_append(GTK_BOX(box), app->notice_label);
	gtk_box_append(GTK_BOX(box), ok);
	gtk_window_set_child(GTK_WINDOW(window), box);
	g_signal_connect(window, "destroy", G_CALLBACK(on_notice_destroy), app);
	app->notice_window = window;
}

/* Show text in the notice window, made if none is up, focus on OK. */
static void notice_show(struct app *app, const char *text)
{
	if (!app->notice_window)
		notice_window_new(app);
	gtk_label_set_text(GTK_LABEL(app->notice_label), text);
	gtk_window_present(GTK_WINDOW(app->notice_window));
	/* the focus on OK: a selectable label that took it would show its
	 * whole text selected */
	gtk_window_set_focus(
		GTK_WINDOW(app->notice_window),
		gtk_window_get_default_widget(GTK_WINDOW(app->notice_window)));
	gtk_label_select_region(GTK_LABEL(app->notice_label), 0, 0);
}

/* Show a notice, or keep it until the main window is mapped. */
void app_notice(struct app *app, const char *text)
{
	if (!app->window)
		return;
	if (!gtk_widget_get_mapped(GTK_WIDGET(app->window))) {
		g_free(app->notice_pending);
		app->notice_pending = g_strdup(text);
		return;
	}
	notice_show(app, text);
}

/*
 * The main window's "map" handler: it is on screen, so a notice that waited
 * for it is shown now.
 */
static void on_window_map(GtkWidget *window, gpointer data)
{
	struct app *app = data;

	(void)window;
	if (app->notice_pending) {
		notice_show(app, app->notice_pending);
		g_clear_pointer(&app->notice_pending, g_free);
	}
}

/*
 * Make every widget show the state; the widgets' handlers see
 * app->refreshing and send nothing.
 */
void app_refresh(struct app *app)
{
	if (!app->window)
		return;
	app->refreshing = true;
	mixer_refresh(app);
	toolbar_refresh(app);
	desk_refresh(app);
	patchbay_refresh(app);
	app->refreshing = false;
}

/*
 * Install the application's style sheet on the default display: the
 * mute, solo and link colours, the desk's dark window, the notice card, the
 * notepad's ink and the connection status.
 */
static void load_css(void)
{
	static const char css[] =
		"togglebutton.channel-mute:checked { background: #b8322c; "
		"color: white; }\n"
		"togglebutton.channel-solo:checked { background: #d07a00; "
		"color: white; }\n"
		"togglebutton.channel-link:checked { background: #2a6fb5; "
		"color: white; }\n"
		".channel-name { font-weight: bold; }\n"
		".small { font-size: smaller; }\n"
		"window.desk-mode, window.desk-mode > box { background: "
		"#101215; color: #e7e5ef; }\n"
		/* a notice: a card in the desk's colours (PALETTE.md) */
		"window.notice-card { background: #16191d; color: #e7e5ef; "
		"border: 1px solid #3d58a3; border-radius: 8px; }\n"
		".notice-card > box { padding: 18px 22px; }\n"
		".notice-title { color: #9093ae; font-weight: bold; "
		"letter-spacing: 2px; }\n"
		".notice-text { font-size: 16px; color: #e7e5ef; }\n"
		".notice-card button { background: #3d58a3; color: #e7e5ef; "
		"border: none; border-radius: 4px; padding: 6px 22px; "
		"font-weight: bold; }\n"
		".notice-card button:hover { background: #4c6ab8; }\n"
		".connected { color: @success_color; font-weight: bold; }\n"
		/* the notepad: the editor writes on the rendered page */
		".notes-on-page, .notes-on-page textview, .notes-on-page text "
		"{ background: transparent; }\n"
		"textview.notes { font-size: 17px; }\n"
		/* ink on the paper: the dark theme the desk asks for would
		 * write white on it */
		".notes-on-page textview.notes, "
		".notes-on-page textview.notes text { color: #23262c; "
		"caret-color: #23262c; }\n"
		".notes-on-page textview.notes text selection "
		"{ background: rgba(61, 88, 163, 0.30); color: #23262c; }\n"
		".disconnected { color: @error_color; font-weight: bold; }\n";
	GtkCssProvider *provider = gtk_css_provider_new();

	gtk_css_provider_load_from_string(provider, css);
	gtk_style_context_add_provider_for_display(
		gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(provider);
}

/*
 * The main window's "destroy" handler.  Once the window is gone, nothing may
 * touch its widgets: the desk stops drawing, and the other windows (the
 * patch bay is otherwise only ever hidden) go with it.
 */
static void on_window_destroy(GtkWidget *window, gpointer data)
{
	struct app *app = data;

	(void)window;
	app->window = NULL;
	desk_detach(app);
	if (app->patchbay.window)
		gtk_window_destroy(GTK_WINDOW(app->patchbay.window));
	app->patchbay.window = NULL;
	if (app->notes.window)
		gtk_window_destroy(GTK_WINDOW(app->notes.window));
	if (app->notice_window)
		gtk_window_destroy(GTK_WINDOW(app->notice_window));
	g_clear_pointer(&app->notice_pending, g_free);
}

/* Load an image file into a cairo surface of the given device scale. */
cairo_surface_t *app_load_image(const char *path, double scale)
{
	GdkTexture *texture = gdk_texture_new_from_filename(path, NULL);
	cairo_surface_t *s;

	if (!texture)
		return NULL;
	s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
				       gdk_texture_get_width(texture),
				       gdk_texture_get_height(texture));
	if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(s);
		g_object_unref(texture);
		return NULL;
	}
	/* GDK_MEMORY_DEFAULT is cairo's ARGB32: premultiplied, native order */
	gdk_texture_download(texture, cairo_image_surface_get_data(s),
			     (gsize)cairo_image_surface_get_stride(s));
	cairo_surface_mark_dirty(s);
	cairo_surface_set_device_scale(s, scale, scale);
	g_object_unref(texture);
	return s;
}

/* The installed data's directory; the Makefile defines it from DATADIR. */
#ifndef SIDECARD_DATADIR
#define SIDECARD_DATADIR "/usr/local/share/sidecard"
#endif

/*
 * The desk panel's images: $SIDECARD_DESK if set (empty: the plain layout),
 * else the installed ones.  Without them the GUI uses the plain layout.
 */
static const char *desk_dir(void)
{
	const char *env = g_getenv("SIDECARD_DESK");

	if (env)
		return *env ? env : NULL;
	return SIDECARD_DATADIR "/desk";
}

/*
 * GTK 4.20 replaced gtk-application-prefer-dark-theme, which now warns on
 * every launch, with gtk-interface-color-scheme: the new one where this GTK
 * has it (found at run time, its "dark" value by name), else the old one.
 */
static void dark_theme(void)
{
	GtkSettings *s = gtk_settings_get_default();
	GParamSpec *p = g_object_class_find_property(
		G_OBJECT_GET_CLASS(s), "gtk-interface-color-scheme");

	if (p && G_IS_PARAM_SPEC_ENUM(p)) {
		GEnumClass *e = G_PARAM_SPEC_ENUM(p)->enum_class;
		GEnumValue *dark = g_enum_get_value_by_nick(e, "dark");

		if (dark) {
			g_object_set(s, "gtk-interface-color-scheme",
				     dark->value, NULL);
			return;
		}
	}
	g_object_set(s, "gtk-application-prefer-dark-theme", TRUE, NULL);
}

/*
 * At most 85 % of the first monitor, the panel's proportions kept: a window
 * asking for more than the screen is maximised by GNOME (the
 * 2059 px panel always opened full screen).
 */
static void fit_to_screen(int *w, int *h)
{
	GListModel *monitors =
		gdk_display_get_monitors(gdk_display_get_default());
	GdkMonitor *m = monitors ? g_list_model_get_item(monitors, 0) : NULL;
	GdkRectangle area;
	double k;

	if (!m)
		return;
	gdk_monitor_get_geometry(m, &area);
	g_object_unref(m);
	k = fmin(1.0, fmin(0.85 * area.width / *w, 0.85 * area.height / *h));
	*w = (int)lround(*w * k);
	*h = (int)lround(*h * k);
}

/* Build the main window, watch the state file and connect to the device. */
void app_build(struct app *app)
{
	load_css();
	toolbar_install_actions(app);

	GtkWidget *window = gtk_application_window_new(app->gtk);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	const char *dir = desk_dir();
	GtkWidget *desk;

	app->window = GTK_WINDOW(window);
	desk = dir ? desk_new(app, dir) : NULL;
	app->desk_dir = desk ? dir : NULL;
	gtk_window_set_title(app->window, "UA-1000 Sidecard");
	/* the installed icon (make install), named as the application */
	gtk_window_set_default_icon_name(
		g_application_get_application_id(G_APPLICATION(app->gtk)));
	gtk_window_set_child(app->window, box);
	g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), app);
	g_signal_connect(window, "map", G_CALLBACK(on_window_map), app);
	if (desk) {
		/* the desk is dark: so are its window's title bar, menus and
		 * dialogs, not the grey of GTK's own theme */
		dark_theme();
		int w, h;

		gtk_widget_add_css_class(window, "desk-mode");
		/* any size, the panel keeping its proportions */
		desk_natural_size(app, &w, &h);
		fit_to_screen(&w, &h);
		gtk_window_set_default_size(app->window, w, h);
		/* the whole window is the desk: the Device menu and the
		 * connection status are on its panel */
		gtk_box_append(GTK_BOX(box), desk);
	} else {
		gtk_window_set_default_size(app->window, 1240, 560);
		gtk_box_append(GTK_BOX(box), mixer_new(app));
		gtk_box_append(GTK_BOX(box), toolbar_new(app));
	}

	watch_state_file(app);
	app_connect(app);
	app_refresh(app);
}

/* Leave the device and the state file in order, then free the GUI. */
void app_shutdown(struct app *app)
{
	end_gesture(app);
	/* the state first: a device that stopped answering must not cost it */
	flush_save(app);
	if (app->midi)
		ua1000_midi_drain(app->midi);
	disconnect(app, 0);
	g_clear_object(&app->file_monitor);
	g_free(app->saved_text);
	app->saved_text = NULL;
	notes_shutdown(app);
	desk_free(app);
}

/* Pick the state file's path and load the state from it. */
int app_load(struct app *app, const char *state_path, char *err, size_t err_len)
{
	bool existed;
	int ret;

	if (state_path) {
		g_strlcpy(app->state_path, state_path, sizeof(app->state_path));
	} else {
		ret = ua1000_statefile_default_path(app->state_path,
						    sizeof(app->state_path));
		if (ret != 0) {
			snprintf(err, err_len, "no state file: %s",
				 strerror(-ret));
			return ret;
		}
	}
	ret = ua1000_statefile_load(app->state_path, &app->state, &existed, err,
				    err_len);
	if (ret != 0)
		return ret;
	app->state_known = existed;
	app->saved_text = existed ? state_text(&app->state) : NULL;
	return 0;
}
