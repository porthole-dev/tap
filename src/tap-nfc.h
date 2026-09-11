/* tap-nfc.h
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

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define TAP_TYPE_NFC (tap_nfc_get_type ())

G_DECLARE_FINAL_TYPE (TapNfc, tap_nfc, TAP, NFC, GObject)

/* Every state the reader can be in, from the point of view of a window that
 * has to show something. There is no "idle": an app whose only job is NFC is
 * either using the reader or explaining why it cannot. */
typedef enum
{
  TAP_NFC_STATE_UNSUPPORTED,  /* no portal, no adapter, or IsNFCPresent false */
  TAP_NFC_STATE_CONNECTING,   /* asking the portal, or waiting on the user */
  TAP_NFC_STATE_DENIED,       /* the user said no, or the grant was revoked */
  TAP_NFC_STATE_OFF,          /* an adapter exists and its radio is switched off */
  TAP_NFC_STATE_SCANNING,     /* polling, nothing in the field */
  TAP_NFC_STATE_TAG,          /* a tag is in the field */
  TAP_NFC_STATE_FAILED,       /* anything else; tap_nfc_get_message() says what */
} TapNfcState;

TapNfc      *tap_nfc_new          (void);
void         tap_nfc_start        (TapNfc  *self);
void         tap_nfc_stop         (TapNfc  *self);
TapNfcState  tap_nfc_get_state    (TapNfc  *self);
const char  *tap_nfc_get_message  (TapNfc  *self);
GDBusProxy  *tap_nfc_get_tag      (TapNfc  *self);
GPtrArray   *tap_nfc_get_records  (TapNfc  *self);

void         tap_nfc_write_async  (TapNfc              *self,
                                   GVariant            *record,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
gboolean     tap_nfc_write_finish (TapNfc        *self,
                                   GAsyncResult  *result,
                                   GError       **error);

G_END_DECLS
