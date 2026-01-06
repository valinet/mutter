/*
 * Copyright (C) 2022 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Carlos Garnacho <carlosg@gnome.org>
 */

#include "config.h"

#include "meta-frames-client.h"
#include "meta-window-tracker.h"

#include <gdk/x11/gdkx.h>
#include <glib-unix.h>
#include <gmodule.h>
#include <X11/extensions/Xfixes.h>

int color_mode = 0;
char* monitored_apps[32];
void *(*p_adw_style_manager_get_default)();
void(*p_adw_style_manager_set_color_scheme)(void*, int);
static pid_t dark_pid = 0;
static pid_t light_pid = 0;

static gboolean should_monitor_color_scheme = TRUE;

typedef void (* InitFunc) (void);

static gboolean
should_load_libadwaita (void)
{
  g_auto(GStrv) desktops = NULL;
  const char *current_desktop;
  const char *platform_library;

  platform_library = g_getenv ("MUTTER_FRAMES_PLATFORM_LIBRARY");

  if (g_strcmp0 (platform_library, "none") == 0)
    return FALSE;

  if (g_strcmp0 (platform_library, "adwaita") == 0)
    return TRUE;

  current_desktop = g_getenv ("XDG_CURRENT_DESKTOP");
  if (current_desktop != NULL)
    desktops = g_strsplit (current_desktop, ":", -1);

  return desktops && g_strv_contains ((const char * const *) desktops, "GNOME");
}

static void
load_libadwaita (void)
{
  GModule *libadwaita;
  InitFunc adw_init;

  libadwaita = g_module_open ("libadwaita-1.so.0", G_MODULE_BIND_LAZY);
  if (!libadwaita)
    return;

  if (!g_module_symbol (libadwaita, "adw_init", (gpointer *) &adw_init))
    return;

  if (!g_module_symbol (libadwaita,
                        "adw_style_manager_get_default",
                        (gpointer *)&p_adw_style_manager_get_default))
    return;

  if (!g_module_symbol (libadwaita,
                        "adw_style_manager_set_color_scheme",
                        (gpointer *)&p_adw_style_manager_set_color_scheme))
    return;

  should_monitor_color_scheme = FALSE;
  adw_init ();
}

static gboolean
on_sigterm (gpointer user_data)
{
  int status = 0;
  if (dark_pid) {
    kill (dark_pid, SIGTERM);
    waitpid(dark_pid, &status, 0);
  }
  if (light_pid) {
    kill (light_pid, SIGTERM);
    waitpid(light_pid, &status, 0);
  }
  exit (0);

  return G_SOURCE_REMOVE;
}

gboolean
meta_frames_client_should_monitor_color_scheme (void)
{
  return should_monitor_color_scheme;
}

float my_roundf(float x)
{
    if (x >= 0.0f)
        return (float)((int)(x + 0.5f));
    else
        return (float)((int)(x - 0.5f));
}

static void
load_compact_header_css (void)
{
  GtkCssProvider *provider;
  GdkDisplay *display;
  char css[512];
  float scale = 0;
  const char* str_scale = g_getenv("WAYLAND_SCALE_FACTOR");
  if (str_scale)
    scale = atof(str_scale);
  if (!scale) scale = 1;
  g_snprintf (css, sizeof css,
    ".titlebar.compact-header, .titlebar.compact-header *, .titlebar.compact-header box, .titlebar.compact-header windowcontrols, .titlebar.compact-header label, .titlebar.compact-header title { min-height: 0px; min-width: 0px; }\n"
    ".titlebar.compact-header windowcontrols button { padding-top: %dpx; padding-bottom: %dpx; padding-left: 0px; padding-right: %dpx; }\n"
    ".titlebar.compact-header windowcontrols button image { -gtk-icon-size: %dpx; }\n"
    ".titlebar.compact-header label { font-size: %fem; }\n",
    (int)my_roundf(scale), (int)my_roundf(scale), (int)(scale * 4),
    (int)(8 * scale), (10 * scale) / 18.0f);

  provider = gtk_css_provider_new ();
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_css_provider_load_from_data (provider, css, -1);
  G_GNUC_END_IGNORE_DEPRECATIONS

  display = gdk_display_get_default ();
  gtk_style_context_add_provider_for_display (
      display,
      GTK_STYLE_PROVIDER (provider),
      GTK_STYLE_PROVIDER_PRIORITY_THEME + 1);

  g_object_unref (provider);
}

int
main (int   argc,
      char *argv[])
{
  if (argc > 1) {
    dark_pid = fork();
    if (!dark_pid) {
      color_mode = 2;
      for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '.')
          break;
        monitored_apps[i - 1] = argv[i];
      }
    } else {
      int needs_light = 0;
      for (int i = 0; i < argc; ++i) {
        if (argv[i][0] == '.')
          needs_light = 1;
      }
      if (needs_light) {
        light_pid = fork();
        if (!light_pid) {
          dark_pid = 0;
          color_mode = 1;
          int now = 0;
          for (int i = 1; i < argc; ++i) {
            if (now)
              monitored_apps[now++ - 1] = argv[i];
            if (argv[i][0] == '.')
              now = 1;
          }
        } else {
          int t = 0;
          for (int i = 1; i < argc; ++i) {
            if (argv[i][0] == '.')
              t++;
            else
              monitored_apps[i - 1 - t] = argv[i];
          }
        }
      } else {
        int t = 0;
        for (int i = 1; i < argc; ++i) {
          if (argv[i][0] == '.')
            t++;
          else
            monitored_apps[i - 1 - t] = argv[i];
        }
      }
    }
  }

  g_autoptr (MetaWindowTracker) window_tracker = NULL;
  GdkDisplay *display;
  GMainLoop *loop;
  Display *xdisplay;

  g_setenv ("GSK_RENDERER", "cairo", TRUE);

  /* We do know the desired GDK backend, don't let
   * anyone tell us otherwise.
   */
  g_unsetenv ("GDK_BACKEND");

  gdk_set_allowed_backends ("x11");

  if (color_mode == 1) g_set_prgname ("mutter-x11-frames_light");
  else if (color_mode == 2) g_set_prgname ("mutter-x11-frames_dark");
  else g_set_prgname ("mutter-x11-frames");

  gtk_init ();
  load_compact_header_css ();

  display = gdk_display_get_default ();

  if (should_load_libadwaita ())
    load_libadwaita ();

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  xdisplay = gdk_x11_display_get_xdisplay (display);
  G_GNUC_END_IGNORE_DEPRECATIONS
  XFixesSetClientDisconnectMode (xdisplay,
                                 XFixesClientDisconnectFlagTerminate);

  window_tracker = meta_window_tracker_new (display);

  loop = g_main_loop_new (NULL, FALSE);
  g_unix_signal_add (SIGTERM, on_sigterm, NULL);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  return 0;
}
