/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * notes.c - one note about the device, kept beside the state file
 *
 * The idea: the units wired to the UA-1000's rear
 * connectors are forgotten, and finding out means unracking them.  NOTES
 * opens a notepad with a single note of any length, which saves itself a
 * moment after the last change and when the window closes.
 *
 * With the desk's images, the editor sits on a rendered notepad page
 * (notebook.jpg and notebook.layout, from gui/scene); without them, it is a
 * plain window.  The text is an ordinary GtkTextView: selection, clipboard,
 * undo and scrolling behave as in any editor.
 *
 * The note lives in its own file, "notes" in the state file's directory,
 * written whole and atomically; nothing else in the editor reads it, and
 * it is not sent to the device.  struct notes_widgets (app->notes) holds
 * the window, its buffer, the pending save and whether the text is
 * unsaved; window and buffer are NULL while the window is closed (it is
 * destroyed, not hidden).
 *
 * Used by toolbar.c (the app.notes action) and by app.c at shutdown.
 */
#include "app.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Quiet time after the last change before the note is written. */
#define NOTES_SAVE_DELAY_MS 500

/* $XDG_STATE_HOME/ua1000/notes, or "notes" beside a state file given by -s. */
static void notes_path(const struct app *app, char *path, size_t len)
{
	gchar *dir = g_path_get_dirname(app->state_path);

	snprintf(path, len, "%s/notes", dir);
	g_free(dir);
}

/*
 * Before the first save over a notes file that was not UTF-8: write its
 * bytes, untouched, to "<path>.orig" (never over an existing one), so that
 * nothing the file held is lost.  Returns false, with a notice, when that
 * copy cannot be made: then the notes file is not replaced either.
 */
static bool keep_original(struct app *app, const char *path)
{
	struct notes_widgets *n = &app->notes;
	gchar *orig = g_strdup_printf("%s.orig", path);
	GError *error = NULL;
	bool ok = true;

	if (!g_file_test(orig, G_FILE_TEST_EXISTS) &&
	    !g_file_set_contents(orig, n->original, (gssize)n->original_len,
				 &error)) {
		app_notice(app, error->message);
		g_error_free(error);
		ok = false;
	}
	if (ok)
		g_clear_pointer(&n->original, g_free);
	g_free(orig);
	return ok;
}

/*
 * Write the note if it is unsaved, cancelling a pending timed save, and
 * create the directory if needed.  A failure is shown as a notice and the
 * note stays unsaved, so the next change or close tries again.
 */
static void save_now(struct app *app)
{
	struct notes_widgets *n = &app->notes;
	GtkTextIter start, end;
	gchar *text, *dir;
	GError *error = NULL;
	char path[4096];

	if (n->save_source) {
		g_source_remove(n->save_source);
		n->save_source = 0;
	}
	if (!n->buffer || !n->dirty)
		return;
	notes_path(app, path, sizeof(path));
	dir = g_path_get_dirname(path);
	g_mkdir_with_parents(dir, 0700);
	g_free(dir);
	if (n->original && !keep_original(app, path))
		return; /* not saved: the original would be lost */
	gtk_text_buffer_get_bounds(n->buffer, &start, &end);
	text = gtk_text_buffer_get_text(n->buffer, &start, &end, FALSE);
	/* g_file_set_contents() writes a temporary file and renames it */
	if (g_file_set_contents(path, text, -1, &error)) {
		n->dirty = false;
	} else {
		app_notice(app, error->message);
		g_error_free(error);
	}
	g_free(text);
}

/* The timed save, NOTES_SAVE_DELAY_MS after the last change. */
static gboolean on_save_timeout(gpointer data)
{
	struct app *app = data;

	app->notes.save_source = 0;
	save_now(app);
	return G_SOURCE_REMOVE;
}

/* The buffer's "changed" handler: mark unsaved and restart the timer. */
static void on_changed(GtkTextBuffer *buffer, gpointer data)
{
	struct app *app = data;
	struct notes_widgets *n = &app->notes;

	(void)buffer;
	n->dirty = true;
	if (n->save_source)
		g_source_remove(n->save_source);
	n->save_source =
		g_timeout_add(NOTES_SAVE_DELAY_MS, on_save_timeout, app);
}

/* Save before the window closes; FALSE lets it close. */
static gboolean on_close_request(GtkWindow *window, gpointer data)
{
	(void)window;
	save_now(data);
	return FALSE;
}

/*
 * The window's "destroy" handler, also reached when the main window takes
 * it along: save, and forget the window and its buffer so that
 * notes_present() builds a new one.
 */
static void on_destroy(GtkWidget *window, gpointer data)
{
	struct app *app = data;

	(void)window;
	save_now(app);
	app->notes.window = NULL;
	app->notes.buffer = NULL;
}

/*
 * Fill the buffer from the notes file.  A missing file is an empty note; a
 * file that is not UTF-8 is shown read as Latin-1 (which any bytes are), and
 * its bytes are kept so that the first save first copies them, untouched, to
 * "notes.orig"; another error is shown as a notice.  The buffer starts saved.
 */
static void load(struct app *app)
{
	gchar *text = NULL;
	gsize len = 0;
	GError *error = NULL;
	char path[4096];

	notes_path(app, path, sizeof(path));
	if (g_file_get_contents(path, &text, &len, &error)) {
		if (g_utf8_validate(text, (gssize)len, NULL)) {
			gtk_text_buffer_set_text(app->notes.buffer, text,
						 (int)len);
			g_free(text);
		} else {
			gchar *shown = g_convert(text, (gssize)len, "UTF-8",
						 "ISO-8859-1", NULL, NULL,
						 NULL);

			gtk_text_buffer_set_text(app->notes.buffer,
						 shown ? shown : "", -1);
			g_free(shown);
			g_free(app->notes.original);
			app->notes.original = text; /* kept for notes.orig */
			app->notes.original_len = len;
			app_notice(app,
				   "The notes file is not UTF-8 text: it is "
				   "shown read as Latin-1, and the first "
				   "save keeps the file as it was in "
				   "notes.orig.");
		}
	} else {
		if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			app_notice(app, error->message);
		g_error_free(error);
	}
	app->notes.dirty = false;
}

/*
 * notebook.layout: "size W H" and "text X0 Y0 X1 Y1", the page's writing
 * area, in pixels of the 1x image.  Other lines are skipped, as is a line
 * whose numbers do not parse.  Returns true when both were found and the
 * size is positive.
 */
static bool load_layout(const char *path, double size[2], double text[4])
{
	FILE *f = fopen(path, "r");
	char line[256];
	bool have_size = false, have_text = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char *tok[5], *save = NULL;
		size_t n = 0;
		double v[4];

		for (char *t = strtok_r(line, " \t\n", &save); t && n < 5;
		     t = strtok_r(NULL, " \t\n", &save))
			tok[n++] = t;
		if (n < 3)
			continue;
		for (size_t i = 1; i < n; i++) {
			char *end;

			errno = 0;
			v[i - 1] = strtod(tok[i], &end);
			if (errno || *end || !isfinite(v[i - 1]))
				n = 0;
		}
		if (n == 3 && strcmp(tok[0], "size") == 0) {
			size[0] = v[0];
			size[1] = v[1];
			have_size = true;
		} else if (n == 5 && strcmp(tok[0], "text") == 0) {
			memcpy(text, v, 4 * sizeof(double));
			have_text = true;
		}
	}
	fclose(f);
	return have_size && have_text && size[0] > 0 && size[1] > 0;
}

/* The page area's draw function: the page image at the origin. */
static void paint_page(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		       gpointer data)
{
	(void)area;
	(void)width;
	(void)height;
	cairo_set_source_surface(cr, data, 0, 0);
	cairo_paint(cr);
}

/*
 * The page, drawn at its 1x size.  (A GtkPicture asks for its image's own
 * size, twice the page's for the 2x image: the window opened at twice the
 * page, the rest of it empty.)  The area owns the surface.  NULL when the
 * image cannot be loaded.
 */
static GtkWidget *page_image(const char *path, double scale, double w, double h)
{
	cairo_surface_t *s = app_load_image(path, scale);
	GtkWidget *area;

	if (!s)
		return NULL;
	area = gtk_drawing_area_new();
	gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(area),
					   (int)lround(w));
	gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(area),
					    (int)lround(h));
	gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), paint_page, s,
				       (GDestroyNotify)cairo_surface_destroy);
	return area;
}

/*
 * The editor on the rendered page, or NULL without the images: the layout
 * from the 1x set, the page from the 2x set when present (else 1x), and
 * the editor sized and placed on the page's writing area.
 */
static GtkWidget *page_new(GtkWidget *editor, const char *dir)
{
	char path[4096];
	double size[2], text[4], scale = 2.0;
	GtkWidget *fixed, *picture;

	snprintf(path, sizeof(path), "%s/1x/notebook.layout", dir);
	if (!load_layout(path, size, text))
		return NULL;
	snprintf(path, sizeof(path), "%s/2x/notebook.jpg", dir);
	if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
		snprintf(path, sizeof(path), "%s/1x/notebook.jpg", dir);
		scale = 1.0;
	}
	picture = page_image(path, scale, size[0], size[1]);
	if (!picture)
		return NULL;
	fixed = gtk_fixed_new();
	gtk_fixed_put(GTK_FIXED(fixed), picture, 0, 0);
	gtk_widget_set_size_request(editor, (int)lround(text[2] - text[0]),
				    (int)lround(text[3] - text[1]));
	gtk_widget_add_css_class(editor, "notes-on-page");
	gtk_fixed_put(GTK_FIXED(fixed), editor, text[0], text[1]);
	return fixed;
}

/* Show the NOTES window, building it (and loading the note) if closed. */
void notes_present(struct app *app)
{
	struct notes_widgets *n = &app->notes;
	GtkWidget *window, *scroll, *view, *content = NULL;

	if (n->window) {
		gtk_window_present(GTK_WINDOW(n->window));
		return;
	}
	window = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(window), "Notes from device");
	if (app->window)
		gtk_window_set_transient_for(GTK_WINDOW(window), app->window);
	view = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
	gtk_widget_add_css_class(view, "notes");
	gtk_accessible_update_property(GTK_ACCESSIBLE(view),
				       GTK_ACCESSIBLE_PROPERTY_LABEL,
				       "Notes from device", -1);
	n->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
	/* loaded before "changed" is connected: loading is not an edit */
	load(app);
	g_signal_connect(n->buffer, "changed", G_CALLBACK(on_changed), app);

	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
	if (app->desk_dir)
		content = page_new(scroll, app->desk_dir);
	if (content) {
		gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
	} else {
		content = scroll;
		gtk_window_set_default_size(GTK_WINDOW(window), 520, 640);
	}
	gtk_window_set_child(GTK_WINDOW(window), content);
	g_signal_connect(window, "close-request", G_CALLBACK(on_close_request),
			 app);
	g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), app);
	n->window = window;
	gtk_window_present(GTK_WINDOW(window));
	gtk_widget_grab_focus(view);
}

/* Write the note now if it is unsaved; forget a kept original. */
void notes_shutdown(struct app *app)
{
	save_now(app);
	g_clear_pointer(&app->notes.original, g_free);
}
