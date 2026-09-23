Didn't have my Tigard handy, so I used an LLM to modify PICO_MPSSE to emulate more of the FT2232H so that iceprog could work. No warranties, YMMV - but it worked for me in a pinch. I still recommend [Tigard](https://www.crowdsupply.com/securinghw/tigard).<br>
-Ian

# pico-iceprog

Use a Raspberry Pi Pico 1 (RP2040) as an SPI programmer with unmodified
[iceprog](https://github.com/YosysHQ/icestorm/tree/main/iceprog). Based on
[MiSTle-Dev/PICO-MPSSE](https://github.com/MiSTle-Dev/PICO-MPSSE).

The Pico appears as an FT2232 with two interfaces, A and B. It handles the
SPI and GPIO commands; iceprog handles flash detection, erase, programming
and verification. Direct FPGA SRAM programming is also supported.

## Build and install

Requires the Pico C/C++ SDK, an ARM GCC toolchain, CMake, and Make or Ninja.
Tested with Pico SDK 2.2.0 and GCC 14.2.1.

```sh
cmake -S pico_mpsse -B build \
  -DPICO_BOARD=pico \
  -DPICO_SDK_PATH=/path/to/pico-sdk \
  -DPICO_TOOLCHAIN_PATH=/path/to/arm-toolchain \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

`PICO_TOOLCHAIN_PATH` points to the directory containing `bin/`. Omit it if
the compiler is already on `PATH`.

Hold BOOTSEL while plugging in the Pico, then copy `build/pico_iceprog.uf2`
to the `RPI-RP2` drive. It will reconnect as **Pico MPSSE**, USB ID `0403:6010`.
Use the same USB driver and permissions setup as for an FT2232 programmer.

## Wiring

Choose either interface and use its matching `-I A` or `-I B` option.
The table uses **physical header pin numbers** on the Pico 1.

| Signal | Interface A pin | Interface B pin |
| --- | ---: | ---: |
| SCK | 7 | 25 |
| MOSI / COPI (Pico output) | 4 | 21 |
| MISO / CIPO (Pico input) | 6 | 24 |
| CS, active low | 9 | 26 |
| CDONE (Pico input) | 11 | 29 |
| CRESET, active low | 12 | 31 |
| Ground | 8 | 28 |

Any Pico GND pin can be used. Power the Pico over USB and the target from
its normal supply, with their grounds connected. Signals are **3.3 V**;
the Pico GPIO pins are not 5 V tolerant.

CS and CRESET are pulled low when asserted and released to high impedance.
The target must provide pull-ups for those signals and CDONE. For direct
SRAM programming, follow the target's configuration-input wiring; it may
differ from the flash connections.

Pin assignments are in [config.h](pico_mpsse/config.h). See also the
[official Pico pinout](https://datasheets.raspberrypi.com/pico/Pico-R3-A4-Pinout.pdf).

## Testing

Hardware testing on interface B at the default 6 MHz has covered flash
programming, bulk erase, readback verification, and FPGA configuration
confirmed by CDONE. Interface A and direct SRAM programming have software
test coverage but have not yet been tested on hardware here.

To run the software tests:

```sh
python3 tests/run.py
```

The suite runs the firmware against a simulated USB/PIO/flash environment,
with unmodified upstream iceprog and ASan/UBSan. It passes 113 iceprog
invocations covering both interfaces, both clock rates, flash and SRAM
operations, and USB packet handling. The script downloads three iceprog
source files at a pinned revision into `build/`. To use a local checkout:

```sh
python3 tests/run.py --iceprog-source /path/to/icestorm/iceprog
```

Debug output is available on UART0, GP0 TX / GP1 RX, at 115200 bit/s.
The retained [upstream notes](pico_mpsse/README.md) describe the original
JTAG support; that use has not been retested in this fork. The
[USB trace parser](usbmon/README.md) is also retained.
