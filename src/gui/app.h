/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * app.h - the GUI's shared state and the functions its parts call
 *
 * The GUI is a view of struct ua1000_state.  Every control calls an
 * ua1000_edit_*() function through app_apply(), which sends the resulting
 * messages and schedules a save; app_refresh() then makes every widget show
 * the state.  Widgets never hold settings of their own.
 */
#ifndef UA1000_GUI_APP_H
#define UA1000_GUI_APP_H

#include "ua1000_midi.h"
#include "ua1000_state.h"

#include <gtk/gtk.h>

/* One channel's controls in a mixer, made by mixer_create(). */
struct strip_widgets {
	GtkWidget *mute; /* toggle: active when muted ("M") */
	GtkWidget *solo; /* toggle */
	GtkWidget *pan; /* GtkScale, -20..20 */
	GtkWidget *fader; /* GtkScale, fader positions 0..1022 */
};

struct desk; /* desk.c: the panel drawn from renders, when installed */

struct patchbay_widgets {
	GtkWidget *window; /* hidden, not destroyed, when closed */
	GtkWidget *route[3][UA1000_PAIRS]; /* per routing table and pair */
};

struct notes_widgets {
	GtkWidget *window; /* destroyed when closed */
	GtkTextBuffer *buffer;
	guint save_source;
	bool dirty; /* changed since the last save */
	/* A notes file that was not UTF-8: its bytes, kept to be written to
	 * "notes.orig" before the first save replaces it; NULL otherwise. */
	gchar *original;
	gsize original_len;
};

struct app {
	GtkApplication *gtk;
	GtkWindow *window; /* the main window; NULL before app_build(), and
			    * again once it is destroyed */

	struct ua1000_state state;
	bool state_known; /* false until a state file exists */
	char state_path[4096];
	char *saved_text; /* the state file as we last wrote it */
	guint save_source; /* the pending save's timeout, or 0 */
	GFileMonitor *file_monitor; /* the state file's; NULL if none */

	const char *port_option; /* -p, or NULL to find the device */
	struct ua1000_midi *midi; /* the control port; NULL: not connected */
	guint midi_source; /* the watch on its input, or 0 */
	/* Inside ua1000_midi_receive(): a disconnect then waits for the read
	 * loop to return (disconnect_pending, with its error). */
	bool receiving;
	bool disconnect_pending;
	int disconnect_err;
	/* On connecting, the device was asked for its state: the answer is
	 * awaited until this timeout (then the saved state is sent instead). */
	guint read_source;
	bool read_on_connect; /* the timeout then sends the saved state */
	guint reread_source; /* a read after a memory load, once its dump came */
	unsigned int reread_slot; /* that load's memory, for the notice */

	/* The vendor's panel brackets each drag of a fader or pan knob. */
	bool gesture_open; /* a begin was sent, its end not yet */
	guint gesture_source; /* the timeout that ends it, or 0 */

	/* Set while widgets are being made to show the state. */
	bool refreshing;

	struct strip_widgets strip[2][UA1000_CHANNELS];
	GtkWidget *link[2][UA1000_PAIRS];
	GtkWidget *monitor_fader[2]; /* indexed by ua1000_side */
	GtkWidget *monitor_link;

	/* The plain layout's bar (toolbar.c); NULL on the desk. */
	GtkWidget *clock;
	GtkWidget *digital_out;
	GtkWidget *mono;
	GtkWidget *soft_ctrl;
	GtkWidget *output_all;
	GtkWidget *rate_label;
	GtkWidget *status;

	GtkWidget *notice_window; /* shown while a notice is up */
	GtkWidget *notice_label; /* its text */
	char *notice_pending; /* a notice waiting for the main window */

	struct patchbay_widgets patchbay;
	struct desk *desk; /* NULL: the plain GTK layout */
	const char *desk_dir; /* the desk's images, or NULL */
	struct notes_widgets notes;
};

/* app.c */
/*
 * Set the state file's path (state_path, or the default one when NULL) and
 * load the state from it; a missing file leaves the editor's defaults.
 * Returns 0, or a negative errno with a description in err.
 */
int app_load(struct app *app, const char *state_path, char *err,
	     size_t err_len);
/*
 * Build the main window (the desk when its images load, else the plain
 * layout), watch the state file and connect to the device.  Once, on the
 * application's activation.
 */
void app_build(struct app *app);
/*
 * End an open gesture, write a pending save, let the device take what was
 * sent, disconnect, and release the file monitor, the notes and the desk.
 * On the application's shutdown.
 */
void app_shutdown(struct app *app);
/*
 * Finish an edit a control made: ret is its ua1000_edit_*() result.  When
 * the state refused it (ret != 0), the widgets are put back; otherwise the
 * edit's messages are sent, a save is scheduled and the widgets refreshed.
 * continuous marks a drag (fader, pan): its edits are bracketed by one edit
 * gesture, ended when the control has been still for a moment or by the
 * next edit that is not continuous.
 */
void app_apply(struct app *app, int ret, const struct ua1000_edit *edit,
	       bool continuous);
/* Save the state file shortly after the last call (each call restarts the
 * delay). */
void app_schedule_save(struct app *app);
/* Send messages to the device, if connected.  A send error disconnects and
 * raises a notice. */
void app_send(struct app *app, const struct ua1000_msg *msg, size_t count);
/* Take the state the device reported (its answer to a block request): the
 * panel shows it, saves it, and gives the device the panel's solos. */

void app_adopt_device_state(struct app *app, const struct ua1000_state *device);

/* After a memory load: read the device once its own dump has come, so the
 * panel shows what the loaded memory holds. */
void app_reread_device_later(struct app *app, unsigned int slot);

/* Send the whole state to the device, save it and show it. */
void app_sync(struct app *app);
/*
 * (Re)open the control port (app->port_option, or the one found by name),
 * watch its input, take the card's sample rate, and ask the device for its
 * state; without an answer the saved state is sent.  A failure raises a
 * notice and leaves the GUI disconnected.
 */
void app_connect(struct app *app);
/*
 * Tell the user something, in a small window over the main window.  A newer
 * notice replaces the text of one shown.  Before the main window is mapped,
 * the latest notice waits for it; without a main window it is dropped.
 */
void app_notice(struct app *app, const char *text);
/* Whether the device's control port is open. */
bool app_connected(const struct app *app);

/* A PNG or JPEG as a cairo surface drawn at its 1x size (a 2x image gets
 * a device scale of 2), or NULL. */
cairo_surface_t *app_load_image(const char *path, double scale);

/* mixer.c */
/* Create every strip, link and monitor control, wired to the state. */
void mixer_create(struct app *app);
/*
 * True when this press on a control follows the previous one within the
 * desktop's double-click time.  GTK's own click count is not used: a range
 * claims the first press, which resets the count (a double
 * click on a fader did nothing).
 */
bool app_double_press(GObject *control);
/* mixer_create(), laid out with GTK's own widgets. */
GtkWidget *mixer_new(struct app *app);
/* Make the mixer's widgets show app->state (from app_refresh()). */
void mixer_refresh(struct app *app);
/* A pan position (-20..20) as the panel writes it: "L5", "C", "R12". */
char *mixer_pan_text(double position);

/* toolbar.c */
/* The plain layout's bar: the panel switches, the rate, PATCH BAY, the
 * Device menu and the status (the desk has them on its panel). */
GtkWidget *toolbar_new(struct app *app);
void toolbar_refresh(struct app *app);
void toolbar_set_status(struct app *app);
/* "Connected" or "Not connected": the toolbar's label, the desk's legend. */
const char *toolbar_status_text(const struct app *app);
/* The Device menu: the toolbar's button and the desk's DEVICE button. */
GMenuModel *toolbar_device_menu(void);
void toolbar_install_actions(struct app *app);
/* Edit the panel switches (ignored while refreshing). */
void toolbar_edit_panel(struct app *app, const struct ua1000_panel *panel);
void toolbar_set_clock(struct app *app, enum ua1000_clock clock);

/* patchbay.c */
void patchbay_present(struct app *app);
void patchbay_refresh(struct app *app);

/* desk.c */
/*
 * The panel as the renders of gui/scene draw it, from the assets in dir
 * (desk.layout, plate.jpg, lit.jpg, cap_*.png, per scale).  NULL when they
 * are missing or do not parse: the caller falls back to the plain layout.
 */
GtkWidget *desk_new(struct app *app, const char *dir);
void desk_refresh(struct app *app);
/* Repaint the desk (the connection status lives on it). */
void desk_redraw(struct app *app);
/* The panel's size at 1x, in pixels: the window's default. */
void desk_natural_size(const struct app *app, int *width, int *height);
/* The main window is going: stop drawing on the desk's widgets. */
void desk_detach(struct app *app);
void desk_free(struct app *app);

/* notes.c */
void notes_present(struct app *app);
/* Save what is unsaved (at shutdown). */
void notes_shutdown(struct app *app);

/* dialogs.c */
void dialog_memory(struct app *app, enum ua1000_memory_op op);
void dialog_user_set(struct app *app);
/* Store the USER SET at hz (44100..96000) and show how it takes effect. */
bool app_store_user_set(struct app *app, unsigned int hz);
void dialog_reset(struct app *app);

/* Make every widget show app->state. */
void app_refresh(struct app *app);

#endif /* UA1000_GUI_APP_H */
