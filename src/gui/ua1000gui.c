/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ua1000gui.c - control panel for the Roland/EDIROL UA-1000
 *
 * The program's entry point, and nothing else: it reads its own options
 * (-p, -s), loads the state file through app_load(), and runs a
 * GtkApplication whose activate builds the window (app_build()) and whose
 * shutdown saves and releases it (app_shutdown()).  The panel itself, the
 * device link and the state file are app.c's and the modules it calls.
 *
 * Every run is its own instance (G_APPLICATION_NON_UNIQUE) and GTK never
 * sees the command line beyond the program's name.
 */
#include "app.h"

#include <string.h>

#define APP_ID "org.nylarea.ua1000gui"
#define VERSION "0.2"

/* "activate": build the window the first time, then raise it. */
static void on_activate(GtkApplication *gtk, gpointer data)
{
	struct app *app = data;

	(void)gtk;
	if (!app->window)
		app_build(app);
	gtk_window_present(app->window);
}

/* "shutdown": save what is pending and release everything. */
static void on_shutdown(GApplication *gtk, gpointer data)
{
	(void)gtk;
	app_shutdown(data);
}

/* Print the usage to f; returns status, for main() to return. */
static int usage(FILE *f, int status)
{
	fputs("Usage: ua1000gui [-p PORT] [-s STATE_FILE]\n"
	      "Control panel for the Roland/EDIROL UA-1000.\n"
	      "\n"
	      "  -p, --port PORT    ALSA rawmidi port (default: found by "
	      "name)\n"
	      "  -s, --state FILE   state file (default: "
	      "$XDG_STATE_HOME/ua1000/state)\n"
	      "  -h, --help         show this help\n"
	      "  -V, --version      show the version\n",
	      f);
	return status;
}

/*
 * Parse the options, load the state file, then run the GtkApplication.
 * app is static, so it starts zeroed: no window, no device, no desk.
 */
int main(int argc, char **argv)
{
	static struct app app;
	const char *state_path = NULL;
	char err[256];
	int status;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0)
			return usage(stdout, 0);
		if (strcmp(argv[i], "-V") == 0 ||
		    strcmp(argv[i], "--version") == 0) {
			puts("ua1000gui " VERSION);
			return 0;
		}
		if ((strcmp(argv[i], "-p") == 0 ||
		     strcmp(argv[i], "--port") == 0) &&
		    i + 1 < argc)
			app.port_option = argv[++i];
		else if ((strcmp(argv[i], "-s") == 0 ||
			  strcmp(argv[i], "--state") == 0) &&
			 i + 1 < argc)
			state_path = argv[++i];
		else
			return usage(stderr, 2);
	}
	if (app_load(&app, state_path, err, sizeof(err)) != 0) {
		fprintf(stderr, "ua1000gui: %s\n", err);
		return 1;
	}

	/*
	 * Each run is its own instance: its options name its port and state
	 * file, which a running instance would otherwise ignore.
	 */
	app.gtk = gtk_application_new(APP_ID, G_APPLICATION_NON_UNIQUE);
	g_signal_connect(app.gtk, "activate", G_CALLBACK(on_activate), &app);
	g_signal_connect(app.gtk, "shutdown", G_CALLBACK(on_shutdown), &app);
	/* Our options are parsed already; GTK sees only the program name. */
	status = g_application_run(G_APPLICATION(app.gtk), 1, argv);
	g_object_unref(app.gtk);
	return status;
}
