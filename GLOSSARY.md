# Sony Head Tracker

Bridge that turns the head-tracking sensor inside compatible Sony headsets into orientation output for OpenTrack and other consumers.

## Language

**Verified tracker**:
A HID collection with top-level usage `0x20:0xE1` whose sensor description carries the `#AndroidHeadTracker#` marker. The only device the app will configure or stream from.
_Avoid_: supported device, Sony device

**Last verified address**:
The Bluetooth address of the most recent verified tracker. It is persisted so recovery can target that exact headset.
_Avoid_: saved device, paired device

**Bluetooth recovery**:
Asking the OS Bluetooth stack to reconnect the last verified address when its HID collection is absent. Stage 1 connects. Stage 2 forces one disconnect+connect.
_Avoid_: repair, reset

**Stream stall**:
A configured verified tracker that delivers no valid sample within the stall timeout.
_Avoid_: freeze, hang

**Classic-only pairing**:
Pairing the headset over Bluetooth BR/EDR rather than LE. Linux needs this so BlueZ accepts the HID channel.
_Avoid_: legacy pairing
