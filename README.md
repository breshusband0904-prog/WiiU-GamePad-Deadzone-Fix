# GamePad Drift Fix (Aroma plugin) — work in progress

An Aroma / Wii U Plugin System (WUPS) plugin that compensates for left-stick drift on the original Wii U GamePad.

**Current stage: input-logging prototype.** The plugin hooks `VPADRead` and logs the left stick. It does **not** modify any input yet.

**Not tested on real hardware.**

## Building

GitHub Actions builds the plugin on every push (see `.github/workflows/build.yml`). The `.wps` files are uploaded as workflow artifacts.

To build locally with Docker:

```sh
docker build . -t gamepad-drift-builder
docker run --rm -v "$PWD:/project" gamepad-drift-builder make                 # release
docker run --rm -v "$PWD:/project" gamepad-drift-builder make DEBUG=1 TARGET=GamePadDriftFix_debug
```

## Measuring drift with the logging build

1. Copy `GamePadDriftFix_debug.wps` to `sd:/wiiu/environments/aroma/plugins/`.
2. On a PC on the same network, listen for UDP logs on port 4405, e.g. `nc -ul 4405` or `udplogserver`.
3. Start a game and leave the left stick untouched.

The log prints the raw left stick every 30 reads. Every 300 reads it also prints a min/max envelope, which shows the drift range of your stick at rest.
