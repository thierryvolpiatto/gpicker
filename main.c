/*
 * Copyright (C) 2008,2009 Aliaksey Kandratsenka
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * `http://www.gnu.org/licenses/'.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <sys/types.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <libgen.h>
#include <sys/wait.h>

#if defined(__APPLE__) && defined(__MACH__)

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>
#include <gdk/gdkquartz.h>

#else

#ifdef WITH_GDK_X11
#include <gdk/gdkx.h>
#endif

#endif

#include "xmalloc.h"
#include "filtration.h"
#include "vector.h"
#include "timing.h"
#include "inline_qsort.h"
#include "loading.h"
#include "do_with_main_loop.h"
#include "scorer.h"

static GtkWindow *top_window;
static GtkEntry *name_entry;
static GtkTreeView *tree_view;
static GtkListStore *list_store;

static char *init_filter;
static gboolean show_version;
static gboolean multiselect;
static gboolean align_left;
static gboolean print_pattern;
static char *project_type;
static char *project_dir;
static char *pattern_text;
static gboolean read_stdin;
static gboolean output_index;
static gboolean disable_bzr;
static gboolean disable_hg;

static
gboolean program_exited;
static
gboolean gpicker_loading_completed;
static
struct do_with_main_loop_process *loading_process;

GPid child_pid;

static
void kill_child_pid(void)
{
	if (child_pid)
		kill(child_pid, SIGINT);
}

static void filter_tree_view_tail();

static
void filter_tree_view(char *pattern)
{
	filter_files(pattern, filter_tree_view_tail);
}

static
char *applied_pattern;

static
void filter_tree_view_tail(char *pattern)
{
	GtkTreeIter iter;
	timing_t start;
	int i, n;
	struct filter_result *results = (struct filter_result *)filtered.buffer;

	if (applied_pattern)
		free(applied_pattern);
	applied_pattern = xstrdup(pattern);

	start = start_timing();

	g_object_ref(G_OBJECT(list_store));
	gtk_tree_view_set_model(tree_view, 0);

	gtk_list_store_clear(list_store);

	finish_timing(start, "gtk_list_store_clear");
	start = start_timing();

	n = filtered.used;
	if (n > FILTER_LIMIT)
		n = FILTER_LIMIT;
	for (i=0; i<n; i++) {
		gtk_list_store_append(list_store, &iter);
		gtk_list_store_set(list_store, &iter,
				   0, results[i].index,
				   -1);
	}

	finish_timing(start, "adding filtered data");
	start = start_timing();

	gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(list_store));
	g_object_unref(G_OBJECT(list_store));

	finish_timing(start, "setting model back");

	{
		GtkTreeSelection *sel = gtk_tree_view_get_selection(tree_view);
		GtkTreePath *path = gtk_tree_path_new_from_indices(0,-1);
		gtk_tree_selection_select_path(sel, path);
		gtk_tree_path_free(path);
	}
}

static
void cell_data_func(GtkTreeViewColumn *col,
		    GtkCellRenderer *_renderer,
		    GtkTreeModel *model,
		    GtkTreeIter *iter,
		    gpointer dummy)
{
	GtkCellRendererText *renderer = GTK_CELL_RENDERER_TEXT(_renderer);
	int index;
	char *text=0;
	gtk_tree_model_get(model, iter, 0, &index, -1);
	text = files[index].p;
	if (text) {
		const char *pattern = applied_pattern;
		int patlen = strlen(pattern);
		unsigned match[patlen ? patlen : 1];
		int passes;
		int i;

		g_object_set(G_OBJECT(renderer),
			     "text", text,
			     NULL);

		memset(match, -1, patlen*sizeof(match[0]));
		passes = obtain_match(pattern, index, match);
		if (!passes)
			return;

		PangoAttrList *list = pango_attr_list_new();
		for (i=0;i<patlen;i++) {
			unsigned match_pos = match[i];
			if (match_pos == SCORER_MATCH_NONE)
				continue;
			PangoAttribute *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
			attr->start_index = match_pos;
			while (utf8_continuation_p(text[++match_pos]));
			attr->end_index = match_pos;
			pango_attr_list_insert(list, attr);
		}

		g_object_set(G_OBJECT(renderer),
			     "attributes", list,
			     NULL);
		pango_attr_list_unref(list);
	}
}

static
void setup_column(void)
{
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	GtkTreeViewColumn *col;
	g_object_set(G_OBJECT(renderer),
		     "ellipsize", align_left ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_START,
		     "alignment", align_left ? PANGO_ALIGN_LEFT : PANGO_ALIGN_RIGHT,
		     NULL);

	col = gtk_tree_view_column_new_with_attributes("filename", renderer, NULL);

	gtk_tree_view_column_set_cell_data_func(col, renderer, cell_data_func, 0, 0);
	gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);

	gtk_tree_view_append_column(tree_view, col);
}

static
void exit_program(void)
{
	program_exited = TRUE;
	if (!gpicker_loading_completed) {
		read_filenames_abort();
		do_with_main_loop_quit(loading_process);
	} else
		gtk_main_quit();
}

static
void selection_printer(gpointer data,
		       gpointer _dummy)
{
	static int did_output = 0;
	GtkTreePath *path = data;
	GtkTreeIter iter;
	gint idx;

	if (did_output)
		putchar(name_separator[0]);
	did_output = 1;

	gtk_tree_model_get_iter(GTK_TREE_MODEL(list_store), &iter, path);
	gtk_tree_model_get(GTK_TREE_MODEL(list_store), &iter, 0, &idx, -1);

	if (output_index) {
		printf("%d", idx);
	} else {
		fputs(files[idx].p, stdout);
	}

	gtk_tree_path_free(path);
}

static
void print_selection(void)
{
	GtkTreeSelection *sel = gtk_tree_view_get_selection(tree_view);
	GList *list;

	list = gtk_tree_selection_get_selected_rows(sel, 0);
	if (!list) {
		if (print_pattern)
			fputs(pattern_text, stdout);
		return;
	}

	g_list_foreach(list, selection_printer, NULL);
	g_list_free(list);
}

static
void choice_made(void)
{
	print_selection();
	exit_program();
}

static
gboolean on_top_window_keypress(GtkWidget *_dummy,
				GdkEventKey *event,
				gpointer _dummy2)
{
	if (event->keyval == GDK_Escape) {
		exit_program();
		return TRUE;
	}
	return FALSE;
}

static
void on_entry_changed(GtkEditable *editable,
		      gpointer data)
{
	if (!gpicker_loading_completed)
		return;

	timing_t start = start_timing();

	free(pattern_text);
	pattern_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(editable)));
	filter_tree_view(pattern_text);

	finish_timing(start, "filtration");
}

static
void set_window_title(void)
{
	char work_dir[PATH_MAX];
	gchar *title;

	if (read_stdin) {
		gtk_window_set_title(top_window, "pick something");
		return;
	}

	if (!getcwd(work_dir, sizeof(work_dir)) || !work_dir[0])
		return;

	title = g_strdup_printf("%s - pick a file", work_dir);
	gtk_window_set_title(top_window, title);
	free(title);
}

static
gboolean async_load_callback(void *_dummy)
{
	if (gpicker_loading_completed)
		return FALSE;

	char *title = g_strdup_printf("Loading filelist (%d bytes) - gpicker", gpicker_bytes_readen);
	gtk_window_set_title(top_window, title);
	free(title);

	return TRUE;
}

static
int my_popen(char *string, GPid *child_pid)
{
	char *argv[] = {"/bin/sh", "-c", string, 0};
	int fd;
	gboolean ok = g_spawn_async_with_pipes(0, // work dir
					       argv,
					       0, //envp
					       G_SPAWN_DO_NOT_REAP_CHILD, // flags
					       0, 0, //child setup & user data
					       child_pid,
					       0, // stdin
					       &fd, //stdout
					       0, //stderr
					       0);
	if (!ok)
		fd = -1;
	return fd;
}

static
gpointer setup_filenames_core(gpointer _dummy,
			      struct do_with_main_loop_process *p)
{
	int pipe = -1;

	do_with_main_loop_init_complete(p);

	if (project_type && !strcmp(project_type, "mlocate")) {
		int fd = open(project_dir, O_RDONLY);
		if (fd < 0) {
			perror("setup_filenames:open");
			exit(1);
		}
		read_filenames_from_mlocate_db(fd);
		close(fd);
		return 0;
	}

	if (read_stdin)
		pipe = fileno(stdin);
	else if (!project_type || !strcmp(project_type, "default")) {
		char *find_command = getenv("GPICKER_FIND");
		if (!find_command)
			find_command = FIND_INVOCATION;
		pipe = my_popen(find_command, &child_pid);
	} else if (!strcmp(project_type, "script")) {
		pipe = my_popen("./.gpicker-script", &child_pid);
	} else if (!strcmp(project_type, "git"))
		pipe = my_popen("git ls-files --exclude-standard -c -o -z .", &child_pid);
	else if (!strcmp(project_type, "hg"))
		pipe = my_popen("hg locate -0 --include .", &child_pid);
	else if (!strcmp(project_type, "bzr"))
		pipe = my_popen("bzr ls -R --versioned --unknown --null", &child_pid);


	if (pipe < 0) {
		perror("failed to spawn find");
		exit(1);
	}

	read_filenames(pipe);

	close(pipe);

	if (child_pid) {
		pid_t rv;
		int child_status;
		do {
			rv = waitpid(child_pid, &child_status, 0);
			if (rv < 0) {
				if (errno == EINTR)
					continue;
				perror("waitpid");
				exit(1);
			}
		} while (0);
		child_pid = 0;
		if (WIFEXITED(child_status) && (child_status = WEXITSTATUS(child_status)) != 0) {
			fprintf(stderr, "child exited with bad status: %d\n", child_status);
			exit(1);
		}
	}

	return 0;
}

static
void setup_filenames(void)
{
	g_timeout_add(250, async_load_callback, 0);
	gtk_widget_set_sensitive(GTK_WIDGET(tree_view), FALSE);

	do_with_main_loop((do_with_main_loop_fn)setup_filenames_core, 0, &loading_process);
	gpicker_loading_completed = TRUE;
	do_with_main_loop_close(loading_process);

	gtk_widget_set_sensitive(GTK_WIDGET(tree_view), TRUE);
	set_window_title();
}

static
void setup_data(void)
{
	list_store = gtk_list_store_new(1, G_TYPE_INT);
	gtk_tree_view_set_model(tree_view, GTK_TREE_MODEL(list_store));

	GtkEditable *editable = GTK_EDITABLE(name_entry);

	if (init_filter) {
		int len = strlen(init_filter);
		gtk_entry_set_text(name_entry, init_filter);
		gtk_editable_set_position(editable, len);
		gtk_editable_select_region(editable, 0, len);
	}

	setup_filenames();
	setup_column();

	on_entry_changed(editable, 0);
}

static
gboolean key_events_delegator(GtkWidget *w, GdkEventKey *e, gpointer dummy)
{
	gint sel_start, sel_end;
	gboolean had_selection;
	gint old_cursor_pos;
	gboolean rv;

	had_selection = gtk_editable_get_selection_bounds(GTK_EDITABLE(name_entry),
							  &sel_start, &sel_end);
	old_cursor_pos = gtk_editable_get_position(GTK_EDITABLE(name_entry));

	g_object_set(G_OBJECT(tree_view), "can-focus", TRUE, NULL);
	gtk_widget_grab_focus(GTK_WIDGET(tree_view));
	rv = gtk_widget_event(GTK_WIDGET(tree_view), (GdkEvent *)e);
	gtk_widget_grab_focus(GTK_WIDGET(name_entry));
	g_object_set(G_OBJECT(tree_view), "can-focus", FALSE, NULL);

	gtk_editable_set_position(GTK_EDITABLE(name_entry), old_cursor_pos);
	if (had_selection)
		gtk_editable_select_region(GTK_EDITABLE(name_entry), sel_start, sel_end);
	else
		gtk_editable_select_region(GTK_EDITABLE(name_entry), old_cursor_pos, old_cursor_pos);

	return rv;
}

static
gboolean on_move_cursor(GtkTreeView *view, GtkMovementStep step, gint arg2, gpointer user_data)
{
	GtkTreeSelection *sel;
	GtkTreePath *path;
	sel = gtk_tree_view_get_selection(view);
	gtk_tree_view_get_cursor(view, &path, NULL);
	gtk_tree_selection_select_path(sel, path);
	gtk_tree_path_free(path);
	return TRUE;
}

static
void setup_signals(void)
{
	g_signal_connect(top_window, "destroy", G_CALLBACK(exit_program), 0);
	g_signal_connect(top_window, "key-press-event", G_CALLBACK(on_top_window_keypress), 0);
	g_signal_connect(name_entry, "activate", G_CALLBACK(choice_made), 0);
	g_signal_connect(tree_view, "row-activated", G_CALLBACK(choice_made), 0);
	g_signal_connect(name_entry, "changed", G_CALLBACK(on_entry_changed), 0);

	g_signal_connect_after(tree_view, "move-cursor", G_CALLBACK(on_move_cursor), 0);

	g_object_set(G_OBJECT(tree_view), "can-focus", FALSE, NULL);
	g_signal_connect_after(name_entry, "key-press-event", G_CALLBACK(key_events_delegator), 0);
	g_signal_connect_after(name_entry, "key-release-event", G_CALLBACK(key_events_delegator), 0);
}

static
GOptionEntry entries[] = {
	{"version", 0, 0, G_OPTION_ARG_NONE, &show_version, "show version", 0},
	{"project-type", 't', 0, G_OPTION_ARG_STRING, &project_type, "respect ignored files for given kind of VCS (default, git, bzr, hg, guess, mlocate)", 0},
	{"disable-bzr", 0, 0, G_OPTION_ARG_NONE, &disable_bzr, "disable autodetection of Bazaar project type", 0},
	{"disable-hg", 0, 0, G_OPTION_ARG_NONE, &disable_hg, "disable autodetection of Mercurial project type", 0},
	{"name-separator", 'n', 0, G_OPTION_ARG_STRING, &name_separator, "separator of filenames from stdin (\\0 is default)", 0},
	{"dir-separator", 'd', 0, G_OPTION_ARG_STRING, &dir_separator, "separator of directory names from stdin (/ is default)", 0},
	{"eat-prefix", 0, 0, G_OPTION_ARG_STRING, &eat_prefix, "eat this prefix from names (./ is default)", 0},
	{"multiselect", 'm', 0, G_OPTION_ARG_NONE, &multiselect, "enable multiselect", 0},
	{"left-align", 'l', 0, G_OPTION_ARG_NONE, &align_left, "left align everything (default is right-align)", 0},
	{"print-pattern", 'p', 0, G_OPTION_ARG_NONE, &print_pattern, "print pattern if nothing matched (default is false)", 0},
	{"dont-sort", 'S', 0, G_OPTION_ARG_NONE, &dont_sort_initial, "dont sort result list", 0},
	{"init-filter", 'i', 0, G_OPTION_ARG_STRING, &init_filter, "initial filter value", 0},
	{"load-stdin-too", 0, 0, G_OPTION_ARG_NONE, &gpicker_load_stdin_too, "read additional filenames from stdin", 0},
	{"output-index", 'I', 0, G_OPTION_ARG_NONE, &output_index, "print selection index instead of value (implies -S)", 0},
	{"ignore-positions", 'P', 0, G_OPTION_ARG_NONE, &ignore_positions, "ignore match position for sorting", 0},
	{0}
};

static
int isdir(char* name)
{
	struct stat statbuf;

	if (stat(name, &statbuf) < 0 || !S_ISDIR(statbuf.st_mode)) {
		return 0;
	}
	return 1;
}

static
int check_parents(char* name)
{
	struct stat rootdir;
	struct stat curdir;
	int isroot, rv;
	int cwd = dirfd(opendir("."));

	stat("/", &rootdir);
	while (1) {
		if (isdir(name)) {
			rv = 1;
			break;
		}
		stat(".", &curdir);
		isroot = (rootdir.st_dev == curdir.st_dev &&
				rootdir.st_ino == curdir.st_ino);
		if (isroot || chdir("..") == -1) {
			rv = 0;
			break;
		}
	}
	if (fchdir(cwd) < 0) {
		perror("cannot chdir back");
		exit(1);
	}
	return rv;
}

static
int detect_project_type_script(void)
{
	struct stat st;
	int rv = stat(".gpicker-script", &st);
	/* NOTE: I was considering checking if script is executable
	 * but decided that guess semantics shouldn't bother with
	 * it. It provides better error reporting. */
	return (rv == 0 && S_ISREG(st.st_mode));
}

static
void enter_project_dir()
{
	int rv = chdir(project_dir);

	if (rv) {
		perror("cannot chdir to project directory");
		exit(1);
	}

	if (project_type && !strcmp(project_type, "guess")) {
		if (detect_project_type_script())
			project_type = "script";
		else if (check_parents(".git"))
			project_type = "git";
		else if (!disable_hg && check_parents(".hg"))
			project_type = "hg";
		else if (!disable_bzr && check_parents(".bzr"))
			project_type = "bzr";
		else
			project_type = "default";
	}
}

static
void process_separator(char **separator_place, char *name, char *def)
{
	char *separator = *separator_place;
	if (separator) {
		if (!read_stdin)
			fprintf(stderr, "Warning: --%s with usual project is useless\n", name);

		if (!strcmp(separator, "\\n"))
			separator = "\n";
		if (!strcmp(separator, "\\r"))
			separator = "\r";
		else if (!strcmp(separator, "\\0"))
			separator = "";
		else if (!strcmp(separator, "\\t"))
			separator = "\t";

		if (strlen(separator) > 1) {
			fprintf(stderr, "%s of more than one character is not supported\n", name);
			exit(1);
		}
	} else
		separator = def;

	*separator_place = separator;
}

static
void parse_options(int argc, char **argv)
{
	GError *error = 0;
	GOptionContext *context;
	context = g_option_context_new("PROJECT-DIR-PATH - quickly pick a file from the project");
	g_option_context_add_main_entries(context, entries, 0);
	g_option_context_add_group(context, gtk_get_option_group(TRUE));

	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		fprintf(stderr, "option parsing failed: %s\n", error->message);
		exit(1);
	}

	if (output_index)
		dont_sort_initial = 1;

	if (show_version) {
		fprintf(stdout, "%s %s\n", PACKAGE_NAME, VERSION);
		exit(0);
	}
	if (argc < 2) {
		fprintf(stderr, "I need a project path\n");
		fputs(g_option_context_get_help(context, TRUE, NULL), stderr);
		exit(1);
	}
	if (project_type && strcmp(project_type, "guess") &&
	    strcmp(project_type, "git") &&
	    strcmp(project_type, "hg") &&
	    strcmp(project_type, "bzr") &&
	    strcmp(project_type, "default") &&
	    strcmp(project_type, "script") &&
	    strcmp(project_type, "mlocate")) {
		fprintf(stderr, "Unknown project type specified: %s\n", project_type);
		exit(1);
	}

	project_dir = argv[1];
	read_stdin = !strcmp(project_dir, "-");

	process_separator(&name_separator, "name-separator", "");
	process_separator(&dir_separator, "dir-separator", "/");
	filter_dir_separator = dir_separator[0];

	if (read_stdin) {
		if (project_type)
			fprintf(stderr, "Warning: project type with stdin input has no effect\n");
	} else if (!project_type || strcmp(project_type, "mlocate")) {
		enter_project_dir();
	}
}

static
void get_monitor_dimensions(unsigned *width, unsigned *height)
{
	GdkScreen *screen = gdk_display_get_default_screen(gdk_display_get_default());
	int i = gdk_screen_get_n_monitors(screen);
	unsigned w = INT_MAX, h = INT_MAX;
	while (--i >= 0) {
		GdkRectangle r;
		gdk_screen_get_monitor_geometry(screen, i, &r);
		w = (w > r.width) ? r.width : w;
		h = (h > r.height) ? r.height : h;
	}
	*width = w;
	*height = h;
}

static
void build_ui()
{
	GtkBox *vbox;
	GtkScrolledWindow *scrolled_window;

	top_window = GTK_WINDOW(gtk_window_new(GTK_WINDOW_TOPLEVEL));
	gtk_window_set_type_hint(top_window, GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_window_set_title(top_window, "Loading filelist - gpicker");

	vbox = GTK_BOX(gtk_vbox_new(FALSE, 3));
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 3);
	gtk_container_add(GTK_CONTAINER(top_window), GTK_WIDGET(vbox));

	name_entry = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_alignment(name_entry, align_left ? 0.0 : 1.0);
	gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(name_entry),
			   FALSE, TRUE, 0);

	scrolled_window = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(0, 0));
	gtk_scrolled_window_set_policy(scrolled_window,
				       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(scrolled_window),
			   TRUE, TRUE, 0);

	tree_view = GTK_TREE_VIEW(gtk_tree_view_new());
	gtk_tree_view_set_headers_visible(tree_view, FALSE);

	gtk_tree_view_set_enable_search(tree_view, FALSE);
	gtk_tree_view_set_fixed_height_mode(tree_view, TRUE);
	gtk_tree_view_set_show_expanders(tree_view, FALSE);
	gtk_container_add(GTK_CONTAINER(scrolled_window), GTK_WIDGET(tree_view));

	unsigned display_width, display_height;
	get_monitor_dimensions(&display_width, &display_height);
	gtk_window_set_default_size(top_window, display_width/2, display_height/2);

	gtk_window_set_position(top_window, GTK_WIN_POS_CENTER);
}

extern
int simple_main(int, char **);

int main(int argc, char **argv)
{
	init_loading();
	prepare_scorer();

	char *gpicker = basename(xstrdup(argv[0]));
	if (!strcmp(gpicker, "gpicker-simple"))
		return simple_main(argc, argv);

	timing_t tstart = start_timing();

	parse_options(argc, argv); /* inits GTK+ too */

	atexit(kill_child_pid);

	finish_timing(tstart, "gtk initialization");
	tstart = start_timing();

	build_ui();

	finish_timing(tstart, "UI initialization");
	tstart = start_timing();

	gtk_widget_show_all(GTK_WIDGET(top_window));

#ifdef WITH_GDK_X11
	// force our popup to be recent enough to display on top
	GdkWindow *gdk_win = GTK_WIDGET(top_window)->window;
	gdk_x11_window_set_user_time(gdk_win, gdk_x11_get_server_time(gdk_win));
#endif
	gdk_window_set_cursor(GTK_WIDGET(top_window)->window, gdk_cursor_new(GDK_WATCH));

#if !defined(__APPLE__)
	// OSX may benefit from this too, but I cannot test it yet.
	while (gtk_events_pending())
		gtk_main_iteration();
#endif

#if defined(__APPLE__) && defined(__MACH__)
	NSWindow *nswin = gdk_quartz_window_get_nswindow(GTK_WIDGET(top_window)->window);
	[nswin center];

	[NSApp activateIgnoringOtherApps:YES];
#endif

	finish_timing(tstart, "initial show");
	tstart = start_timing();

	setup_signals();

	finish_timing(tstart, "setup_signals");
	tstart = start_timing();

	setup_data();

	if (multiselect) {
		gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree_view),
					    GTK_SELECTION_MULTIPLE);
	}

	gdk_window_set_cursor(GTK_WIDGET(top_window)->window, 0);

	if (!gtk_widget_is_focus(GTK_WIDGET(name_entry)))
		gtk_widget_grab_focus(GTK_WIDGET(name_entry));

	finish_timing(tstart, "setup_data");

	if (!program_exited)
		gtk_main();

	uninit_filter();

	return 0;
}
