# ESP32-S3 N16R8 + 2.8" ST7789 SPI display

DIY target for an ESP32-S3-WROOM-1 **N16R8** module (16MB quad SPI flash, 8MB octal PSRAM)
paired with a 2.8 inch 320x240 TFT SPI panel using an **ST7789(V)** controller.

It is a sibling of the `esp32-s3-devkit` target: same pinout, but the display init sequence is a
proper ST7789 sequence instead of the ILI9341 one.

## Toolchain

This branch tracks retro-go `dev`, which requires **ESP-IDF 5.1 - 5.5**.

If ESP-IDF was installed with `eim`, note that it only provides `idf.py` as a *shell alias*, which
`rg_tool.py` cannot invoke as a subprocess. Put the real scripts on `PATH`:

```
source ~/.espressif/tools/activate_idf_v5.5.2.sh
export PATH="$IDF_PATH/tools:$IDF_PATH/components/partition_table:$PATH"
```

## Building

```
python rg_tool.py --target esp32-s3-n16r8 build-img
```

With the GBA dynarec (see below):

```
GBAJIT=1 python rg_tool.py --target esp32-s3-n16r8 build-img
```

## Flashing

```
python rg_tool.py --target esp32-s3-n16r8 --port /dev/ttyACM0 install
```

or

```
esptool.py write_flash --flash_size detect 0x0 retro-go_*_esp32-s3-n16r8.img
```

## Pinout

| Function     | GPIO |
| ------------ | ---- |
| LCD MOSI     | 12   |
| LCD SCLK     | 48   |
| LCD DC       | 47   |
| LCD RST      | 3    |
| LCD BACKLIGHT| 39   |
| LCD CS       | 45   |
| SD MISO      | 9    |
| SD MOSI      | 11   |
| SD SCLK      | 13   |
| SD CS        | 10   |
| I2S BCK      | 41   |
| I2S WS       | 42   |
| I2S DATA     | 40   |
| Status LED   | 38   |
| A / B        | 15 / 5   |
| X / Y        | 21 / 14  |
| L / R        | 1 / 2    |
| SELECT / START | 16 / 17 |
| MENU / OPTION  | 18 / 8  |
| D-Pad (ADC)  | ADC1 CH5 (up/down), ADC1 CH6 (left/right) |
| Battery      | ADC1 CH3 |

Buttons are active low with internal pull-ups.

X/Y/L/R are only read by `snes9x` (all four) and `gbsp`/GBA (L and R only). Every other app
uses the 10 button Odroid-GO layout and ignores them, so they may be left unpopulated if you
do not care about SNES or GBA shoulder buttons.

## Display troubleshooting

The driver emits `COLMOD` and `MADCTL` itself, from `RG_SCREEN_ROTATION` and `RG_SCREEN_RGB_BGR`.
`RG_SCREEN_INIT()` only carries the ST7789 specific power/gamma tuning.

- Image mirrored, rotated or upside down: change `RG_SCREEN_ROTATION`. Valid values are 0-7; just
  try them all. It currently is `3` (MV|MX, landscape).
- Red and blue swapped: flip `RG_SCREEN_RGB_BGR` between 0 and 1.
- Colors look inverted: remove the `ILI9341_CMD(0x21)` (INVON) line from `RG_SCREEN_INIT()`. Most
  ST7789 panels need it, a few do not.
- Image shifted by a few pixels: adjust `RG_SCREEN_VISIBLE_AREA` (left, top, right, bottom).
- Glitches at high SPI speed: lower `RG_SCREEN_SPEED` (or raise it to `SPI_MASTER_FREQ_80M` if your
  wiring is short and clean).

## GBA (gbsp)

`gbsp` is a port of gpSP. With gpSP's plain ARM interpreter it runs at roughly 30-50% speed on this
chip, which is not playable. This branch therefore uses the **Xtensa LX7 dynarec backend** from
[pjcau/esp32-emu-turbo](https://github.com/pjcau/esp32-emu-turbo), enabled with `GBAJIT=1`.

Relevant build flags (all off unless set):

| Flag | Effect |
| ---- | ------ |
| `GBAJIT=1` | Use the dynarec instead of the interpreter |
| `GBAPROF=1` | Print a per-second `GBAPROF` line with ms/frame per stage |
| `GBABENCH=1` | Scripted deterministic benchmark, `GBABENCH` line every 300 frames |
| `GBAJIT_IRAM=1` | Put the translation cache in internal RAM. **Does not work on this board** (see below) |

### Measured performance

With `GBAJIT=1` and the 32KB instruction cache, Metroid: Zero Mission runs at **58 fps** in simple
scenes (cpu 6.4 ms, render 6.0 ms) and drops to **46 fps** in demanding ones (cpu 20.2 ms,
render 14.0 ms). The bottleneck is ARM emulation on core 0 plus the scanline renderer on core 1.
Display (0.05 ms) and audio submission (0.06 ms) are negligible, so neither faster SPI nor a
different display driver would help.

### The GBA audio artifacts in heavy scenes

`rg_audio_submit()` is what paces the gbsp emulation loop: it blocks until the I2S buffer has
room, so submitting one frame's worth of samples yields 60 fps. Instrumenting every region of the
loop shows wall time splitting cleanly:

| Scene | cpu | render | submit blocking | fps |
| ----- | --- | ------ | --------------- | --- |
| calm      |  9-11 ms |  9-10 ms | 5-6 ms | 57-58 |
| demanding | 20-28 ms | 13-24 ms | 0.2 ms | 33-46 |

In demanding scenes core 0's ARM emulation alone exceeds the 16.7 ms budget, so the GBA genuinely
runs in slow motion and produces audio slower than the DAC consumes it. The buffer drains and the
gaps are heard as distortion. It is not clipping: it persists at 15% volume.

**Three fixes were tried and all failed. Do not repeat them:**

- *Larger DMA buffers* (`DMA_BUFFER_COUNT` 4 -> 8): no audible change. The shortfall is sustained,
  not transient, so more buffering only delays the underrun.
- *Adaptive DAC sample rate* (`rg_audio_set_sample_rate` tracking measured output): death spiral.
  Lowering the rate makes `rg_audio_submit` block longer, which throttles the emulator, which
  lowers the measured rate further. It pinned every scene to ~35 fps.
- *Stretching the samples to fill the elapsed time*: the same feedback loop. Feeding more samples
  than the game produces makes submit block longer and throttles the emulator, badly.

The common trap is that **anything which changes how much audio is submitted, or how fast it
drains, also changes the frame pacing**, because submit is the pacer. A correct fix would need
negative feedback on the actual DMA fill level, which the `rg_audio` API does not expose. Short of
that, the only real cure is making the emulation faster.

### Why `GBAJIT_IRAM=1` does not work here

It requires `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE=n`, and even then only about 5KB of internal RAM is
free for the translation cache. The dynarec thrashes and then executes garbage:
`bad jump 8000242` followed by a `Guru Meditation Error: IllegalInstruction` with the PC inside
IRAM. Leave this flag off.

### Why the instruction cache setting matters

The dynarec executes translated code out of PSRAM. An instruction fetch that misses the
instruction cache costs roughly 11.6 cycles instead of 1, and that cache is **shared by both
cores**. `sdkconfig` therefore selects `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`, up from the 16KB
default, at a cost of 16KB of internal SRAM. This is the single most effective knob for GBA
performance on this board.

### Local changes to the grafted code

`gbsp/` and `components/xjit/` are taken from `pjcau/retro-go`, whose retro-go base is older than
`dev`. Three call sites in `gbsp/main/main.c` were adapted to the current API:

- `rg_system_init(rate, &handlers, NULL)` -> `rg_system_init(&(rg_config_t){...})`
- `rg_display_sync(false)` -> `!rg_display_is_busy()`
- `rg_display_sync(true)` -> `while (rg_display_is_busy()) rg_task_yield();`
