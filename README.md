# ESP32 Digital Audio Player

A standalone music player for the Freenove ESP32-WROVER CAM board: FLAC and
MP3 from an SD card, a PCM5102A I2S DAC, a 16x2 LCD and a 4x4 keypad.

- Design: `docs/superpowers/specs/2026-09-30-dap-design.md`
- Stage plans: `docs/superpowers/plans/`
- Pin assignments: `main/pins.h`
- The ESP-IDF example this grew from: `reference/`

## Build and flash

```bash
source /opt/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

`sdkconfig` is generated from `sdkconfig.defaults`; change settings there.

## Tests

Hardware-free logic lives in `components/dap_core` and is tested on the host:

```bash
cmake -S test/host -B test/host/build
cmake --build test/host/build
ctest --test-dir test/host/build --output-on-failure
```

## Serial log

```bash
python3 tools/serial_capture.py /dev/ttyUSB0 60
```
