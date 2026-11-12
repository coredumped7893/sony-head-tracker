# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Bridge that reads the Android Head Tracker HID sensor in Sony headsets and streams yaw/pitch/roll over UDP (OpenTrack on `4242`, JSON on port+1). C++20, zero third-party dependencies. Domain terms are in `GLOSSARY.md`; use them.

## Commands

Linux / macOS (CMake 3.25+):

```bash
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Debug   # build/macos on macOS
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

- Single ctest target: `ctest --test-dir build/linux -R linux` (targets: `core`, `linux`, `c-api` on macOS).
- Single test case: there is no filter. Run the binary and grep, e.g. `build/linux/sony_head_tracker_linux_tests | grep <name>`.
- Sanitizers: `-DSHT_ENABLE_SANITIZERS=ON`. CI also uses `-DSHT_WARNINGS_AS_ERRORS=ON` (Clang `-Wall -Wextra -Wpedantic`).
- Linux needs `libsystemd` 246+ via pkg-config and glibc 2.36+.

Windows (MSVC, no CMake): `build.cmd` builds `sony-head-tracker.exe`, `build-tests.cmd` builds and runs core tests. Must be warning-clean at `/W4`.

macOS app: Xcode project is generated from `macos/project.yml` — run `(cd macos && xcodegen generate)` after editing it. `./script/build_and_run.sh` builds and runs.

## Architecture

**Pure core vs platform layer.** `sony_head_tracker_core` (`math`, `orientation`, `protocol`, `hid_descriptor`, `hid_report_descriptor`, `app_config`, `diagnostics`) has no OS headers and is unit-tested without hardware. Every backend produces a `MotionSample` (`types.hpp`) and feeds it to `OrientationFilter`, then the UDP serializers in `protocol`. Keep hardware access in the platform layer; push anything testable into the core or a pure header.

**One header, three implementations.** `hid_backend.hpp`, `logger.hpp`, `output_udp.hpp` declare classes with a pimpl `Context`. Implementations:
- Windows: top-level `src/*.cpp` (`hid_backend`, `sensor_api_backend`, `bluetooth`, `gui`, `main`), all pulling OS headers through `windows_prelude.hpp`.
- macOS: `src/macos/` (IOHID, IOBluetooth) plus the Swift app in `macos/App` via the C bridge `macos/Bridge/sony_head_tracker_c.h`.
- Linux: `src/linux/` (hidraw, BlueZ over sd-bus). CLI only, no GUI.

**Pure decision headers.** `macos_support.hpp` and `linux_support.hpp` are header-only, OS-free state machines (recovery stage, stream-stall action, address normalization, config path). Platform code calls them; tests cover them. `linux_support.hpp` reuses parts of `macos_support.hpp`.

**Device verification.** A device is used only if its top-level collection is sensor page `0x20` / usage `0xE1` and the sensor description contains `#AndroidHeadTracker#`. Never match by model or VID/PID.

**Build-file gotchas.**
- `build.cmd` compiles `src\*.cpp` (top level only), so a new top-level `src/` file lands in the Windows exe automatically. Put non-Windows platform code in `src/macos/` or `src/linux/`.
- `build-tests.cmd` lists core sources and test files explicitly. Adding a core `.cpp` or core test means updating both `CMakeLists.txt` and `build-tests.cmd`.
- Release tags must match `include/sony_head_tracker/version.h`; CI fails otherwise.

**Tests** use the in-repo `tests/test_framework.hpp` (`TEST`, `CHECK`, `CHECK_NEAR`; self-registering). `tests/ult_wear_descriptor.hpp` is a real descriptor fixture.

## Rules

- Never spoof devices, change Bluetooth identities, or modify firmware. Recovery only reconnects the last verified address through public OS APIs (Windows: inbox drivers only).
- Never print or log Bluetooth addresses or BlueZ object paths.
- Changing UDP/JSON output: update `docs/PROTOCOL.md`, bump the JSON `version` if not backward-compatible.
- Add a line under **Unreleased** in `CHANGELOG.md` for notable changes.
- Platform docs: `docs/LINUX.md`, `docs/MACOS.md`.
