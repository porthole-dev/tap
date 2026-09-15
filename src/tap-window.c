/* tap-window.c
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

/* The window is deliberately dumb: TapNfc emits one "changed" signal and this
 * file rebuilds whatever it shows from the current state. There is no second
 * copy of the state machine here to disagree with the first.
 */

#include "config.h"

#include "tap-window.h"
#include "tap-nfc.h"

#include <glib/gi18n.h>
#include <string.h>

struct _TapWindow
{
  AdwApplicationWindow parent_instance;

  AdwToastOverlay *toast_overlay;
  GtkStack *stack;

  AdwStatusPage *message_page;
  GtkButton *action_button;

  AdwActionRow *type_row;
  AdwActionRow *protocol_row;
  AdwActionRow *uid_row;
  AdwPreferencesGroup *records_group;
  GtkButton *write_button;

  TapNfc *nfc;
  GPtrArray *record_rows;   /* borrowed; the group owns them */
  AdwDialog *write_dialog;  /* borrowed; only set while one is open */
  GtkWidget *write_content; /* the entry row inside it */
  GtkWidget *write_warning; /* ...and the line that says the tag has gone */
  gboolean first_answer;
  gboolean action_opens_settings;
};

G_DEFINE_FINAL_TYPE (TapWindow, tap_window, ADW_TYPE_APPLICATION_WINDOW)

static void
toast (TapWindow  *self,
       const char *text)
{
  adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (text));
}

/* ------------------------------------------------------------ reading */

/* Owned, not borrowed. A string inside a cached property lives exactly as
 * long as the proxy's cache keeps that GVariant, and PropertiesChanged
 * replaces it whenever neard feels like it -- a copy costs nothing here and
 * removes the whole question. */
static char *
proxy_string (GDBusProxy *proxy,
              const char *name)
{
  g_autoptr(GVariant) value = g_dbus_proxy_get_cached_property (proxy, name);

  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    return NULL;

  return g_variant_dup_string (value, NULL);
}

static void
set_value_row (AdwActionRow *row,
               const char   *value)
{
  adw_action_row_set_subtitle (row, value != NULL && *value != '\0'
                                    ? value
                                    : _("Unknown"));
}

static char *
format_uid (GDBusProxy *tag)
{
  g_autoptr(GVariant) value = g_dbus_proxy_get_cached_property (tag, "Uid");
  const guchar *bytes;
  gsize length = 0;
  GString *out;

  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_BYTESTRING))
    return NULL;

  bytes = g_variant_get_fixed_array (value, &length, sizeof (guchar));
  if (length == 0)
    return NULL;

  out = g_string_new (NULL);
  for (gsize i = 0; i < length; i++)
    g_string_append_printf (out, i == 0 ? "%02x" : " %02x", bytes[i]);

  return g_string_free (out, FALSE);
}

/* A record carries its whole content as properties, and which ones are set
 * depends on its type. Show the first that says something. */
static char *
record_value (GDBusProxy *record)
{
  static const char * const keys[] = { "Representation", "URI", "MIME",
                                       "AndroidPackage", "Encoding" };

  for (gsize i = 0; i < G_N_ELEMENTS (keys); i++)
    {
      g_autofree char *value = proxy_string (record, keys[i]);

      if (value != NULL && *value != '\0')
        return g_steal_pointer (&value);
    }

  return NULL;
}

static void
show_records (TapWindow *self)
{
  GPtrArray *records = tap_nfc_get_records (self->nfc);

  for (guint i = 0; i < self->record_rows->len; i++)
    adw_preferences_group_remove (self->records_group,
                                  g_ptr_array_index (self->record_rows, i));
  g_ptr_array_set_size (self->record_rows, 0);

  if (records == NULL || records->len == 0)
    {
      GtkWidget *row = adw_action_row_new ();

      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     _("This tag carries no records"));
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_group_add (self->records_group, row);
      g_ptr_array_add (self->record_rows, row);
      return;
    }

  for (guint i = 0; i < records->len; i++)
    {
      GDBusProxy *record = g_ptr_array_index (records, i);
      GtkWidget *row = adw_action_row_new ();
      g_autofree char *type = proxy_string (record, "Type");
      g_autofree char *value = record_value (record);

      /* Everything on this row came off the tag, so none of it is markup. */
      adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
      adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                     type != NULL ? type : _("Record"));
      if (value != NULL)
        {
          adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
          adw_action_row_set_subtitle_selectable (ADW_ACTION_ROW (row), TRUE);
        }

      adw_preferences_group_add (self->records_group, row);
      g_ptr_array_add (self->record_rows, row);
    }
}

static void
show_tag (TapWindow *self)
{
  GDBusProxy *tag = tap_nfc_get_tag (self->nfc);
  g_autofree char *type = NULL;
  g_autofree char *protocol = NULL;
  g_autofree char *uid = NULL;
  g_autoptr(GVariant) read_only = NULL;
  gboolean writable;

  if (tag == NULL)
    return;

  type = proxy_string (tag, "Type");
  protocol = proxy_string (tag, "Protocol");
  set_value_row (self->type_row, type);
  set_value_row (self->protocol_row, protocol);

  uid = format_uid (tag);
  set_value_row (self->uid_row, uid);

  read_only = g_dbus_proxy_get_cached_property (tag, "ReadOnly");
  writable = read_only == NULL ||
             !g_variant_is_of_type (read_only, G_VARIANT_TYPE_BOOLEAN) ||
             !g_variant_get_boolean (read_only);

  gtk_widget_set_sensitive (GTK_WIDGET (self->write_button), writable);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->write_button),
                               writable ? NULL : _("This tag is read-only."));

  show_records (self);
}

static void
show_message (TapWindow *self)
{
  const char *icon = "io.github.porthole_dev.Tap-symbolic";
  const char *title;
  const char *description;
  const char *button = NULL;
  gboolean opens_settings = FALSE;
  g_autofree char *escaped_title = NULL;
  g_autofree char *escaped_description = NULL;

  switch (tap_nfc_get_state (self->nfc))
    {
    case TAP_NFC_STATE_UNSUPPORTED:
      /* Nearly no machine has an NFC reader. This is not an error and does
       * not get error styling -- there is simply nothing here to offer. */
      title = _("No NFC Reader");
      description = _("This device has no NFC reader, or the system does not "
                      "offer one to apps.");
      break;

    case TAP_NFC_STATE_OFF:
      /* The radio is the user's switch, and this app is deliberately not
       * permitted to touch it -- the portal refuses. So do not describe the
       * route to it, open it. */
      /* GNOME writes its empty states this way: "Bluetooth Turned Off". */
      title = _("NFC Turned Off");
      description = _("Tap cannot switch the radio on. That stays your decision.");
      button = _("Open NFC Settings");
      opens_settings = TRUE;
      break;

    case TAP_NFC_STATE_DENIED:
      /* The icon of the thing being asked for, not a key: that is what every
       * portal does. Camera's dialog shows camera-web-symbolic, Location's
       * shows find-location-symbolic, and the NFC portal's shows
       * nfc-symbolic. */
      title = _("Permission Needed");
      /* The same sentence sandboxed or not. The brief this app was written
       * from expected a native build to share one anonymous grant with every
       * other host process -- but GTK registers the application id with the
       * portal itself, so Tap gets its own grant and its own row either way.
       * Measured on hardware; there is nothing here to apologise for. */
      description = _("Tap has not been allowed to use the NFC reader.");
      /* Not "Ask Again": once a no is stored the portal answers from the
       * store without prompting, so nothing would be asked. Send them where
       * the answer can actually be changed; the window re-checks by itself
       * when it comes back to the front. */
      button = _("Open NFC Settings");
      opens_settings = TRUE;
      break;

    case TAP_NFC_STATE_FAILED:
    default:
      icon = "dialog-warning-symbolic";
      title = _("Something Went Wrong");
      description = tap_nfc_get_message (self->nfc);
      button = _("Try Again");
      break;
    }

  /* AdwStatusPage reads both labels as Pango markup, so a bare ampersand --
   * "Privacy & Security", or anything inside a D-Bus error message -- fails
   * to parse and the label silently keeps whatever it had before. Escaping
   * here keeps the strings themselves plain, which is what translators and
   * error messages both want. */
  escaped_title = g_markup_escape_text (title, -1);
  escaped_description = description != NULL
    ? g_markup_escape_text (description, -1)
    : NULL;

  adw_status_page_set_icon_name (self->message_page, icon);
  adw_status_page_set_title (self->message_page, escaped_title);
  adw_status_page_set_description (self->message_page, escaped_description);

  self->action_opens_settings = opens_settings;
  gtk_widget_set_visible (GTK_WIDGET (self->action_button), button != NULL);
  if (button != NULL)
    gtk_button_set_label (self->action_button, button);
}

/* The first page the window ever shows must not animate into place. Until the
 * portal answers there is nothing true to display, so the window opens empty;
 * crossfading that emptiness into the answer is a flash on every start. Later
 * changes are real changes and do fade. */
static void
show_page (TapWindow  *self,
           const char *name)
{
  if (self->first_answer && g_strcmp0 (name, "starting") != 0)
    {
      self->first_answer = FALSE;
      gtk_stack_set_visible_child_full (self->stack, name,
                                        GTK_STACK_TRANSITION_TYPE_NONE);
      return;
    }

  gtk_stack_set_visible_child_name (self->stack, name);
}


/* Taking the dialog away when the tag leaves loses whatever was typed and
 * explains nothing. GNOME's own habit for an action that cannot be taken is
 * to disable it and say why in place -- and, from
 * gnome-control-center's widget_set_error(), to tell assistive technology the
 * thing is invalid as well as colouring it, which is the half that usually
 * gets forgotten. Put the tag back and the dialog comes straight back to
 * life, still holding what was typed. */
static void
write_dialog_set_tag_present (TapWindow *self,
                              gboolean   present)
{
  adw_alert_dialog_set_response_enabled (ADW_ALERT_DIALOG (self->write_dialog),
                                         "write", present);
  gtk_widget_set_visible (self->write_warning, !present);

  if (present)
    {
      gtk_widget_remove_css_class (self->write_content, "error");
      gtk_accessible_reset_state (GTK_ACCESSIBLE (self->write_content),
                                  GTK_ACCESSIBLE_STATE_INVALID);
    }
  else
    {
      gtk_widget_add_css_class (self->write_content, "error");
      gtk_accessible_update_state (GTK_ACCESSIBLE (self->write_content),
                                   GTK_ACCESSIBLE_STATE_INVALID,
                                   GTK_ACCESSIBLE_INVALID_TRUE,
                                   -1);
    }
}


static void
update (TapWindow *self)
{
  /* Writing is about the tag that was on the phone when the dialog opened.
   * Keep the dialog in step with whether that tag is still there. */
  if (self->write_dialog != NULL)
    write_dialog_set_tag_present (self,
                                  tap_nfc_get_state (self->nfc) == TAP_NFC_STATE_TAG);

  switch (tap_nfc_get_state (self->nfc))
    {
    case TAP_NFC_STATE_TAG:
      show_tag (self);
      show_page (self, "tag");
      break;

    case TAP_NFC_STATE_CONNECTING:
      /* Only while nothing has been shown yet. Connecting happens again
       * whenever the permission is re-checked -- returning from Settings, or
       * the window simply being given focus after it opens -- and blanking
       * the page for the length of a portal round trip each time is the
       * flash that keeps coming back. Leave whatever is on screen until
       * there is a new answer to replace it with. */
      if (self->first_answer)
        show_page (self, "starting");
      break;

    case TAP_NFC_STATE_SCANNING:
      show_page (self, "scanning");
      break;

    default:
      show_message (self);
      show_page (self, "message");
      break;
    }
}

/* ------------------------------------------------------------ writing */

/* neard wants an ISO 639 language for a Text record. The user's own locale is
 * the only honest guess about what they are typing. */
static char *
record_language (void)
{
  const char * const *names = g_get_language_names ();
  g_autofree char *language = NULL;
  char *separator;

  if (names == NULL || names[0] == NULL || g_str_equal (names[0], "C"))
    return g_strdup ("en");

  language = g_strdup (names[0]);
  separator = strpbrk (language, "_.@");
  if (separator != NULL)
    *separator = '\0';

  return g_steal_pointer (&language);
}

/* A person types example.com and means https://example.com. */
static char *
as_uri (const char *text)
{
  if (strstr (text, "://") != NULL || strchr (text, ':') != NULL)
    return g_strdup (text);

  return g_strconcat ("https://", text, NULL);
}

static void
on_write_finished (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr(TapWindow) self = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *message = NULL;

  if (!tap_nfc_write_finish (TAP_NFC (source), result, &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      /* neard's own words, said plainly. "No space left on device" is what a
       * tag with no writable NDEF area answers, and inventing friendlier copy
       * for each errno would only hide which one it was. */
      message = g_strdup_printf (_("Could not write: %s"), error->message);
      toast (self, message);
      return;
    }

  /* neard re-reads the tag after a write, so the records below refresh
   * themselves through the object manager. Nothing to do here but say so. */
  toast (self, _("Tag written"));
}

static void
on_write_response (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG (source);
  g_autoptr(TapWindow) self = user_data;
  const char *response = adw_alert_dialog_choose_finish (dialog, result);

  self->write_dialog = NULL;
  self->write_content = NULL;
  self->write_warning = NULL;
  GtkWidget *kind_row;
  GtkWidget *content_row;
  const char *text;
  GVariantBuilder record;

  if (g_strcmp0 (response, "write") != 0)
    return;

  kind_row = g_object_get_data (G_OBJECT (dialog), "kind-row");
  content_row = g_object_get_data (G_OBJECT (dialog), "content-row");
  text = gtk_editable_get_text (GTK_EDITABLE (content_row));

  if (text == NULL || *text == '\0')
    {
      toast (self, _("Nothing to write"));
      return;
    }

  g_variant_builder_init (&record, G_VARIANT_TYPE_VARDICT);

  if (adw_combo_row_get_selected (ADW_COMBO_ROW (kind_row)) == 0)
    {
      g_autofree char *language = record_language ();

      g_variant_builder_add (&record, "{sv}", "Type", g_variant_new_string ("Text"));
      g_variant_builder_add (&record, "{sv}", "Encoding", g_variant_new_string ("UTF-8"));
      g_variant_builder_add (&record, "{sv}", "Language", g_variant_new_string (language));
      g_variant_builder_add (&record, "{sv}", "Representation", g_variant_new_string (text));
    }
  else
    {
      g_autofree char *uri = as_uri (text);

      g_variant_builder_add (&record, "{sv}", "Type", g_variant_new_string ("URI"));
      g_variant_builder_add (&record, "{sv}", "URI", g_variant_new_string (uri));
    }

  tap_nfc_write_async (self->nfc, g_variant_builder_end (&record),
                       on_write_finished, g_object_ref (self));
}

static void
on_write_clicked (TapWindow *self,
                  GtkButton *button)
{
  g_autoptr(GtkStringList) kinds = gtk_string_list_new (NULL);
  AdwAlertDialog *dialog;
  GtkWidget *group;
  GtkWidget *box;
  GtkWidget *warning;
  GtkWidget *kind_row;
  GtkWidget *content_row;

  gtk_string_list_append (kinds, _("Text"));
  gtk_string_list_append (kinds, _("Link"));

  kind_row = adw_combo_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (kind_row), _("Kind"));
  adw_combo_row_set_model (ADW_COMBO_ROW (kind_row), G_LIST_MODEL (kinds));

  content_row = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (content_row), _("Content"));

  warning = gtk_label_new (_("Put the tag back to write to it"));
  gtk_label_set_wrap (GTK_LABEL (warning), TRUE);
  gtk_widget_set_visible (warning, FALSE);
  gtk_widget_add_css_class (warning, "error");
  gtk_widget_add_css_class (warning, "caption");

  group = adw_preferences_group_new ();
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), kind_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), content_row);

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append (GTK_BOX (box), group);
  gtk_box_append (GTK_BOX (box), warning);

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Write to Tag"), NULL));
  adw_alert_dialog_set_body (dialog,
                             _("Keep the tag against the device until writing finishes."));
  adw_alert_dialog_set_extra_child (dialog, box);
  adw_alert_dialog_add_responses (dialog,
                                  "cancel", _("_Cancel"),
                                  "write", _("_Write"),
                                  NULL);
  adw_alert_dialog_set_response_appearance (dialog, "write", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "write");
  adw_alert_dialog_set_close_response (dialog, "cancel");

  g_object_set_data (G_OBJECT (dialog), "kind-row", kind_row);
  g_object_set_data (G_OBJECT (dialog), "content-row", content_row);

  self->write_dialog = ADW_DIALOG (dialog);
  self->write_content = content_row;
  self->write_warning = warning;

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           on_write_response, g_object_ref (self));
}

/* Open Settings on the NFC page itself, rather than telling someone to go and
 * find it. gnome-control-center exports the standard org.freedesktop.
 * Application interface, and its "launch-panel" action takes the panel id and
 * the subpage -- the same route Settings' own pages use to link to each
 * other. A Flatpak build needs --talk-name=org.gnome.Settings for this. */
static void
open_nfc_settings (TapWindow *self)
{
  g_autoptr(GDBusConnection) bus = NULL;
  g_autoptr(GError) error = NULL;
  GVariantBuilder args;
  GVariantBuilder params;
  GVariantBuilder platform;

  bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (bus == NULL)
    {
      toast (self, error->message);
      return;
    }

  g_variant_builder_init (&args, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&args, "v", g_variant_new_string ("nfc"));

  g_variant_builder_init (&params, G_VARIANT_TYPE ("av"));
  g_variant_builder_add (&params, "v",
                         g_variant_new ("(sav)", "privacy", &args));

  g_variant_builder_init (&platform, G_VARIANT_TYPE_VARDICT);

  g_dbus_connection_call (bus,
                          "org.gnome.Settings", "/org/gnome/Settings",
                          "org.freedesktop.Application", "ActivateAction",
                          g_variant_new ("(sava{sv})", "launch-panel", &params,
                                         &platform),
                          NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_action_clicked (TapWindow *self,
                   GtkButton *button)
{
  if (self->action_opens_settings)
    open_nfc_settings (self);
  else
    tap_nfc_start (self->nfc);
}

/* Coming back from Settings is the moment to look again: a permission the
 * user has just granted is not something anything else tells us about. The
 * radio coming on needs no help -- neard says so itself. */
static void
on_is_active_changed (TapWindow *self)
{
  if (gtk_window_is_active (GTK_WINDOW (self)) &&
      tap_nfc_get_state (self->nfc) == TAP_NFC_STATE_DENIED)
    tap_nfc_start (self->nfc);
}

/* ------------------------------------------------------------ GObject */

static void
tap_window_dispose (GObject *object)
{
  TapWindow *self = TAP_WINDOW (object);

  /* Before anything else: stop the radio looking for tags. Disposing the
   * reader does this too, but saying it here is what makes it true whether
   * the window was closed or the application was quit. */
  if (self->nfc != NULL)
    tap_nfc_stop (self->nfc);

  g_clear_object (&self->nfc);
  g_clear_pointer (&self->record_rows, g_ptr_array_unref);

  gtk_widget_dispose_template (GTK_WIDGET (object), TAP_TYPE_WINDOW);

  G_OBJECT_CLASS (tap_window_parent_class)->dispose (object);
}

static void
tap_window_class_init (TapWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = tap_window_dispose;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/io/github/porthole_dev/Tap/tap-window.ui");

  gtk_widget_class_bind_template_child (widget_class, TapWindow, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, stack);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, message_page);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, action_button);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, type_row);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, protocol_row);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, uid_row);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, records_group);
  gtk_widget_class_bind_template_child (widget_class, TapWindow, write_button);

  gtk_widget_class_bind_template_callback (widget_class, on_action_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_write_clicked);
}

static void
tap_window_init (TapWindow *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->record_rows = g_ptr_array_new ();
  self->first_answer = TRUE;
  self->nfc = tap_nfc_new ();

  g_signal_connect_swapped (self->nfc, "changed", G_CALLBACK (update), self);
  g_signal_connect (self, "notify::is-active",
                    G_CALLBACK (on_is_active_changed), NULL);

  update (self);
  tap_nfc_start (self->nfc);
}

TapWindow *
tap_window_new (GtkApplication *application)
{
  return g_object_new (TAP_TYPE_WINDOW, "application", application, NULL);
}
