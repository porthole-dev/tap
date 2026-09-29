# Tap

[Website](https://porthole-dev.github.io/porthole/) · [Downloads](https://porthole-dev.github.io/porthole/downloads/) · [Device support](https://porthole-dev.github.io/porthole/devices/)

Read and write NFC tags, through `org.freedesktop.portal.NFC`.

Hold a tag against the back of the device and Tap shows its type, its
protocol, its UID and any NDEF records on it. With a writable tag in the
field it can store a text or a link record.

## Build from source

```sh
meson setup _build
meson compile -C _build
meson test -C _build          # validates the desktop and metainfo files
meson install -C _build
```

Needs GTK 4.14, libadwaita 1.6 and `blueprint-compiler` at build time.

As a Flatpak, the manifest is `build-aux/io.github.porthole_dev.Tap.json`:

```sh
flatpak-builder --user --install --force-clean _flatpak build-aux/io.github.porthole_dev.Tap.json
```

On Nura, the package is `temp/tap` in
[porthole-dev/pmaports](https://github.com/porthole-dev/pmaports), built from
this repository's release tarballs.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Licence

GPL-3.0-or-later, see [COPYING](COPYING).

## Project links

[Device support](https://github.com/porthole-dev/porthole) ·
[Packaged builds](https://github.com/porthole-dev/pmos-packages) ·
[Contribution policy](https://github.com/porthole-dev/.github/blob/main/CONTRIBUTING.md)

Independent project; not endorsed by Nura or GNOME. See [AI.md](AI.md)
for the assistance policy.

<details>
<summary><strong>Portal integration and permissions</strong></summary>

## What talking to the portal costs

One function. `connect_to_portal()` in `src/tap-nfc.c` reads `IsNFCPresent`,
calls `AccessNFC`, waits for the `Response`, calls `OpenNFCRemote` and wraps
the file descriptor it gets back. Everything after that is ordinary neard on
an ordinary `GDBusObjectManager` -- the same code an unsandboxed program
would write against the system bus.

Three things in there are not guessable and are commented where they happen:

- the filtered connection's lifetime is tied to **the session bus connection**
  the portal calls were made on, not to the file descriptor;
- `g_dbus_connection_new_for_address ("unix:fd=N")` does not work, the fd has
  to become a real `GSocketConnection`;
- `G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION` is required, or every call
  fails with `Hello() was not yet called`.

## What it deliberately cannot do

Turn the radio on. `Properties.Set` on `org.neard.Adapter` is refused by the
portal's filter, permanently and by design -- the switch is the user's, in
**Settings ▸ Privacy & Security ▸ NFC**. When the radio is off Tap says so and
points there.

## Permissions, and one caveat about the native build

As a Flatpak, Tap has its own app id, its own grant, and its own row in
Settings ▸ Privacy & Security ▸ NFC ▸ Permitted Apps.

As a native package on a `xdg-desktop-portal` built `-Dsystemd=disabled`
(Alpine's stock default), every unsandboxed caller collapses to the same blank
app id: Tap still works and still gets a grant, but it shares that grant with
every other host process and gets **no row** in Permitted Apps. Tap says this
in its own permission screen rather than claiming a permission model it does
not have there. The fix is a portal rebuild with `-Dsystemd=enabled`, which is
a packaging change and not a design one.

</details>
