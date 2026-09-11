/* tap-nfc.c
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

/* Everything this app knows about NFC, which is deliberately little: it asks
 * org.freedesktop.portal.NFC for a connection and then speaks plain neard on
 * it. The interface on the far side is the same one an unsandboxed program
 * gets from the system bus -- only the way of reaching it differs -- so the
 * portal costs one function, connect(), and nothing else in this file knows
 * the portal exists.
 */

#include "config.h"

#include "tap-nfc.h"

#include <gio/gunixfdlist.h>
#include <glib/gi18n.h>
#include <unistd.h>

#define PORTAL_BUS_NAME      "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH   "/org/freedesktop/portal/desktop"
#define PORTAL_NFC_IFACE     "org.freedesktop.portal.NFC"
#define PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define PORTAL_NOT_ALLOWED   "org.freedesktop.portal.Error.NotAllowed"

#define NEARD_BUS_NAME       "org.neard"
#define NEARD_ADAPTER_IFACE  "org.neard.Adapter"
#define NEARD_TAG_IFACE      "org.neard.Tag"
#define NEARD_RECORD_IFACE   "org.neard.Record"

/* How long a tag may vanish for before the window admits it is gone.
 *
 * This was 1200ms for a tag that had been present briefly and 250ms
 * otherwise, to stop the window flashing between "Ready to Scan" and the tag
 * when a card at the edge of the antenna dropped and came back. Then it was
 * measured: a card sitting on the phone produced no add and no remove for 162
 * seconds. The flapping that justified the long wait came from handling the
 * card, which is also exactly when a removal is real, so the wait was being
 * paid on every removal to hide something that does not happen on its own.
 *
 * What is left is a debounce below the threshold of noticing, so a genuine
 * lift clears the screen at once and only a sub-perceptual blip is absorbed.
 *
 * ponytail: one number. If a weaker antenna ever brings the flashing back,
 * the fix is to stop redrawing for a UID that is already on screen, not to
 * make everyone wait again. */
#define TAG_GRACE_MS 150

/* Some cards are dropped and re-found by neard while lying perfectly still.
 * A MIFARE Classic is the case measured here: neard probes whether a tag is
 * still in the field by authenticating to its MAD sector with the public key,
 * and a card that was never NDEF formatted refuses every time, so neard
 * concludes it has gone. It returns within a few hundred milliseconds, every
 * time, for as long as the card sits there.
 *
 * That cannot be fixed from here and it cannot be guessed at either, so the
 * app learns it: a card that comes back with the same UID within
 * TAG_CYCLE_WINDOW_MS of vanishing is remembered as one that does this, and
 * from then on its disappearances are given a longer benefit of the doubt.
 * Cards that do not do it, such as the Type 4 tested alongside, keep the
 * short one and clear the screen as soon as they are lifted. */
#define TAG_CYCLE_WINDOW_MS 1000
#define TAG_GRACE_CYCLING_MS 800

struct _TapNfc
{
  GObject parent_instance;

  GCancellable *cancellable;

  /* The session bus connection AccessNFC and OpenNFCRemote were called on.
   *
   * The portal ties the filtered connection's lifetime to THIS, not to the
   * file descriptor it hands out: let it go and xdg-desktop-portal reads
   * that as the application leaving the bus and tears the proxy down about
   * 150ms later, after which every call on `neard` below fails with "The
   * connection is closed". So it is held for as long as this object lives.
   * (An application gets this for free by having a main loop; a script has
   * to hold it on purpose. See docs/NFC-FOR-APPLICATIONS.md.) */
  GDBusConnection *session;

  char *request_path;
  guint response_id;

  GDBusConnection *neard;       /* the filtered connection from the portal */
  GDBusObjectManager *manager;

  GDBusProxy *adapter;
  GDBusProxy *tag;
  GPtrArray *records;           /* GDBusProxy * on org.neard.Record */

  TapNfcState state;
  char *message;

  gboolean want_scan;
  gboolean poll_pending;
  gboolean poll_failed;
  gboolean connecting;
  guint tag_grace_id;
  gint64 handled_at;
  gint64 tag_vanished_at;
  char *last_uid;      /* the UID that vanished most recently */
  char *cycling_uid;   /* ...and the one known to come back by itself */
};

G_DEFINE_FINAL_TYPE (TapNfc, tap_nfc, G_TYPE_OBJECT)

enum {
  CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void connect_to_portal   (TapNfc *self);
static void refresh             (TapNfc *self);
static gboolean on_tag_grace_expired (gpointer user_data);
static void on_neard_connected  (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      user_data);

/* ------------------------------------------------------------------ state */

static void
set_state (TapNfc      *self,
           TapNfcState  state,
           const char  *message)
{
  if (state != self->state && (state == TAP_NFC_STATE_TAG || self->state == TAP_NFC_STATE_TAG))
    g_debug ("%s after %.1f ms in this handler",
             state == TAP_NFC_STATE_TAG ? "showing a tag" : "clearing the tag",
             (g_get_monotonic_time () - self->handled_at) / 1000.0);

  self->state = state;
  /* Tracked separately from the state, because the state starts out as
   * CONNECTING so that the window's first frame is not a wrong answer -- and
   * a start() that read the state alone would take that for a connection
   * already in flight and never make one. */
  self->connecting = state == TAP_NFC_STATE_CONNECTING;
  g_clear_pointer (&self->message, g_free);
  self->message = g_strdup (message);

  /* One signal, no detail: the window is cheap to rebuild and a reader who
   * has to match six signals to six widgets has to hold more in their head.
   * ponytail: rebuild-everything; split it if a tag list ever gets long. */
  g_signal_emit (self, signals[CHANGED], 0);
}

static gboolean
proxy_bool (GDBusProxy *proxy,
            const char *name)
{
  g_autoptr(GVariant) value = NULL;

  if (proxy == NULL)
    return FALSE;

  value = g_dbus_proxy_get_cached_property (proxy, name);

  return value != NULL &&
         g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) &&
         g_variant_get_boolean (value);
}

/* True once the operation this callback belongs to was cancelled, which on
 * this object means dispose() ran: `self` is gone and must not be touched. */
static gboolean
was_cancelled (const GError *error)
{
  return error != NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

/* What to put in front of a person. A GDBus error message carries the remote
 * error name glued to the front of it, which is useful in a log and noise in
 * a status page. */
static char *
user_message (const GError *error)
{
  g_autoptr(GError) copy = NULL;

  if (error == NULL)
    return g_strdup (_("The NFC service did not say what went wrong."));

  copy = g_error_copy (error);
  g_dbus_error_strip_remote_error (copy);

  return g_strdup (copy->message);
}

static char *
tag_uid (GDBusProxy *tag)
{
  g_autoptr (GVariant) value = NULL;
  const guchar *bytes;
  gsize length = 0;
  GString *out;

  if (tag == NULL)
    return NULL;

  value = g_dbus_proxy_get_cached_property (tag, "Uid");
  if (value == NULL || !g_variant_is_of_type (value, G_VARIANT_TYPE_BYTESTRING))
    return NULL;

  bytes = g_variant_get_fixed_array (value, &length, sizeof (guchar));
  if (length == 0)
    return NULL;

  out = g_string_new (NULL);
  for (gsize i = 0; i < length; i++)
    g_string_append_printf (out, "%02x", bytes[i]);

  return g_string_free (out, FALSE);
}


/* ------------------------------------------------------------- poll loop */

static void
on_poll_started (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  TapNfc *self;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;
  self->poll_pending = FALSE;

  if (error != NULL)
    {
      /* A refusal here is often not a failure at all. neard answers
       * "Resource busy" when a tag is ALREADY in the field -- there is
       * nothing to poll for, and the tag's own InterfacesAdded may already
       * have arrived. "No such device" is a radio switched off between the
       * property read and this call. So look again before saying anything,
       * and only complain if looking found nothing better to show.
       *
       * Asking again immediately would turn a genuinely busy adapter into a
       * spin, so polling stays off until something actually changes: a tag
       * arriving or leaving, the radio cycling, or the user retrying. */
      g_autofree char *message = user_message (error);

      self->poll_failed = TRUE;
      refresh (self);

      if (self->state == TAP_NFC_STATE_SCANNING)
        set_state (self, TAP_NFC_STATE_FAILED, message);
    }
}

static void
maybe_poll (TapNfc *self)
{
  if (!self->want_scan || self->poll_pending || self->poll_failed ||
      self->adapter == NULL)
    return;

  if (proxy_bool (self->adapter, "Polling"))
    return;

  self->poll_pending = TRUE;
  g_dbus_proxy_call (self->adapter, "StartPollLoop",
                     g_variant_new ("(s)", "Initiator"),
                     G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                     on_poll_started, self);
}

/* -------------------------------------------------------------- the tree */

static int
cmp_object_path (gconstpointer a,
                 gconstpointer b)
{
  return g_strcmp0 (g_dbus_object_get_object_path ((GDBusObject *) a),
                    g_dbus_object_get_object_path ((GDBusObject *) b));
}

static void
refresh (TapNfc *self)
{
  g_autoptr(GDBusProxy) adapter = NULL;
  g_autoptr(GDBusProxy) tag = NULL;
  g_autoptr(GPtrArray) records = NULL;
  GList *objects;

  if (self->manager == NULL)
    return;

  /* Stamped here so the log can say what this code cost, separately from what
   * neard and the radio cost before the signal ever reached us. */
  self->handled_at = g_get_monotonic_time ();

  records = g_ptr_array_new_with_free_func (g_object_unref);

  /* Sorted, so that a machine with two adapters picks the same one twice.
   * Do not assume nfc0: adapters are numbered and the number is neard's. */
  objects = g_list_sort (g_dbus_object_manager_get_objects (self->manager),
                         cmp_object_path);

  for (GList *l = objects; l != NULL; l = l->next)
    {
      GDBusObject *object = l->data;

      if (adapter == NULL)
        adapter = G_DBUS_PROXY (g_dbus_object_get_interface (object, NEARD_ADAPTER_IFACE));
      if (tag == NULL)
        tag = G_DBUS_PROXY (g_dbus_object_get_interface (object, NEARD_TAG_IFACE));
    }

  if (tag != NULL)
    {
      const char *tag_path = g_dbus_proxy_get_object_path (tag);

      for (GList *l = objects; l != NULL; l = l->next)
        {
          GDBusObject *object = l->data;
          g_autoptr(GDBusInterface) record = NULL;

          if (!g_str_has_prefix (g_dbus_object_get_object_path (object), tag_path))
            continue;

          record = g_dbus_object_get_interface (object, NEARD_RECORD_IFACE);
          if (record != NULL)
            g_ptr_array_add (records, g_steal_pointer (&record));
        }
    }

  g_list_free_full (objects, g_object_unref);

  /* The tag blinked out. Keep showing it for a moment -- it is usually the
   * same card, still on the phone, that the antenna lost its grip on -- while
   * letting the adapter go straight back to looking for it. */
  if (tag == NULL && self->tag != NULL && adapter != NULL &&
      proxy_bool (adapter, "Powered"))
    {
      if (self->tag_grace_id == 0)
        {
          g_autofree char *uid = tag_uid (self->tag);
          gboolean cycles = uid != NULL && g_strcmp0 (uid, self->cycling_uid) == 0;

          g_clear_pointer (&self->last_uid, g_free);
          self->last_uid = g_steal_pointer (&uid);
          self->tag_vanished_at = g_get_monotonic_time ();

          self->tag_grace_id = g_timeout_add (cycles ? TAG_GRACE_CYCLING_MS
                                                     : TAG_GRACE_MS,
                                              on_tag_grace_expired, self);
        }

      self->poll_failed = FALSE;
      g_set_object (&self->adapter, adapter);
      maybe_poll (self);
      return;
    }

  g_clear_handle_id (&self->tag_grace_id, g_source_remove);

  /* A tag arriving or leaving is the event that makes polling worth trying
   * again after neard refused it. */
  if (self->tag != tag)
    self->poll_failed = FALSE;

  /* Back so soon, and the same card? Then it never left, and this one is a
   * card neard cannot keep hold of. Remember it. */
  if (self->tag == NULL && tag != NULL && self->last_uid != NULL &&
      g_get_monotonic_time () - self->tag_vanished_at <
        TAG_CYCLE_WINDOW_MS * G_GINT64_CONSTANT (1000))
    {
      g_autofree char *uid = tag_uid (tag);

      if (uid != NULL && g_strcmp0 (uid, self->last_uid) == 0 &&
          g_strcmp0 (uid, self->cycling_uid) != 0)
        {
          g_debug ("%s comes back by itself; giving it a longer grace", uid);
          g_free (self->cycling_uid);
          self->cycling_uid = g_steal_pointer (&uid);
        }
    }

  g_set_object (&self->adapter, adapter);
  g_set_object (&self->tag, tag);
  g_clear_pointer (&self->records, g_ptr_array_unref);
  self->records = g_ptr_array_ref (records);

  if (self->adapter == NULL)
    set_state (self, TAP_NFC_STATE_UNSUPPORTED, NULL);
  else if (!proxy_bool (self->adapter, "Powered"))
    {
      /* The switch going off and on again is a fresh start for the radio. */
      self->poll_failed = FALSE;
      set_state (self, TAP_NFC_STATE_OFF, NULL);
    }
  else if (self->tag != NULL)
    set_state (self, TAP_NFC_STATE_TAG, NULL);
  else
    {
      set_state (self, TAP_NFC_STATE_SCANNING, NULL);
      maybe_poll (self);
    }
}

/* The tag did not come back. Forget it, and let refresh () say so -- with
 * self->tag cleared, the grace branch above cannot re-arm. */
static gboolean
on_tag_grace_expired (gpointer user_data)
{
  TapNfc *self = user_data;

  self->tag_grace_id = 0;
  g_clear_object (&self->tag);
  refresh (self);

  return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------ the portal, once */

static void
clear_request (TapNfc *self)
{
  if (self->response_id != 0)
    {
      g_dbus_connection_signal_unsubscribe (self->session, self->response_id);
      self->response_id = 0;
    }
  g_clear_pointer (&self->request_path, g_free);
}

static void
on_manager_ready (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  GDBusObjectManager *manager;
  TapNfc *self;

  manager = g_dbus_object_manager_client_new_finish (result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;

  if (manager == NULL)
    {
      g_autofree char *message = user_message (error);

      set_state (self, TAP_NFC_STATE_FAILED, message);
      return;
    }

  self->manager = manager;

  /* Everything that can change the answer, on one handler. Swapped, so each
   * one calls refresh (self) whatever else it was going to pass. */
  g_signal_connect_swapped (manager, "object-added",
                            G_CALLBACK (refresh), self);
  g_signal_connect_swapped (manager, "object-removed",
                            G_CALLBACK (refresh), self);
  g_signal_connect_swapped (manager, "interface-proxy-properties-changed",
                            G_CALLBACK (refresh), self);

  refresh (self);
}

static void
on_neard_closed (GDBusConnection *connection,
                 gboolean         remote_peer_vanished,
                 GError          *error,
                 gpointer         user_data)
{
  TapNfc *self = user_data;

  /* The portal drops the proxy when the grant is revoked, and xdg-dbus-proxy
   * dying looks the same from here. Do NOT reconnect on our own: the portal
   * caps live proxies per sender and does not reclaim a slot when the client
   * closes its end, so a reconnect loop would burn the app's whole allowance
   * in a second. Retrying is the user's decision, through the button. */
  g_clear_object (&self->manager);
  g_clear_object (&self->adapter);
  g_clear_object (&self->tag);

  set_state (self, TAP_NFC_STATE_FAILED,
             _("The connection to the NFC service was closed."));
}

static void
on_remote_opened (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GSocket) socket = NULL;
  g_autoptr(GIOStream) stream = NULL;
  TapNfc *self;
  gint32 handle;
  int fd;

  reply = g_dbus_connection_call_with_unix_fd_list_finish (G_DBUS_CONNECTION (source),
                                                           &fd_list, result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;

  if (reply == NULL)
    {
      g_autofree char *name = g_dbus_error_get_remote_error (error);

      if (g_strcmp0 (name, PORTAL_NOT_ALLOWED) == 0)
        set_state (self, TAP_NFC_STATE_DENIED, NULL);
      else
        {
          g_autofree char *message = user_message (error);

          set_state (self, TAP_NFC_STATE_FAILED, message);
        }
      return;
    }

  g_variant_get (reply, "(h)", &handle);

  fd = fd_list != NULL ? g_unix_fd_list_get (fd_list, handle, &error) : -1;
  if (fd < 0)
    {
      g_autofree char *message = user_message (error);

      set_state (self, TAP_NFC_STATE_FAILED, message);
      return;
    }

  /* NOT g_dbus_connection_new_for_address ("unix:fd=N"): that address form is
   * for a server to listen on an already-open fd, and GLib refuses it here
   * with "the unix transport requires exactly one of the keys 'path' or
   * 'abstract' to be set". A client needs a real GSocketConnection so that
   * SASL EXTERNAL can pass credentials -- the factory picks GUnixConnection,
   * which can, where the plain base class cannot. */
  socket = g_socket_new_from_fd (fd, &error);
  if (socket == NULL)
    {
      g_autofree char *message = user_message (error);

      close (fd);
      set_state (self, TAP_NFC_STATE_FAILED, message);
      return;
    }

  stream = G_IO_STREAM (g_socket_connection_factory_create_connection (socket));

  /* MESSAGE_BUS_CONNECTION is not optional. The far end is xdg-dbus-proxy
   * relaying to a real dbus-daemon, which refuses every message from a peer
   * that has not sent Hello -- without this flag GDBusConnection never sends
   * it and every call comes back "Hello() was not yet called". */
  g_dbus_connection_new (stream, NULL,
                         G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                         G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                         NULL, self->cancellable, on_neard_connected, self);
}

static void
open_remote (TapNfc *self)
{
  GVariantBuilder options;

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);

  g_dbus_connection_call_with_unix_fd_list (self->session,
                                            PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
                                            PORTAL_NFC_IFACE, "OpenNFCRemote",
                                            g_variant_new ("(a{sv})", &options),
                                            G_VARIANT_TYPE ("(h)"),
                                            G_DBUS_CALL_FLAGS_NONE, -1,
                                            NULL, self->cancellable,
                                            on_remote_opened, self);
}

static void
on_access_response (GDBusConnection *connection,
                    const char      *sender_name,
                    const char      *object_path,
                    const char      *interface_name,
                    const char      *signal_name,
                    GVariant        *parameters,
                    gpointer         user_data)
{
  TapNfc *self = user_data;
  g_autoptr(GVariant) results = NULL;
  guint32 response;

  g_variant_get (parameters, "(u@a{sv})", &response, &results);
  clear_request (self);

  /* 0 granted, 1 the user cancelled, 2 anything else. Only the first is a
   * yes, and a no is not an error -- the user is allowed to say no. */
  if (response != 0)
    {
      set_state (self, TAP_NFC_STATE_DENIED, NULL);
      return;
    }

  open_remote (self);
}

static void
on_access_called (GObject      *source,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  TapNfc *self;
  const char *handle;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;

  if (reply == NULL)
    {
      g_autofree char *message = user_message (error);

      clear_request (self);
      set_state (self, TAP_NFC_STATE_FAILED, message);
      return;
    }

  g_variant_get (reply, "(&o)", &handle);

  if (g_strcmp0 (handle, self->request_path) != 0)
    {
      /* The path is predictable and we subscribed to the prediction before
       * calling, because a grant that already exists is answered immediately
       * and a subscription made afterwards can miss the Response. If a
       * portal ever hands back a different path, follow it -- and accept
       * that this one call has a race the prediction exists to avoid. */
      g_warning ("portal returned request path %s, expected %s",
                 handle, self->request_path);

      if (self->response_id != 0)
        g_dbus_connection_signal_unsubscribe (self->session, self->response_id);

      g_free (self->request_path);
      self->request_path = g_strdup (handle);
      self->response_id = g_dbus_connection_signal_subscribe (self->session,
                                                              PORTAL_BUS_NAME,
                                                              PORTAL_REQUEST_IFACE,
                                                              "Response",
                                                              self->request_path,
                                                              NULL,
                                                              G_DBUS_SIGNAL_FLAGS_NONE,
                                                              on_access_response,
                                                              self, NULL);
    }
}

static void
access_nfc (TapNfc *self)
{
  g_autofree char *token = NULL;
  g_autofree char *sender = NULL;
  GVariantBuilder options;

  /* /org/freedesktop/portal/desktop/request/<sender>/<token>, with the
   * leading colon of the unique name dropped and its dots turned into
   * underscores. Every portal request works this way. */
  token = g_strdup_printf ("tap_%u", g_random_int ());
  sender = g_strdup (g_dbus_connection_get_unique_name (self->session) + 1);
  for (char *p = sender; *p != '\0'; p++)
    if (*p == '.')
      *p = '_';

  self->request_path = g_strdup_printf ("%s/request/%s/%s",
                                        PORTAL_OBJECT_PATH, sender, token);
  self->response_id = g_dbus_connection_signal_subscribe (self->session,
                                                          PORTAL_BUS_NAME,
                                                          PORTAL_REQUEST_IFACE,
                                                          "Response",
                                                          self->request_path,
                                                          NULL,
                                                          G_DBUS_SIGNAL_FLAGS_NONE,
                                                          on_access_response,
                                                          self, NULL);

  g_variant_builder_init (&options, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&options, "{sv}", "handle_token",
                         g_variant_new_string (token));

  g_dbus_connection_call (self->session, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
                          PORTAL_NFC_IFACE, "AccessNFC",
                          g_variant_new ("(a{sv})", &options),
                          G_VARIANT_TYPE ("(o)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                          on_access_called, self);
}

static void
on_neard_connected (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  GDBusConnection *connection;
  TapNfc *self;

  connection = g_dbus_connection_new_finish (result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;

  if (connection == NULL)
    {
      g_autofree char *message = user_message (error);

      set_state (self, TAP_NFC_STATE_FAILED, message);
      return;
    }

  self->neard = connection;
  g_dbus_connection_set_exit_on_close (connection, FALSE);
  g_signal_connect (connection, "closed", G_CALLBACK (on_neard_closed), self);

  /* From here on this is ordinary neard: the same object manager an
   * unsandboxed program would build on the system bus. */
  g_dbus_object_manager_client_new (connection,
                                    G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                    NEARD_BUS_NAME, "/",
                                    NULL, NULL, NULL,
                                    self->cancellable,
                                    on_manager_ready, self);
}

static void
on_is_present (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;
  g_autoptr(GVariant) value = NULL;
  TapNfc *self;

  reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;

  /* No portal, or a portal without this interface. Nearly no machine has an
   * adapter; that is what this property is for. Not an error, and not a
   * dialog -- the app just has nothing to offer here. */
  if (reply == NULL)
    {
      set_state (self, TAP_NFC_STATE_UNSUPPORTED, NULL);
      return;
    }

  g_variant_get (reply, "(v)", &value);

  if (!g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN) ||
      !g_variant_get_boolean (value))
    {
      set_state (self, TAP_NFC_STATE_UNSUPPORTED, NULL);
      return;
    }

  access_nfc (self);
}

static void
on_session_bus (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  GDBusConnection *connection;
  TapNfc *self;

  connection = g_bus_get_finish (result, &error);
  if (was_cancelled (error))
    return;

  self = user_data;

  if (connection == NULL)
    {
      g_autofree char *message = user_message (error);

      set_state (self, TAP_NFC_STATE_FAILED, message);
      return;
    }

  self->session = connection;
  connect_to_portal (self);
}

static void
connect_to_portal (TapNfc *self)
{
  if (self->session == NULL)
    {
      g_bus_get (G_BUS_TYPE_SESSION, self->cancellable, on_session_bus, self);
      return;
    }

  g_dbus_connection_call (self->session, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
                          "org.freedesktop.DBus.Properties", "Get",
                          g_variant_new ("(ss)", PORTAL_NFC_IFACE, "IsNFCPresent"),
                          G_VARIANT_TYPE ("(v)"),
                          G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                          on_is_present, self);
}

/* --------------------------------------------------------------- writing */

static void
on_write_done (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = NULL;

  reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);

  if (reply == NULL)
    {
      g_dbus_error_strip_remote_error (error);
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  g_task_return_boolean (task, TRUE);
}

void
tap_nfc_write_async (TapNfc              *self,
                     GVariant            *record,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (TAP_IS_NFC (self));

  task = g_task_new (self, self->cancellable, callback, user_data);

  if (self->tag == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "%s", _("The tag was taken away before it could be written."));
      g_object_unref (task);
      return;
    }

  /* Tag.Write is permitted by the portal's filter, on purpose: withholding
   * it would make the portal useless for the obvious write-a-tag app, and
   * the user consented to NFC the way camera permission covers recording
   * and not just preview. */
  g_dbus_proxy_call (self->tag, "Write",
                     g_variant_new ("(@a{sv})", record),
                     G_DBUS_CALL_FLAGS_NONE, -1, self->cancellable,
                     on_write_done, task);
}

gboolean
tap_nfc_write_finish (TapNfc        *self,
                      GAsyncResult  *result,
                      GError       **error)
{
  g_return_val_if_fail (g_task_is_valid (result, self), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/* ------------------------------------------------------------------- API */

void
tap_nfc_start (TapNfc *self)
{
  g_return_if_fail (TAP_IS_NFC (self));

  self->want_scan = TRUE;
  self->poll_failed = FALSE;

  if (self->manager != NULL)
    {
      refresh (self);
      return;
    }

  if (self->connecting)
    return;

  set_state (self, TAP_NFC_STATE_CONNECTING, NULL);
  connect_to_portal (self);
}

void
tap_nfc_stop (TapNfc *self)
{
  g_return_if_fail (TAP_IS_NFC (self));

  self->want_scan = FALSE;

  if (!proxy_bool (self->adapter, "Polling"))
    return;

  /* Synchronous on purpose: this runs as the window closes, and an async
   * reply would never be dispatched. Leaving a phone polling is a battery
   * cost the user did not ask for.
   * ponytail: two seconds and give up -- if neard is wedged, exiting wins. */
  g_dbus_proxy_call_sync (self->adapter, "StopPollLoop", NULL,
                          G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
}

TapNfcState
tap_nfc_get_state (TapNfc *self)
{
  g_return_val_if_fail (TAP_IS_NFC (self), TAP_NFC_STATE_UNSUPPORTED);

  return self->state;
}

const char *
tap_nfc_get_message (TapNfc *self)
{
  g_return_val_if_fail (TAP_IS_NFC (self), NULL);

  return self->message;
}

GDBusProxy *
tap_nfc_get_tag (TapNfc *self)
{
  g_return_val_if_fail (TAP_IS_NFC (self), NULL);

  return self->tag;
}

GPtrArray *
tap_nfc_get_records (TapNfc *self)
{
  g_return_val_if_fail (TAP_IS_NFC (self), NULL);

  return self->records;
}

/* ------------------------------------------------------------ GObject */

static void
tap_nfc_dispose (GObject *object)
{
  TapNfc *self = TAP_NFC (object);

  tap_nfc_stop (self);

  g_cancellable_cancel (self->cancellable);
  g_clear_handle_id (&self->tag_grace_id, g_source_remove);
  clear_request (self);

  g_clear_object (&self->manager);
  g_clear_object (&self->adapter);
  g_clear_object (&self->tag);
  g_clear_pointer (&self->records, g_ptr_array_unref);

  if (self->neard != NULL)
    g_signal_handlers_disconnect_by_func (self->neard, on_neard_closed, self);
  g_clear_object (&self->neard);
  g_clear_object (&self->session);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->message, g_free);
  g_clear_pointer (&self->last_uid, g_free);
  g_clear_pointer (&self->cycling_uid, g_free);

  G_OBJECT_CLASS (tap_nfc_parent_class)->dispose (object);
}

static void
tap_nfc_class_init (TapNfcClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = tap_nfc_dispose;

  signals[CHANGED] = g_signal_new ("changed", TAP_TYPE_NFC,
                                   G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL,
                                   G_TYPE_NONE, 0);
}

static void
tap_nfc_init (TapNfc *self)
{
  self->cancellable = g_cancellable_new ();
  self->records = g_ptr_array_new_with_free_func (g_object_unref);
  self->state = TAP_NFC_STATE_CONNECTING;
}

TapNfc *
tap_nfc_new (void)
{
  return g_object_new (TAP_TYPE_NFC, NULL);
}
