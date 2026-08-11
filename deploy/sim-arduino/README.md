# Aster Arduino / ESP32 firmware simulation

Cross-compiles `aster::embedded::Db` (Tiny / bare-metal) into an **ESP32
Arduino** firmware with PlatformIO, then boots the image under **Espressif
QEMU** and asserts serial output `ASTER_OK`.

Classic AVR Uno is intentionally out of scope (too little RAM for float
vectors). ESP32 matches the PlatformIO / Arduino-ESP32 path documented for
`//aster/embedded`.

## Commands

```bash
make sim-arduino          # build ESP32 firmware + QEMU expect ASTER_OK
make sim-arduino-native   # host smoke of the same harness (no MCU)
make sim-arduino-build    # firmware only
make sim-arduino-clean    # remove .pio build + flash/log cache
```

Or directly:

```bash
./deploy/sim-arduino/run.sh           # all
./deploy/sim-arduino/run.sh native
./deploy/sim-arduino/run.sh build
./deploy/sim-arduino/run.sh qemu
./deploy/sim-arduino/run.sh clean
```

## Prerequisites

- Python 3 (creates `deploy/sim-arduino/.venv` with PlatformIO + esptool)
- Network once for the pioarduino ESP32 platform / Espressif QEMU download
- **macOS:** Homebrew libs for the Espressif QEMU binary:

  ```bash
  brew install libgcrypt sdl2 glib gettext pixman jpeg libpng snappy libslirp vde libssh
  ```

## What the firmware checks

Mirrors [`aster/embedded/embedded_test.cc`](../../aster/embedded/embedded_test.cc)
at Tiny scale (dim 8, auto-flush, tag search, compact):

- dimension mismatch rejected
- upsert → auto-flush → search → compact → get
- prints `ASTER_OK` or `ASTER_FAIL:<reason>` on UART0

## Layout

| Path | Role |
| --- | --- |
| `platformio.ini` | ESP32 (pioarduino / GCC 13) + `native` host env |
| `extra_script.py` | Compiles monorepo Tiny sources into the firmware |
| `include/aster` | Symlink to repo `aster/` (avoids `-I` repo root clobbering `<version>`) |
| `src/smoke.h` | Shared harness |
| `src/main.cpp` | Arduino sketch |
| `src/native_main.cpp` | Host entrypoint |
| `run.sh` | Build, merge 4 MiB flash, QEMU serial poll |

## Notes

- Stock PlatformIO `espressif32` ships GCC 8 (no C++20 `std::span`). This
  suite pins [pioarduino](https://github.com/pioarduino/platform-espressif32)
  so Aster headers compile unchanged.
- QEMU flash image is padded to **4 MiB** (Espressif QEMU requirement).
- QEMU is started with `-nic none` and the TIMG watchdog disabled; openeth +
  flash races can otherwise Guru-Meditation under QEMU. Panics still retry
  (default 3 attempts).
- This is separate from [`../sim-grafana`](../sim-grafana) (kind cluster of
  `aster serve` nodes).
