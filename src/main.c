/* main.c
 *
 * Copyright 2026 Giuseppe Maggio
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <adwaita.h>
#include <glib/gi18n.h>

#include "tap-window.h"

static void
on_about (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  GtkApplication *application = user_data;
  static const char *developers[] = { "Giuseppe Maggio", NULL };
  AdwDialog *dialog;

  /* Built from the AppStream metainfo, which is how GNOME's own apps do it:
   * the name, the summary, the licence, the links and the release notes are
   * already written down there, and an About dialog that restates them in C
   * is a second copy to drift. What is left here is what appdata has no field
   * for. */
  dialog = adw_about_dialog_new_from_appdata ("/io/github/porthole_dev/Tap/io.github.porthole_dev.Tap.metainfo.xml",
                                              APP_VERSION);

  adw_about_dialog_set_developers (ADW_ABOUT_DIALOG (dialog), developers);
  adw_about_dialog_set_copyright (ADW_ABOUT_DIALOG (dialog), "© 2026 Giuseppe Maggio");
  /* Translators: replace with your own name and email */
  adw_about_dialog_set_translator_credits (ADW_ABOUT_DIALOG (dialog), _("translator-credits"));

  adw_dialog_present (dialog, GTK_WIDGET (gtk_application_get_active_window (application)));
}


static void
on_quit (GSimpleAction *action,
         GVariant      *parameter,
         gpointer       user_data)
{
  GtkApplication *application = user_data;
  GtkWindow *window = gtk_application_get_active_window (application);

  /* Close the window rather than quitting outright: closing runs the
   * window's dispose, and that is what stops the radio polling. */
  if (window != NULL)
    gtk_window_close (window);
  else
    g_application_quit (G_APPLICATION (application));
}

static void
on_activate (GtkApplication *application)
{
  GtkWindow *window = gtk_application_get_active_window (application);

  if (window == NULL)
    window = GTK_WINDOW (tap_window_new (application));

  gtk_window_present (window);
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(AdwApplication) application = NULL;
  static const GActionEntry actions[] = {
    { "about", on_about },
    { "quit", on_quit },
  };

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  application = adw_application_new (APP_ID, G_APPLICATION_DEFAULT_FLAGS);

  g_action_map_add_action_entries (G_ACTION_MAP (application), actions,
                                   G_N_ELEMENTS (actions), application);
  gtk_application_set_accels_for_action (GTK_APPLICATION (application),
                                         "app.quit",
                                         (const char *[]) { "<primary>q", NULL });

  g_signal_connect (application, "activate", G_CALLBACK (on_activate), NULL);

  return g_application_run (G_APPLICATION (application), argc, argv);
}
