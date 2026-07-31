#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <math.h>
#include <libgen.h>
#include <ctype.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *menubar;
    GtkWidget *overlay;
    GtkWidget *da;
    GtkWidget *open_btn;
    GdkPixbuf *pixbuf;

    char **files;
    int num_files;
    int current_idx;
    char *current_dir;

    double scale;
    double dx, dy;
    gboolean fit;
    int angle;

    gboolean dragging;
    double start_x, start_y;
    double start_dx, start_dy;

    gboolean fast_mode;
    guint timer_id;
    gboolean fullscreen;
} AppState;

void load_image(AppState *app, const char *filepath, gboolean reload_dir);
void trigger_fast_render(AppState *app);
void center_image(AppState *app);
void update_title(AppState *app);
void show_open_dialog(GtkWidget *widget, AppState *app);
void toggle_fullscreen(AppState *app);

void free_file_list(AppState *app) {
    if (app->files) {
        for (int i = 0; i < app->num_files; i++) free(app->files[i]);
        free(app->files);
        app->files = NULL;
    }
    app->num_files = 0;
    if (app->current_dir) {
        free(app->current_dir);
        app->current_dir = NULL;
    }
}

int img_filter(const struct dirent *dir) {
    if (dir->d_name[0] == '.') return 0;
    const char *ext = strrchr(dir->d_name, '.');
    if (!ext || ext == dir->d_name) return 0;
    ext++;

    char lw[16] = {0};
    for (int i = 0; i < 15 && ext[i]; i++) lw[i] = tolower((unsigned char)ext[i]);

    if (!strcmp(lw, "jpg") || !strcmp(lw, "jpeg") || !strcmp(lw, "png") ||
        !strcmp(lw, "bmp") || !strcmp(lw, "gif") || !strcmp(lw, "webp") ||
        !strcmp(lw, "tif") || !strcmp(lw, "tiff") || !strcmp(lw, "svg")) {
        return 1;
    }
    return 0;
}

void load_directory_of_file(AppState *app, const char *filepath) {
    free_file_list(app);
    char *path_copy = strdup(filepath);
    char *dname = dirname(path_copy);
    app->current_dir = strdup(dname);
    free(path_copy);

    struct dirent **namelist;
    int n = scandir(app->current_dir, &namelist, img_filter, alphasort);
    if (n < 0) {
        app->num_files = 0;
        return;
    }

    app->num_files = n;
    app->files = malloc(n * sizeof(char *));

    const char *base_filename = strrchr(filepath, '/');
    base_filename = base_filename ? base_filename + 1 : filepath;

    app->current_idx = 0;
    for (int i = 0; i < n; i++) {
        app->files[i] = strdup(namelist[i]->d_name);
        if (strcmp(app->files[i], base_filename) == 0) app->current_idx = i;
        free(namelist[i]);
    }
    free(namelist);
}

void load_image(AppState *app, const char *filepath, gboolean reload_dir) {
    if (app->pixbuf) {
        g_object_unref(app->pixbuf);
        app->pixbuf = NULL;
    }

    GError *error = NULL;
    app->pixbuf = gdk_pixbuf_new_from_file(filepath, &error);
    if (!app->pixbuf) {
        fprintf(stderr, "rezzview: Failed to load image '%s': %s\n", filepath, error->message);
        g_error_free(error);
        return;
    }

    if (reload_dir) load_directory_of_file(app, filepath);

    app->fit = TRUE;
    app->angle = 0;
    app->dx = 0;
    app->dy = 0;
    app->scale = 1.0;

    gtk_widget_hide(app->open_btn);
    update_title(app);
    trigger_fast_render(app);
}

void load_current_index(AppState *app) {
    if (!app->files || app->num_files == 0) return;
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", app->current_dir, app->files[app->current_idx]);
    load_image(app, path, FALSE);
}

void update_title(AppState *app) {
    if (!app->pixbuf) {
        gtk_window_set_title(GTK_WINDOW(app->window), "rezzview");
        return;
    }
    char title[1024];
    const char *base = (app->files && app->num_files > 0) ? app->files[app->current_idx] : "unknown";
    if (app->num_files > 0) {
        snprintf(title, sizeof(title), "rezzview - %s [%d / %d] (%.0f%%)",
                 base, app->current_idx + 1, app->num_files, app->scale * 100.0);
    } else {
        snprintf(title, sizeof(title), "rezzview - %s (%.0f%%)", base, app->scale * 100.0);
    }
    gtk_window_set_title(GTK_WINDOW(app->window), title);
}

gboolean hq_render_cb(gpointer data) {
    AppState *app = (AppState *)data;
    app->fast_mode = FALSE;
    app->timer_id = 0;
    gtk_widget_queue_draw(app->da);
    return G_SOURCE_REMOVE;
}

void trigger_fast_render(AppState *app) {
    app->fast_mode = TRUE;
    if (app->timer_id > 0) g_source_remove(app->timer_id);
    app->timer_id = g_timeout_add(150, hq_render_cb, app);
    gtk_widget_queue_draw(app->da);
}

void center_image(AppState *app) {
    if (!app->pixbuf) return;
    int pw = gdk_pixbuf_get_width(app->pixbuf);
    int ph = gdk_pixbuf_get_height(app->pixbuf);
    int rw = (app->angle % 180 != 0) ? ph : pw;
    int rh = (app->angle % 180 != 0) ? pw : ph;
    int ww = gtk_widget_get_allocated_width(app->da);
    int wh = gtk_widget_get_allocated_height(app->da);
    app->dx = (ww - rw * app->scale) / 2.0;
    app->dy = (wh - rh * app->scale) / 2.0;
}

void zoom_center(AppState *app, double factor) {
    if (!app->pixbuf) return;
    app->fit = FALSE;
    int ww = gtk_widget_get_allocated_width(app->da);
    int wh = gtk_widget_get_allocated_height(app->da);
    double cx = ww / 2.0;
    double cy = wh / 2.0;
    double old_scale = app->scale;
    double new_scale = old_scale * factor;

    if (new_scale < 0.01) new_scale = 0.01;
    if (new_scale > 100.0) new_scale = 100.0;

    app->dx = cx - (cx - app->dx) * (new_scale / old_scale);
    app->dy = cy - (cy - app->dy) * (new_scale / old_scale);
    app->scale = new_scale;
    update_title(app);
    trigger_fast_render(app);
}

gboolean on_draw(GtkWidget *widget, cairo_t *cr, AppState *app) {
    int ww = gtk_widget_get_allocated_width(widget);
    int wh = gtk_widget_get_allocated_height(widget);

    cairo_set_source_rgb(cr, 0.08, 0.08, 0.08);
    cairo_paint(cr);

    if (!app->pixbuf) {
        cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 16.0);
        const char *msg = "Drag & drop an image here or press Ctrl+O to open";
        cairo_text_extents_t extents;
        cairo_text_extents(cr, msg, &extents);
        cairo_move_to(cr, (ww - extents.width) / 2.0, (wh / 2.0) - 40.0);
        cairo_show_text(cr, msg);
        return FALSE;
    }

    int pw = gdk_pixbuf_get_width(app->pixbuf);
    int ph = gdk_pixbuf_get_height(app->pixbuf);
    int rw = (app->angle % 180 != 0) ? ph : pw;
    int rh = (app->angle % 180 != 0) ? pw : ph;

    if (app->fit) {
        double scale_x = (double)ww / rw;
        double scale_y = (double)wh / rh;
        app->scale = scale_x < scale_y ? scale_x : scale_y;
        app->dx = (ww - rw * app->scale) / 2.0;
        app->dy = (wh - rh * app->scale) / 2.0;
    }

    cairo_translate(cr, app->dx + (rw * app->scale) / 2.0, app->dy + (rh * app->scale) / 2.0);
    cairo_rotate(cr, app->angle * G_PI / 180.0);
    cairo_scale(cr, app->scale, app->scale);
    cairo_translate(cr, -pw / 2.0, -ph / 2.0);

    gdk_cairo_set_source_pixbuf(cr, app->pixbuf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), app->fast_mode ? CAIRO_FILTER_FAST : CAIRO_FILTER_GOOD);
    cairo_paint(cr);

    return FALSE;
}

gboolean on_scroll(GtkWidget *widget, GdkEventScroll *event, AppState *app) {
    (void)widget;
    if (!app->pixbuf) return FALSE;

    double delta_y = 0;
    if (event->direction == GDK_SCROLL_UP) delta_y = -1;
    else if (event->direction == GDK_SCROLL_DOWN) delta_y = 1;
    else if (event->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent*)event, NULL, &delta_y);

    if (delta_y == 0) return FALSE;

    app->fit = FALSE;
    double old_scale = app->scale;
    double new_scale = old_scale * (delta_y < 0 ? 1.1 : (1.0 / 1.1));
    if (new_scale < 0.01) new_scale = 0.01;
    if (new_scale > 100.0) new_scale = 100.0;

    app->dx = event->x - (event->x - app->dx) * (new_scale / old_scale);
    app->dy = event->y - (event->y - app->dy) * (new_scale / old_scale);
    app->scale = new_scale;

    update_title(app);
    trigger_fast_render(app);
    return TRUE;
}

gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AppState *app) {
    (void)widget;
    if (event->button == 1 && app->pixbuf) {
        if (event->type == GDK_2BUTTON_PRESS) {
            app->fit = !app->fit;
            if (!app->fit) {
                app->scale = 1.0;
                center_image(app);
            }
            update_title(app);
            trigger_fast_render(app);
            return TRUE;
        }
        app->dragging = TRUE;
        app->start_x = event->x;
        app->start_y = event->y;
        app->start_dx = app->dx;
        app->start_dy = app->dy;
        return TRUE;
    }
    return FALSE;
}

gboolean on_button_release(GtkWidget *widget, GdkEventButton *event, AppState *app) {
    (void)widget;
    if (event->button == 1) {
        app->dragging = FALSE;
        trigger_fast_render(app); 
    }
    return FALSE;
}

gboolean on_motion_notify(GtkWidget *widget, GdkEventMotion *event, AppState *app) {
    (void)widget;
    if (app->dragging && app->pixbuf) {
        app->fit = FALSE;
        app->dx = app->start_dx + (event->x - app->start_x);
        app->dy = app->start_dy + (event->y - app->start_y);
        trigger_fast_render(app);
    }
    return FALSE;
}

gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, AppState *app) {
    (void)widget;
    guint key = event->keyval;

    if (key == GDK_KEY_Escape) {
        if (app->fullscreen) {
            gtk_window_unfullscreen(GTK_WINDOW(app->window));
        } else {
            gtk_main_quit();
        }
        return TRUE;
    }

    if (key == GDK_KEY_Left || key == GDK_KEY_a || key == GDK_KEY_A || key == GDK_KEY_Page_Up) {
        if (app->num_files > 0) {
            app->current_idx = (app->current_idx - 1 + app->num_files) % app->num_files;
            load_current_index(app);
        }
        return TRUE;
    }

    if (key == GDK_KEY_Right || key == GDK_KEY_d || key == GDK_KEY_D || key == GDK_KEY_Page_Down) {
        if (app->num_files > 0) {
            app->current_idx = (app->current_idx + 1) % app->num_files;
            load_current_index(app);
        }
        return TRUE;
    }

    if (key == GDK_KEY_r || key == GDK_KEY_R) {
        app->angle = (app->angle + 90) % 360;
        trigger_fast_render(app);
        return TRUE;
    }

    if (key == GDK_KEY_1) {
        app->fit = FALSE;
        app->scale = 1.0;
        center_image(app);
        update_title(app);
        trigger_fast_render(app);
        return TRUE;
    }

    if (key == GDK_KEY_f || key == GDK_KEY_F) {
        app->fit = TRUE;
        update_title(app);
        trigger_fast_render(app);
        return TRUE;
    }

    if (key == GDK_KEY_plus || key == GDK_KEY_equal || key == GDK_KEY_KP_Add) {
        zoom_center(app, 1.1);
        return TRUE;
    }

    if (key == GDK_KEY_minus || key == GDK_KEY_KP_Subtract) {
        zoom_center(app, 1.0 / 1.1);
        return TRUE;
    }

    return FALSE;
}

gboolean on_window_state(GtkWidget *widget, GdkEventWindowState *event, AppState *app) {
    (void)widget;
    if (event->changed_mask & GDK_WINDOW_STATE_FULLSCREEN) {
        app->fullscreen = (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN);
        if (app->fullscreen) gtk_widget_hide(app->menubar);
        else gtk_widget_show(app->menubar);
    }
    return FALSE;
}

void toggle_fullscreen(AppState *app) {
    if (app->fullscreen) gtk_window_unfullscreen(GTK_WINDOW(app->window));
    else gtk_window_fullscreen(GTK_WINDOW(app->window));
}

void on_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *data, guint info, guint time, AppState *app) {
    (void)widget; (void)x; (void)y; (void)info;
    gchar **uris = gtk_selection_data_get_uris(data);
    if (uris && uris[0]) {
        gchar *filename = g_filename_from_uri(uris[0], NULL, NULL);
        if (filename) {
            load_image(app, filename, TRUE);
            g_free(filename);
        }
    }
    if (uris) g_strfreev(uris);
    gtk_drag_finish(context, TRUE, FALSE, time);
}

void show_open_dialog(GtkWidget *widget, AppState *app) {
    (void)widget;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Open Image", GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images");
    gtk_file_filter_add_pattern(filter, "*.jpg");
    gtk_file_filter_add_pattern(filter, "*.jpeg");
    gtk_file_filter_add_pattern(filter, "*.png");
    gtk_file_filter_add_pattern(filter, "*.webp");
    gtk_file_filter_add_pattern(filter, "*.gif");
    gtk_file_filter_add_pattern(filter, "*.bmp");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        load_image(app, filename, TRUE);
        g_free(filename);
    }
    gtk_widget_destroy(dialog);
}

void show_about_dialog(GtkWidget *widget, AppState *app) {
    (void)widget;
    const char *authors[] = {"Senior C Developer", NULL};
    gtk_show_about_dialog(GTK_WINDOW(app->window),
                          "program-name", "rezzview",
                          "version", "1.1.0",
                          "comments", "High-performance minimalist image viewer",
                          "license-type", GTK_LICENSE_MIT_X11,
                          "authors", authors,
                          NULL);
}

void action_quit(GtkWidget *w, gpointer data) { (void)w; (void)data; gtk_main_quit(); }
void action_zoom_in(GtkWidget *w, AppState *app) { (void)w; zoom_center(app, 1.1); }
void action_zoom_out(GtkWidget *w, AppState *app) { (void)w; zoom_center(app, 1.0/1.1); }
void action_orig(GtkWidget *w, AppState *app) { (void)w; app->fit = FALSE; app->scale = 1.0; center_image(app); update_title(app); trigger_fast_render(app); }
void action_fit(GtkWidget *w, AppState *app) { (void)w; app->fit = TRUE; update_title(app); trigger_fast_render(app); }
void action_rot(GtkWidget *w, AppState *app) { (void)w; app->angle = (app->angle + 90) % 360; trigger_fast_render(app); }
void action_fs(GtkWidget *w, AppState *app) { (void)w; toggle_fullscreen(app); }

void setup_menu(AppState *app) {
    app->menubar = gtk_menu_bar_new();
    GtkAccelGroup *accel = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(app->window), accel);

    GtkWidget *mi_file = gtk_menu_item_new_with_mnemonic("_File");
    GtkWidget *m_file = gtk_menu_new();
    GtkWidget *mi_open = gtk_menu_item_new_with_mnemonic("_Open Image...");
    GtkWidget *mi_quit = gtk_menu_item_new_with_mnemonic("_Quit");
    gtk_widget_add_accelerator(mi_open, "activate", accel, GDK_KEY_O, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_quit, "activate", accel, GDK_KEY_Q, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    g_signal_connect(mi_open, "activate", G_CALLBACK(show_open_dialog), app);
    g_signal_connect(mi_quit, "activate", G_CALLBACK(action_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_file), mi_open);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_file), mi_quit);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi_file), m_file);

    GtkWidget *mi_view = gtk_menu_item_new_with_mnemonic("_View");
    GtkWidget *m_view = gtk_menu_new();
    GtkWidget *mi_zi = gtk_menu_item_new_with_mnemonic("Zoom _In");
    GtkWidget *mi_zo = gtk_menu_item_new_with_mnemonic("Zoom _Out");
    GtkWidget *mi_z1 = gtk_menu_item_new_with_mnemonic("_Original Size 1:1");
    GtkWidget *mi_fit = gtk_menu_item_new_with_mnemonic("_Fit to Window");
    GtkWidget *mi_rot = gtk_menu_item_new_with_mnemonic("_Rotate Clockwise");
    GtkWidget *mi_fs = gtk_menu_item_new_with_mnemonic("F_ullscreen");
    
    gtk_widget_add_accelerator(mi_zi, "activate", accel, GDK_KEY_plus, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_zo, "activate", accel, GDK_KEY_minus, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_z1, "activate", accel, GDK_KEY_1, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_fit, "activate", accel, GDK_KEY_f, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_rot, "activate", accel, GDK_KEY_r, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_fs, "activate", accel, GDK_KEY_F11, 0, GTK_ACCEL_VISIBLE);
    gtk_widget_add_accelerator(mi_fs, "activate", accel, GDK_KEY_Return, GDK_MOD1_MASK, GTK_ACCEL_VISIBLE);

    g_signal_connect(mi_zi, "activate", G_CALLBACK(action_zoom_in), app);
    g_signal_connect(mi_zo, "activate", G_CALLBACK(action_zoom_out), app);
    g_signal_connect(mi_z1, "activate", G_CALLBACK(action_orig), app);
    g_signal_connect(mi_fit, "activate", G_CALLBACK(action_fit), app);
    g_signal_connect(mi_rot, "activate", G_CALLBACK(action_rot), app);
    g_signal_connect(mi_fs, "activate", G_CALLBACK(action_fs), app);

    gtk_menu_shell_append(GTK_MENU_SHELL(m_view), mi_zi);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_view), mi_zo);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_view), mi_z1);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_view), mi_fit);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_view), mi_rot);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_view), mi_fs);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi_view), m_view);

    GtkWidget *mi_help = gtk_menu_item_new_with_mnemonic("_Help");
    GtkWidget *m_help = gtk_menu_new();
    GtkWidget *mi_about = gtk_menu_item_new_with_mnemonic("_About");
    gtk_widget_add_accelerator(mi_about, "activate", accel, GDK_KEY_F1, 0, GTK_ACCEL_VISIBLE);
    g_signal_connect(mi_about, "activate", G_CALLBACK(show_about_dialog), app);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_help), mi_about);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi_help), m_help);

    gtk_menu_shell_append(GTK_MENU_SHELL(app->menubar), mi_file);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menubar), mi_view);
    gtk_menu_shell_append(GTK_MENU_SHELL(app->menubar), mi_help);
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);

    AppState app = {0};
    app.scale = 1.0;
    app.fit = TRUE;

    app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(app.window), 900, 700);
    gtk_window_set_title(GTK_WINDOW(app.window), "rezzview");
    gtk_window_set_position(GTK_WINDOW(app.window), GTK_WIN_POS_CENTER);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    setup_menu(&app);
    gtk_box_pack_start(GTK_BOX(vbox), app.menubar, FALSE, FALSE, 0);

    app.overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(vbox), app.overlay, TRUE, TRUE, 0);

    app.da = gtk_drawing_area_new();
    gtk_container_add(GTK_CONTAINER(app.overlay), app.da);

    app.open_btn = gtk_button_new_with_label("Open Image File");
    gtk_widget_set_halign(app.open_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app.open_btn, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(app.overlay), app.open_btn);
    g_signal_connect(app.open_btn, "clicked", G_CALLBACK(show_open_dialog), &app);

    gtk_widget_add_events(app.da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                  GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    g_signal_connect(app.window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), &app);
    g_signal_connect(app.window, "window-state-event", G_CALLBACK(on_window_state), &app);
    g_signal_connect(app.da, "draw", G_CALLBACK(on_draw), &app);
    g_signal_connect(app.da, "button-press-event", G_CALLBACK(on_button_press), &app);
    g_signal_connect(app.da, "button-release-event", G_CALLBACK(on_button_release), &app);
    g_signal_connect(app.da, "motion-notify-event", G_CALLBACK(on_motion_notify), &app);
    g_signal_connect(app.da, "scroll-event", G_CALLBACK(on_scroll), &app);

    gtk_drag_dest_set(app.da, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
    gtk_drag_dest_add_uri_targets(app.da);
    g_signal_connect(app.da, "drag-data-received", G_CALLBACK(on_drag_data_received), &app);

    gtk_widget_show_all(app.window);

    if (argc > 1) load_image(&app, argv[1], TRUE);

    gtk_main();

    free_file_list(&app);
    if (app.pixbuf) g_object_unref(app.pixbuf);
    if (app.timer_id > 0) g_source_remove(app.timer_id);

    return 0;
}
