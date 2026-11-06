/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * patchbay.c - the PATCH BAY window: what each pair records and plays
 *
 * The window is hidden, not destroyed, when closed, so its widgets live as
 * long as the application and app_refresh() can always reach them.
 */
#include "app.h"

/* The sources in menu order; DM ALL OFF adds "same" in front. */
static const enum ua1000_source sources[] = {
	UA1000_SRC_MONITOR_OUT,	  UA1000_SRC_INPUT_1_2,
	UA1000_SRC_INPUT_3_4,	  UA1000_SRC_INPUT_5_6,
	UA1000_SRC_INPUT_7_8,	  UA1000_SRC_INPUT_9_10,
	UA1000_SRC_WAVE_OUT_1_2,  UA1000_SRC_WAVE_OUT_3_4,
	UA1000_SRC_WAVE_OUT_5_6,  UA1000_SRC_WAVE_OUT_7_8,
	UA1000_SRC_WAVE_OUT_9_10,
};

static const char *const source_labels[] = {
	"MONITOR OUT",	"INPUT 1-2",	"INPUT 3-4",	 "INPUT 5-6",
	"INPUT 7-8",	"INPUT 9-10",	"WAVE OUT 1-2",	 "WAVE OUT 3-4",
	"WAVE OUT 5-6", "WAVE OUT 7-8", "WAVE OUT 9-10",
};

#define N_SOURCES G_N_ELEMENTS(sources)

struct route_ref {
	struct app *app;
	enum ua1000_route_table table;
	unsigned int pair;
};

static bool has_same(enum ua1000_route_table table)
{
	return table == UA1000_ROUTE_OUTPUT_DM_OFF;
}

static guint source_index(enum ua1000_route_table table, unsigned int source)
{
	guint offset = has_same(table) ? 1 : 0;

	if (has_same(table) && source == UA1000_SRC_SAME_AS_DM_ON)
		return 0;
	for (guint i = 0; i < N_SOURCES; i++)
		if (sources[i] == source)
			return i + offset;
	return 0;
}

static enum ua1000_source source_at(enum ua1000_route_table table, guint index)
{
	if (has_same(table)) {
		if (index == 0)
			return UA1000_SRC_SAME_AS_DM_ON;
		index--;
	}
	return index < N_SOURCES ? sources[index] : UA1000_SRC_MONITOR_OUT;
}

static void on_route(GtkDropDown *dropdown, GParamSpec *pspec, gpointer data)
{
	struct route_ref *ref = data;
	struct app *app = ref->app;
	struct ua1000_edit edit;
	int ret;

	(void)pspec;
	if (app->refreshing)
		return;
	ret = ua1000_edit_route(
		&app->state, &edit, ref->table, ref->pair,
		source_at(ref->table, gtk_drop_down_get_selected(dropdown)));
	app_apply(app, ret, &edit, false);
}

static GtkWidget *route_new(struct app *app, enum ua1000_route_table table,
			    unsigned int pair)
{
	const char *labels[N_SOURCES + 2];
	guint n = 0;

	if (has_same(table))
		labels[n++] = "(same as DM ON)";
	for (guint i = 0; i < N_SOURCES; i++)
		labels[n++] = source_labels[i];
	labels[n] = NULL;

	GtkWidget *dropdown = gtk_drop_down_new_from_strings(labels);
	struct route_ref *ref = g_new(struct route_ref, 1);

	ref->app = app;
	ref->table = table;
	ref->pair = pair;
	g_object_set_data_full(G_OBJECT(dropdown), "route-ref", ref, g_free);
	g_signal_connect(dropdown, "notify::selected", G_CALLBACK(on_route),
			 ref);
	app->patchbay.route[table][pair] = dropdown;
	return dropdown;
}

static void preset(struct app *app, enum ua1000_wave_in_preset wave_in,
		   enum ua1000_output_preset output)
{
	struct ua1000_edit edit;
	int ret = ua1000_edit_preset(&app->state, &edit, wave_in, output);

	/* app_apply() refreshes the menus without sending them again. */
	app_apply(app, ret, &edit, false);
}

static void on_wave_in_initial(GtkButton *b, gpointer app)
{
	(void)b;
	preset(app, UA1000_WAVE_IN_INITIAL, UA1000_OUTPUT_KEEP);
}

static void on_wave_in_loop_back(GtkButton *b, gpointer app)
{
	(void)b;
	preset(app, UA1000_WAVE_IN_LOOP_BACK, UA1000_OUTPUT_KEEP);
}

static void on_output_initial(GtkButton *b, gpointer app)
{
	(void)b;
	preset(app, UA1000_WAVE_IN_KEEP, UA1000_OUTPUT_INITIAL);
}

static void on_output_hard_thru(GtkButton *b, gpointer app)
{
	(void)b;
	preset(app, UA1000_WAVE_IN_KEEP, UA1000_OUTPUT_HARD_THRU);
}

static void on_output_monitor_out(GtkButton *b, gpointer app)
{
	(void)b;
	preset(app, UA1000_WAVE_IN_KEEP, UA1000_OUTPUT_MONITOR_OUT);
}

static GtkWidget *button_new(const char *label, GCallback callback,
			     struct app *app)
{
	GtkWidget *button = gtk_button_new_with_label(label);

	g_signal_connect(button, "clicked", callback, app);
	return button;
}

static GtkWidget *column_header(const char *text)
{
	GtkWidget *label = gtk_label_new(text);

	gtk_widget_add_css_class(label, "heading");
	return label;
}

static GtkWidget *pair_label(const char *table, unsigned int pair)
{
	gchar *text =
		g_strdup_printf("%s %u-%u", table, 2 * pair + 1, 2 * pair + 2);
	GtkWidget *label = gtk_label_new(text);

	g_free(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	return label;
}

/* A screen reader names a route by its row's label (and its column's). */
static GtkWidget *labelled(GtkWidget *dropdown, GtkWidget *row,
			   GtkWidget *column)
{
	/* the list ends at its first NULL: a missing column must not end it
	 * early and leave NULL, -1 read as another relation */
	if (column)
		gtk_accessible_update_relation(
			GTK_ACCESSIBLE(dropdown),
			GTK_ACCESSIBLE_RELATION_LABELLED_BY, row, column, NULL,
			-1);
	else
		gtk_accessible_update_relation(
			GTK_ACCESSIBLE(dropdown),
			GTK_ACCESSIBLE_RELATION_LABELLED_BY, row, NULL, -1);
	return dropdown;
}

static GtkWidget *wave_in_frame(struct app *app)
{
	GtkWidget *frame = gtk_frame_new("WAVE IN — what the computer records");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *presets = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *grid = gtk_grid_new();

	gtk_box_append(GTK_BOX(presets),
		       button_new("Initial Setting",
				  G_CALLBACK(on_wave_in_initial), app));
	gtk_box_append(GTK_BOX(presets),
		       button_new("Loop Back", G_CALLBACK(on_wave_in_loop_back),
				  app));
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
		GtkWidget *label = pair_label("WAVE IN", pair);

		gtk_grid_attach(GTK_GRID(grid), label, 0, (int)pair, 1, 1);
		gtk_grid_attach(
			GTK_GRID(grid),
			labelled(route_new(app, UA1000_ROUTE_WAVE_IN, pair),
				 label, NULL),
			1, (int)pair, 1, 1);
	}
	gtk_box_append(GTK_BOX(box), presets);
	gtk_box_append(GTK_BOX(box), grid);
	gtk_widget_set_margin_start(box, 8);
	gtk_widget_set_margin_end(box, 8);
	gtk_widget_set_margin_bottom(box, 8);
	gtk_frame_set_child(GTK_FRAME(frame), box);
	return frame;
}

static GtkWidget *output_frame(struct app *app)
{
	GtkWidget *frame = gtk_frame_new("OUTPUT — what each output plays");
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget *presets = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *grid = gtk_grid_new();
	GtkWidget *on = column_header("Direct monitor on");
	GtkWidget *off = column_header("Direct monitor all off");

	gtk_box_append(GTK_BOX(presets),
		       button_new("Initial Setting",
				  G_CALLBACK(on_output_initial), app));
	gtk_box_append(GTK_BOX(presets),
		       button_new("Hard Thru", G_CALLBACK(on_output_hard_thru),
				  app));
	gtk_box_append(GTK_BOX(presets),
		       button_new("Monitor Out",
				  G_CALLBACK(on_output_monitor_out), app));
	gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_grid_attach(GTK_GRID(grid), on, 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), off, 2, 0, 1, 1);
	for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++) {
		int row = (int)pair + 1;
		GtkWidget *label = pair_label("OUTPUT", pair);

		gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
		gtk_grid_attach(GTK_GRID(grid),
				labelled(route_new(app,
						   UA1000_ROUTE_OUTPUT_DM_ON,
						   pair),
					 label, on),
				1, row, 1, 1);
		gtk_grid_attach(GTK_GRID(grid),
				labelled(route_new(app,
						   UA1000_ROUTE_OUTPUT_DM_OFF,
						   pair),
					 label, off),
				2, row, 1, 1);
	}
	gtk_box_append(GTK_BOX(box), presets);
	gtk_box_append(GTK_BOX(box), grid);
	gtk_widget_set_margin_start(box, 8);
	gtk_widget_set_margin_end(box, 8);
	gtk_widget_set_margin_bottom(box, 8);
	gtk_frame_set_child(GTK_FRAME(frame), box);
	return frame;
}

void patchbay_present(struct app *app)
{
	struct patchbay_widgets *pb = &app->patchbay;

	if (!pb->window) {
		GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

		pb->window = gtk_window_new();
		gtk_window_set_title(GTK_WINDOW(pb->window),
				     "UA-1000 Patch Bay");
		gtk_window_set_transient_for(GTK_WINDOW(pb->window),
					     app->window);
		gtk_window_set_hide_on_close(GTK_WINDOW(pb->window), TRUE);
		gtk_box_append(GTK_BOX(row), wave_in_frame(app));
		gtk_box_append(GTK_BOX(row), output_frame(app));
		gtk_widget_set_margin_start(row, 12);
		gtk_widget_set_margin_end(row, 12);
		gtk_widget_set_margin_top(row, 12);
		gtk_widget_set_margin_bottom(row, 12);
		gtk_window_set_child(GTK_WINDOW(pb->window), row);
		bool was = app->refreshing;

		app->refreshing = true;
		patchbay_refresh(app);
		app->refreshing = was;
	}
	gtk_window_present(GTK_WINDOW(pb->window));
}

void patchbay_refresh(struct app *app)
{
	if (!app->patchbay.window)
		return;
	for (int t = UA1000_ROUTE_WAVE_IN; t <= UA1000_ROUTE_OUTPUT_DM_OFF; t++)
		for (unsigned int pair = 0; pair < UA1000_PAIRS; pair++)
			gtk_drop_down_set_selected(
				GTK_DROP_DOWN(app->patchbay.route[t][pair]),
				source_index(
					(enum ua1000_route_table)t,
					app->state.patchbay.route[t][pair]));
}
