# GamePad Drift Fix (Aroma plugin)

An Aroma / Wii U Plugin System (WUPS) plugin that compensates for left-stick drift on the original Wii U GamePad.

It hooks `VPADRead`, calls the original function, and applies a configurable radial deadzone to the left stick before the game sees it.

> **Status: NOT tested on real hardware.** The filter logic is covered by host unit tests and the plugin compiles in CI. Nothing here has been verified on a Wii U, and compatibility with Breath of the Wild or any other game is **unconfirmed** until someone tests it.

> **Compensation is OFF after a fresh install.** Enable it in the config menu after measuring your stick (see below).

## Requirements

- **Aroma with a recent plugin loader.** The plugin is built with WiiUPluginSystem **0.9.1**. WiiUPluginLoaderBackend accepts WUPS 0.9.1 plugins starting with its commit of 2026-04-19 ("Implement support for WUPS 0.9.1").
- **Older Aroma installs will refuse to load it** ("Unsupported WUPS version"). Update Aroma first, for example with the Aroma Updater.
- I couldn't check which Aroma release bundle first ships that backend from the build environment. If the plugin doesn't show up in the plugin list, an outdated backend is the most likely cause.

## Installation

1. Download `GamePadDriftFix.wps` from the build artifacts (Actions tab → latest run → *Artifacts*).
2. Copy it to `sd:/wiiu/environments/aroma/plugins/`.
3. Boot Aroma. Open the plugin config menu with **L + D-pad Down + Minus** on the GamePad.

`GamePadDriftFix_debug.wps` is the same plugin with UDP logging enabled. Install **only one** of the two files.

## Configuration

| Setting | Range | Default | Meaning |
|---|---|---|---|
| Enable drift compensation | on/off | **off** | The main switch. When off, all input is passed through unchanged. |
| Inner deadzone | 0–50 % | 10 % | Stick vectors at or below this radius are reported as centered. |
| Outer limit | 50–100 % | 100 % | Raw radius that maps to full deflection. Lower it if your stick can't reach the edge. |
| Sensitivity | 50–200 % | 100 % | Gain applied after rescaling. The output is always clamped to the unit circle. |
| Apply center offset | on/off | off | Subtracts the X/Y offset from the raw stick before the deadzone. |
| Offset X / Y | -25…25 (×0.01) | 0 | Your stick's resting position. |
| Toggle combo | rebindable | ZL + ZR + L-stick click, held 1 s | Turns compensation on/off mid-game. |

The menu shows the raw left-stick position captured when the menu opened. With the stick released, this is your current drift, and it's a starting point for the deadzone or offset.

### How the filter works

1. **Center-offset correction** (optional): `v = raw - offset`.
2. **Radial deadzone:** if `|v| <= deadzone`, the output is (0, 0).
3. **Rescale:** `n = (|v| - deadzone) / (outer - deadzone)`, clamped to [0, 1]. Multiply by sensitivity and clamp to ≤ 1 again. The output is `v / |v| * n`. Direction is preserved, and there's no jump at the deadzone edge.

### Emulated d-pad directions (`VPAD_STICK_L_EMULATION_*`)

The system also turns the left stick into four digital "emulated direction" bits, which some games use for menu navigation. Drift can hold one of those bits down. To keep the digital and analog directions consistent:

- **Clear only, never add.** A system-reported direction bit is only kept if the *filtered* stick has at least 0.25 magnitude and points within 67.5° of that direction (8-way, so diagonals keep both bits). The plugin never sets a bit the system didn't set, because the system's own thresholds aren't documented.
- **Edges are recomputed.** `trigger` and `release` for those four bits are recomputed from what the game was actually given. Example: if drift held LEFT and was suppressed, pushing left for real produces a proper press edge.
- **LOOSE and TIGHT modes are both handled.**
  - In `VPAD_BUTTON_PROC_MODE_LOOSE` (the default), all returned samples share one button state, decided from the newest sample.
  - In `TIGHT` mode, edges are chained sample by sample, from oldest to newest.
- **All other buttons** and every other `VPADStatus` field (right stick, gyro, accelerometer, touch, volume and so on) are returned exactly as the system reported them.

## Measuring your drift (debug build)

1. Install `GamePadDriftFix_debug.wps`.
2. On a PC on the same network, listen on UDP port 4405, e.g. `nc -ul 4405` (or `udplogserver`). If the LoggingModule is installed, logs go through it instead.
3. Start a game, leave the left stick untouched, and watch the `raw x/y/r` values printed every 30 reads.
4. Set the deadzone a little above the largest resting `r` you see. Alternatively, enable the center offset with the resting x/y and use a smaller deadzone.

## Persistence

Settings are stored with the WUPS storage API under `gamepad_drift_fix`.

- **Config menu changes** are saved when the menu closes.
- **The mid-game toggle combo** flips the setting immediately but doesn't write to the SD card from inside the input path. The toggle is saved at the next of these safe points:
  - opening the HOME menu
  - the game asking to exit or ending
  - opening or closing the plugin config menu
  - the plugin being unloaded
- **Known gap:** if you toggle with the combo and then hard-power-off the console (holding Power) before any of those events, the toggle may not be saved.

## Thread safety

- **The `VPADRead` hook never blocks.** Settings are read through a seqlock with a bounded number of attempts (4). If a writer is interrupted mid-update, the hook keeps using that channel's last consistent snapshot instead of spinning.
- **Per-channel button history** is protected by an atomic try-acquire flag. A concurrent or re-entrant `VPADRead` on the same channel falls back to a stateless filter instead of waiting.
- **Writers** (config menu, toggle flush, init) are serialized by an `OSMutex` that the input path never takes.
- **Button history is reset** on application start and end, on return from the HOME menu, and when the config menu closes.

## Limitations (including Breath of the Wild)

All of these follow from how the plugin works. **None has been confirmed on hardware**, and none should be read as a claim that BotW works with the plugin.

- **GamePad only.** Pro Controller, Wii Remote and Classic Controller input is read through WPAD/KPAD and isn't affected.
- **Deadzones stack.** BotW applies its own deadzone and response curve on top of this one. Set the plugin's deadzone just above your measured drift; a large deadzone eats small, intentional movements (slow walking, fine aiming).
- **Severe drift needs a hardware fix.** Drift bigger than about 20–30 % of the stick's range can't be hidden without losing a lot of fine control.
- **Filtering happens after any VPAD clamp settings** the game configures itself.
- **Emulated directions with offset correction on:** the system derives the direction bits from the *raw* stick, so a menu direction may need a slightly bigger push.
- **TIGHT mode is unverified** for the case where VPADRead returns samples that overlap a previous call.
- **The default toggle combo uses BotW buttons** (ZL target lock, ZR, L-stick click crouch). Needing a 1-second hold makes accidental toggles unlikely but possible. Rebind it if needed. There's no on-screen notification when it toggles.
- **Other plugins that also hook `VPADRead`** (remappers, input displays and so on) run in an order this plugin doesn't control. Those interactions are untested.
- **Untested assumption:** a positive Y on the left stick corresponds to the UP emulation bit (the same convention as the emulated-direction names). The debug build logs both, so you can verify it.

## Hardware test checklist

Please report results.

1. Install the debug build. Confirm the plugin appears in the config menu and that compensation is **off** by default.
2. With compensation off, check that BotW controls behave exactly as without the plugin.
3. Measure the resting stick from the log or the menu readout, then set the deadzone and enable compensation.
4. In BotW, check:
   - Link stays still with the stick released.
   - Slow walking at small deflections still works.
   - Running and the camera behave normally.
   - Menu and inventory navigation with the left stick works, including the first push after the stick has been resting.
5. Check that the toggle combo and the config switch both turn compensation on and off.
6. Toggle with the combo, open the HOME menu, reboot, and confirm the toggle state persisted. Change a setting in the config menu, reboot, and confirm it persisted.
7. In the log, confirm that the `L-emu` bits match the sign of the stick values.

## Building

GitHub Actions (`.github/workflows/build.yml`) runs the host unit tests and then builds both `.wps` files with the official `ghcr.io/wiiu-env/devkitppc` and `wiiupluginsystem` images. It also checks that `.wups.meta` declares `wups 0.9.1`.

The pinned images are `ghcr.io/wiiu-env/devkitppc:20260504` and `ghcr.io/wiiu-env/wiiupluginsystem:20260503` (WUPS 0.9.1). The wut inside that devkitppc image is older than wut's 2026-06-01 VPAD header update. The code avoids the `VPADButtonProcMode` enum names that update added, so it compiles with either header version.

To build locally:

```sh
make -C tests test                              # host unit tests (gcc, ASan/UBSan)
docker build . -t gamepad-drift-builder
docker run --rm -v "$PWD:/project" gamepad-drift-builder make
docker run --rm -v "$PWD:/project" gamepad-drift-builder make clean
docker run --rm -v "$PWD:/project" gamepad-drift-builder make DEBUG=1 TARGET=GamePadDriftFix_debug
```

## Credits

- The VPADRead hook pattern is based on Maschell's [WUPSPluginPlayground](https://github.com/Maschell/WUPSPluginPlayground) `vpad_input_logger`.
- The build setup and logger come from the [WiiUPluginSystem](https://github.com/wiiu-env/WiiUPluginSystem) `example_plugin_cpp`.
- Uses [wut](https://github.com/devkitPro/wut).

License: GPLv3 (as declared in the plugin metadata).
