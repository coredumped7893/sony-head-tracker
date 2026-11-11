# Linux support

Sony Head Tracker runs on Linux as a command-line bridge. It reads the headset's
Android Head Tracker HID sensor through the kernel `hidraw` interface and reuses
the same platform-independent descriptor parser, orientation filter,
recentering, axis mapping, and UDP serializers as the Windows and macOS builds.

This is an unofficial project and is not affiliated with or endorsed by Sony.
Linux has a command-line bridge only; there is no GUI yet. Only the Sony ULT WEAR
(WH-ULT900N) has been tested. XM5, XM6, and other models should work if they
expose the same protocol, but that is unconfirmed on Linux.

## Requirements

- CMake 3.25 or later and a C++20 compiler (verified with GCC 15.2 and Clang
  21);
- `libsystemd-dev` (`systemd-devel` on Fedora) and `pkg-config`;
- glibc 2.36 or later (the build uses `arc4random_buf`);
- BlueZ on a systemd-based distribution;
- a paired compatible headset with current firmware.

## Build

```bash
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

The CLI executable is:

```text
build/linux/sony-head-tracker-linux
```

## Pairing (Classic-only)

The headset must be bonded over Bluetooth Classic. If it was first paired over
Bluetooth LE, BlueZ rejects the HID connection and no hidraw node appears.

Symptom: `probe` finds nothing, and `journalctl -u bluetooth` shows:

```text
hidp_add_connection() Rejected connection from !bonded device
```

Fix, in this order:

1. Back up the BlueZ configuration:
   `sudo cp /etc/bluetooth/main.conf /etc/bluetooth/main.conf.bak`
2. In `/etc/bluetooth/main.conf`, under `[General]`, set
   `ControllerMode = bredr`.
3. Restart Bluetooth: `sudo systemctl restart bluetooth`
4. Remove the headset, then pair, trust, and connect it again, using your
   headset's address in place of the placeholder:

   ```bash
   bluetoothctl remove XX:XX:XX:XX:XX:XX
   bluetoothctl scan on      # put the headset in pairing mode
   bluetoothctl pair XX:XX:XX:XX:XX:XX
   bluetoothctl trust XX:XX:XX:XX:XX:XX
   bluetoothctl connect XX:XX:XX:XX:XX:XX
   ```

5. Restore the original `ControllerMode` (for example from
   `main.conf.bak`) and restart Bluetooth again.

The Classic bond survives the revert and later power cycles. While
`ControllerMode = bredr` is active, Bluetooth LE devices are unavailable, so keep
that window short.

Sony Head Tracker never changes the BlueZ configuration itself.

## Permissions

By default hidraw nodes belong to root. The rule
[`linux/70-sony-head-tracker.rules`](../linux/70-sony-head-tracker.rules) tags
hidraw nodes on the Bluetooth bus (`0005`) with Sony's vendor ID (`054C`) with
`uaccess`, which gives the user logged in at the local seat read access. USB Sony
devices are not matched.

Install it:

```bash
sudo cp linux/70-sony-head-tracker.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=hidraw
```

Then disconnect and reconnect the headset. To check, find the node with
`sony-head-tracker-linux probe` (the `HID` line shows `/dev/hidrawN`) and run:

```bash
getfacl /dev/hidrawN
```

The output should include a `user:<you>:rw-` entry. `uaccess` applies to the
active local session, not to SSH logins.

Device discovery stays descriptor-based, as on the other platforms: the rule only
controls access and does not select the device.

## Commands

```text
sony-head-tracker-linux probe
sony-head-tracker-linux dump [--seconds N]
sony-head-tracker-linux bridge [--port 4242] [--seconds N]
                                [--axis-map YXZ]
                                [--invert XZ]
                                [--smoothing 0.18]
sony-head-tracker-linux help | version
```

`probe` lists the hidraw candidates and verifies the Android Head Tracker
descriptor. Exit codes: `0` verified tracker found, `2` no verified tracker, `77`
a candidate exists but access was denied.

`dump` prints raw reports and decoded samples for hardware diagnosis. It exits
`2` or `77` like `probe`, and `3` if the tracker could not be configured.
`--seconds 0` or no option runs until Ctrl+C.

`bridge` streams full-rate data to OpenTrack on UDP `127.0.0.1:4242` and version
2 JSON on UDP `127.0.0.1:4243` by default. With `--port N`, JSON uses `N + 1`.
The binary OpenTrack packet is the same six little-endian doubles
`(x, y, z, yaw, pitch, roll)` as on the other platforms, with translation set to
zero. The default axis mapping and smoothing are the same as on Windows and
macOS. Bridge exit codes: `0` normal stop, `1` invalid option, `3` no tracker
found at start, `4` UDP output could not be opened, `77` access denied at start.

While the bridge runs:

- recenter with `kill -USR1 <pid>` (the PID is printed at startup);
- stop with Ctrl+C or `SIGTERM`.

## Recovery

The bridge keeps running when the headset disappears or stops sending samples:

- Reconnects back off for 1, 2, 5, 10, then 30 seconds, and stay at 30 seconds.
- If no valid sample arrives within 5 seconds of configuring the tracker, the
  bridge reopens the hidraw node once. If there is still no sample, it closes the
  node and falls back to the reconnect backoff.
- When the tracker is absent and a headset has been verified before, the bridge
  asks BlueZ to recover that same paired headset. The first attempt asks it to
  connect. If BlueZ reports it connected but the hidraw node is still absent, the
  next attempt disconnects and reconnects it once. A powered-off headset keeps
  retrying the first step.

Recovery does not run when a matching hidraw node exists but access is denied:
reconnecting would not help, so install the udev rule instead (see
[Permissions](#permissions)).

After a headset is verified, its Bluetooth address is stored in:

```text
$XDG_CONFIG_HOME/sony-head-tracker/last-tracker
```

`$XDG_CONFIG_HOME` must be an absolute path; otherwise (or when unset) the file is
`~/.config/sony-head-tracker/last-tracker`. The file has mode `0600` in a `0700`
directory. The address is used only to recover that same paired headset. The code
does not scan for new devices, pair or forget devices, or change BlueZ settings,
and the recovery log lines do not contain the address. Delete the file to forget
the headset.

A BlueZ connect request has a 10 second timeout, so Ctrl+C can take up to about
15 seconds to take effect while one is in progress.

## Troubleshooting

### Access denied (exit 77)

`probe` or `bridge` found the tracker but could not open it. Install the udev rule
as described under [Permissions](#permissions), then reconnect the headset.

### No verified tracker is found (exit 2 or 3)

1. Confirm the headset is connected: `bluetoothctl info XX:XX:XX:XX:XX:XX`.
2. Run `sony-head-tracker-linux probe`.
3. If no hidraw node exists, check `journalctl -u bluetooth` for the
   `Rejected connection from !bonded device` message and follow
   [Pairing](#pairing-classic-only).
4. Update the headset firmware using Sony Sound Connect, and disconnect phones or
   other multipoint hosts that may own the sensor.

### The tracker is configured but no samples arrive

The headset streams sensor data only while its Bluetooth audio sink is running. With
no audio playing, the bridge configures the tracker and then loops through
"No valid sample within 5 s", reopen, and reconnect backoff without saying why.
Play audio while tracking; silence is enough. To keep the sink running without
sound, start this in another terminal (stop it with Ctrl+C):

```bash
paplay --raw /dev/zero
```

On PipeWire without the PulseAudio tools, use `pw-play --raw /dev/zero`. The
command plays to the default output, which must be the headset: select it in your
sound settings, or pass `--device=<sink>` to `paplay` (names are listed by
`pactl list short sinks`). It sends only zero samples and changes no settings.
Samples stop again when the audio stops.

### The kernel's `hid-sensor-custom` driver binds the sensor

Seeing `hid-sensor-custom` attached to the headset's HID node is expected. No
action is needed.

### OpenTrack or JSON receives nothing

Only one process can bind a given local UDP receiver port. Start the bridge first,
then configure OpenTrack for UDP input port `4242`. A JSON receiver must listen on
`127.0.0.1:4243`.

## Scope

This release covers the command-line bridge only. Hardware validation so far is a
Sony ULT WEAR on Linux. Reports for XM5, XM6, and other descriptor-compatible
models are welcome; include the model, firmware, distribution and kernel version,
and redacted `probe` output.
