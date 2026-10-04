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
| `GBAPROF=1` | Print a per-second `GBAPROF` line with ms/frame per stage, ROM page loads, halt share |
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
runs in slow motion and produces audio slower than the DAC consumes it. Three defects turned that
shortfall into distortion, and the last two distort calm scenes as well:

- **Underruns replayed stale audio.** Without `tx_desc_auto_clear` the legacy I2S driver keeps
  cycling its DMA buffers when nothing new arrives, so a drained ring buzzed with old samples
  instead of going quiet. `drivers/audio/i2s.c` now sets it for the external DAC. This is why
  larger DMA buffers alone (4 -> 8) made no audible difference.
- **The last frame of every submit was dropped** by the conversion loop in `drivers/audio/i2s.c`.
  That is one sample per video frame, a faint 60 Hz tick, and the fix applies to every app.
- **Direct-sound channels outran the PSG channels** when a game plays them faster than the 32768 Hz
  mix rate (the m4a engine's top rates are 36-42 kHz). `sound_timer()` masked the FIFO position
  to its fraction instead of carrying it, so every timer tick wrote an output sample. The
  direct-sound write position ran 10-28% ahead and wrapped the 1024-frame (~31 ms) stereo ring
  over audio not yet played. `gbsp-libretro/sound.c` now carries it.

With these fixed, a slow frame still produces less audio than it takes, so the ring drains. The
**Audio stretch** option (gbsp's options menu, on by default) decides what happens then:

- **On**: `gbsp/main/main.c` linearly stretches each frame's samples to the time the frame took to
  emulate. The sound is continuous, but its pitch drops with the frame rate: at 36 fps it plays
  40% slow, about 9 semitones low.
- **Off**: the pitch is exact, and the slowdown is heard as short silences.

Two earlier attempts failed because submit is the pacer. Stretching to the *elapsed* time counts
the time submit spent blocking, so more audio meant slower frames and more audio again. Adapting
the DAC sample rate to the measured output gave the same death spiral and pinned every scene at
~35 fps. Do not repeat either. The current stretch avoids the loop:

- It times only the work, from the return of one submit to the start of the stretch before the
  next (the stretch and the conversion inside submit fall outside it). If it submits
  too much, the ring fills, submit blocks, and that wait does not count, so the excess limits
  itself. It is not free, though: time blocked in submit is time not spent emulating.
- Its target is an exponential average (1/8) of the work time, so the pitch follows the frame rate
  instead of jumping with every frame that is a little faster or slower than the last.
- An eighth of what the DMA ring lacked after the previous submit, below the 1710 frames the
  estimate counts as full (see below), is added on top. The work time misses what runs outside it
  and the average lags a rising load, so without this the ring drains slowly under
  a sustained heavy load until it runs dry. It reads the level after the previous submit, not the
  current one: the current level already reflects this frame's work time, and counting that twice
  makes the pitch wobble when fast and slow frames alternate.
- When the ring is estimated to hold less than one frame (546 samples) and this frame was slower
  than the average, it uses this frame's work time, plus the same eighth, instead of the average.
  This rescue closes the gap after a sudden slowdown. It acts only after a slow frame (over 16.7 ms),
  or when the ring was already a buffer short. A single slow frame in a calm scene has the full
  ring to absorb it. Stretching that frame whole drops its pitch, by more than an octave for a
  frame 25 ms slower than the rest, and fills the ring, which then costs the next frames time
  blocked in submit. In the simulation below, rescuing every slow frame lost 10-26% of the frame
  rate when fast frames alternate with slow ones of 33-45 ms (9/9/33 ms, 9/40 ms, 9/9/45 ms...),
  with a warble in the pitch; this rescue loses 3-6%. With slow frames of 25-30 ms the two did not
  differ.
- The ring is filled with silence (~52 ms with 10 buffers, ~74 ms with 14) at start-up and when
  a menu closes. The menus silence
  the ring and it runs empty, and the restoring force above would otherwise refill it by
  stretching the first frames of the game: an audible dip in pitch. When a menu opens,
  `drivers/audio/i2s.c` finishes the DMA buffer being written with silence, so the fill starts on
  a buffer boundary and leaves exactly the 1710 frames the estimate assumes. Before, the writer
  resumed mid-buffer, and the fill left anywhere within a buffer (5.5 ms) of that.

`rg_audio` does not expose the fill level, so it is estimated from the clock: the level drains at
the sample rate since the last submit and grows by what each submit writes. The estimate is kept
in millionths of a frame: rounded to whole frames at every submit, it drifted upwards, read the
ring as fuller than it was, and the rescue stopped firing. The clock runs at the rate the DAC
plays: the sample rate times the **Speed** option, read again when a menu closes, as only the
menus change it. Assuming 32768 Hz at 2x read the ring as fuller than it was, so the stretch fell
far short (in calm scenes it never acted) and the simulation had a gap in nearly every frame, as
with the stretch off; at 0.5x it read the ring as emptier, overfilled it, and cost a third of the
frame rate in heavy scenes. The mix buffer holds the audio of a 15 fps frame even at 2.5x, the
highest Speed (5462 frames, 21 KB). **Overclock** also moves the PLL that clocks the I2S on the
S3; `rg_system_set_overclock()` now corrects the sample rate for it (see below), so the DAC still
plays at the nominal rate and the clock needs no correction. gbsp builds with 14 DMA buffers
(~77 ms, `RG_AUDIO_DMA_BUFFER_COUNT` in `gbsp/CMakeLists.txt`), up from 10 (~55 ms) to ride out
the retranslation after a flush of the translation cache (see below). The frame counts in this
section (1710, 1620, 1665) and the simulation are for 10 buffers: with N buffers of 180 frames the
estimate counts (N - 1) x 180 + 90 frames as full, 2430 with 14 (~74 ms), and a submit that waited
as at least (N - 1) x 180, 2340.

The clock cannot see two events, so `drivers/audio/i2s.c` now counts them and
`rg_audio_get_counters()` returns them as `fullWaits` and `underruns` (a driver without the
optional `get_counters` hook leaves them at 0, and the clock alone):

- **A submit that waited for room.** The driver first writes without a timeout; a short write
  means every DMA buffer was full. All buffers but the one being written are then full, so the
  ring holds at least 1620 frames, and the estimate is raised to that if it was lower. A clock
  estimate above it is kept, as it is the closer one; 1710 frames (half of the last buffer on top)
  is the most it assumes.
  An earlier version took any submit slower than 1 ms for a wait. A submit held up by another
  task (flash, PSRAM or a mutex) then read as a full ring, which stopped the restoring force.
  Under a heavy load these overestimates piled up until the ring ran dry.
- **An underrun.** With an event queue, the legacy driver posts `I2S_EVENT_TX_Q_OVF` when the DMA
  finishes a buffer and no other one was written since: at the end of the buffer the ring ran dry
  in, up to 5.5 ms late. A gap that ends before that, the next submit having written past that
  buffer, goes unreported. The ring then holds about what the submit just wrote, less what
  played during the submit, within a buffer either way. The DMA has passed the buffer it ran dry
  in. A submit that starts after the report first finishes that buffer with silence, so its audio
  starts at the next buffer boundary, in order, after up to 5.5 ms more silence. Before, the
  submit wrote its first frames into the passed buffer: they were not played, or, if the ring ran
  dry again within one turn (~55 ms with 10 buffers), played then, out of place.

The stretch fills the ring to 1710 frames, the most the estimate assumes. Aiming a quarter buffer
lower, at 1665, stretched a little less: 0.4% more frame rate on average, and a smaller dip at
start-up when frames take just under 16.7 ms (0.06 semitones instead of 0.38). But it leaves less
in reserve for a submit that is held up (see below): 263 gaps instead of 107 for a 3 ms hold-up in
one submit out of 10, 406 instead of 319 for 8 ms in one out of 50, and 21 instead of 3 for
1.5 ms in one out of 10.

The design was checked in a frame-time simulation of this loop. It models the legacy I2S driver's
ring in whole 180-frame buffers, its underrun report at the end of the buffer that ran dry, the
audio it loses after an underrun (the writer finishes the buffer the DMA has passed, and the new
audio starts at the next buffer boundary), the time spent outside the work window, the stretch
itself, and the same loop with the stretch off as the reference frame rate. A gap is one stretch
of silence, however many frames it spans. "The earlier version" below took a submit slower than
1 ms for a full ring and had no underrun report. Over 26 traces of 8000 frames (calm, spiky,
sustained 20-40 ms per frame, slowly rising, random, and fast/slow patterns such as 9/9/45 ms) it
gave:

- no underruns, except in a random trace of 20 ± 8 ms per frame: 15 short gaps, 43 ms of silence
  in about three minutes, against 3650 gaps with the stretch off and 19 gaps (68 ms) with the
  earlier version;
- frame rates on average 2.6% below those of the stretch off, 5.5% at most for a fixed pattern and
  7.7% for the random trace. The earlier version lost 2.1% on average: it counted a ring that made
  submit wait as 1710 frames full, where the driver's count guarantees only 1620, and so stretched
  less;
- no gap after a menu or at start-up. When frames take just under 16.7 ms the pitch dips by up to
  0.16 semitones after a menu and 0.38 at start-up, and not at all in calmer scenes. Without the
  silence fill there was a gap (11-22 ms in calm scenes, up to 42 ms in heavy ones) and, in calm
  scenes, a dip of up to 5.6 semitones;
- for a single frame 25-35 ms slower than the 12-15 ms around it, a dip of at most 0.9-5.5
  semitones, in the frames after it as the average catches up, against 14-19 semitones in that
  frame when rescuing every slow frame;
- one gap at a sudden, lasting drop from full speed to 36, 40 or 45 ms per frame, of 9, 15 and
  26 ms. Rescuing every slow frame reacts to the first one and left none. That is the price of
  ignoring single slow frames.

Submits held up by another task (flash, PSRAM or a mutex) were added at random to nine of the
traces, four times over with different draws. The driver's count of full waits and its underrun
report each remove most of the gaps the earlier version left:

| Hold-up | In one submit out of | Gaps | With the 1 ms check instead of the count | Earlier version |
| ------- | -------------------- | ---- | ---------------------------------------- | --------------- |
| 1.5 ms  | 50                   |    0 |  1109 |  3980 |
| 1.5 ms  | 10                   |    3 |  6050 | 18452 |
| 3 ms    | 500                  |    0 |    68 |   161 |
| 3 ms    | 50                   |    6 |  1738 |  5865 |
| 3 ms    | 10                   |  107 | 10001 | 28795 |
| 8 ms    | 500                  |   23 |   348 |   796 |
| 8 ms    | 50                   |  319 |  3630 | 10853 |
| 20 ms   | 500                  |  167 |   819 |  1705 |
| 20 ms   | 50                   | 2153 |  5880 | 15309 |

Long, frequent hold-ups still leave gaps, and so does any stall longer than the buffering (~74 ms
with 14 buffers), such as a slow SD read. That is a model, not a measurement: confirm it on the board
with `GBAPROF=1`, and by ear with the option on and off.

Stretching hides the slowdown; it does not remove it. Clean audio at the right pitch in demanding
scenes still needs the emulation itself to run faster.

### Running faster: Overclock and Frameskip

Two options raise the frame rate in demanding scenes.

**Overclock** (in-game menu, levels 0-3) raises the PLL
that every PLL-derived clock comes from, by 1/12 per level: 260, 280 and 300 MHz. In the scenes
above the ARM emulation took 20.2 ms per frame, which at 280 MHz is about 17.3 ms and at 300 MHz
about 16.2 ms, inside the 16.7 ms of a 60 Hz frame. The S3 had the option but nothing corrected
for its side effects; `rg_system_set_overclock()` now:

- measures the CPU speed against `esp_timer`, which on the S3 runs from the crystal (SYSTIMER), as
  does the FreeRTOS tick, so timing, frame pacing and the FPS figures stay right without scaling;
- divides the I2S sample rate by the speed-up: the legacy driver always clocks the I2S from
  `PLL_F160M`, which moves with the PLL, so without it every system played sharp (1.4 semitones at
  level 1, 3.9 at level 3). The Speed option goes through the same correction;
- leaves the UART0 console alone: on the S3 it runs from the crystal (on the ESP32, from the APB
  clock, which is why it is retuned there).

It also speeds up everything else on the PLL: PSRAM and flash (MSPI), the LCD SPI and the SD SPI
(20 MHz becomes 25 MHz at level 3). Whether a given chip is stable there is luck; if a level
crashes or the SD card fails, use a lower one. USB-Serial/JTAG takes its 48 MHz from the same PLL
and is likely to drop off the bus while overclocked, so read logs on UART0, which stays readable.

retro-go itself forgets the level at a reset. gbsp saves it (`overclock` in its settings) when a
menu closes and applies it at start-up, once the ROM and the save state are read at the stock
clocks. In case the level made it crash, it does not apply it, and clears it (choose it again in
the menu), when the last overclocked session did not exit cleanly: a panic, a watchdog or a
brown-out reset, or a power loss in about its first minute (until a calm frame after 60 s).
`overclockLive` records the session, as a panic does not show in the reset reason: it reboots
into retro-go's crash dialog, which goes to the launcher. A power loss later on keeps the level,
as that is how a devkit is switched off. Other apps still start at 240 MHz.

**Frameskip** (Options, saved, `Auto` by default) skips drawing the frame after one that took
longer than a frame (1/60 s at Speed 1x), never two in a row. A skipped frame still runs the ARM code, the sound and
the video-memory copy, but core 0 does not wait for core 1 to draw its lines, and core 1 leaves
PSRAM to core 0. `Off` draws every frame. With the lines drawn on core 1, gbsp ignores retro-go's
own frameskip (which it raises when the game runs slow, and the overclock sets).

`GBAPROF=1` adds three figures to its line to choose the next step: `ROM pages loaded` (ROM
pages read from the SD card that second; a ROM larger than the cache, 1 MB blocks in PSRAM,
pages 32 KB at a time, roughly 13 ms per read), the share of GBA cycles the CPU spent halted
waiting for an interrupt, and which native m4a mixer was found (0 none, 1 stereo, 2 mono). The
boot log prints how many cache blocks were allocated.

### The short hitches: translation-cache flushes and core 1

With Overclock on, Metroid Zero Mission still stuttered now and then. Most of it was the dynarec:
its ROM translation cache (2 MB of PSRAM) filled every one to two minutes, translation then
flushed it mid-frame, and the code running had to be translated again: 800-1800 blocks in that
second, which ran 42-51 frames instead of 55. Five changes, measured in the same attract-mode loop at
240 MHz (`GBAPROF=1`, the USB console does not survive an overclock):

- **The translator runs from IRAM.** The Thumb translator, the emitter helpers and the Xtensa
  encoders (`XT_HOT` in `cpu_threaded.c` and `xtensa/`) were fetched from flash, through the
  instruction cache that the translated code also needs. In IRAM a block translates 2.2-2.4x
  faster. It costs 27 KB of internal RAM: 82 KB stays free, the largest block 31 KB.
  `GBAPROF`'s translate time used to add up nested translation once per level (the translator
  recurses into a block's exits); it now counts only the outermost call.
- **The ROM cache gets more of the PSRAM.** The ROM block hash went from 16 to 15 bits (128 KB
  less) and the split from 2048 KB ROM + 384 KB RAM to 2304 KB + 256 KB: same footprint, and the
  early flush below takes back the extra, so the ROM cache is flushed about as often as before,
  but between frames. The RAM cache still holds over six times MZM's RAM code (38 KB at most).
  Do not shrink it much further: the RAM blocks linked by direct branches are translated at
  once, a set larger than the cache cannot run at all (`bad jump`, then a crash), and a game
  with more RAM code flushes it more often. The figures below were measured with a 2432 KB +
  128 KB split, which left too little room for such games.
- **The flush happens between frames, at a calm moment.** Once less than `ROM_FLUSH_SOFT` (256 KB)
  is left, `gbsp/main/main.c` flushes the ROM cache before a frame, with no translated code
  running, after a frame where the audio ring was nearly full and the work time averaged under
  a frame, so the ring has the most lead to ride out the retranslation. If no such frame comes,
  it flushes at `ROM_FLUSH_URGENT` (128 KB left), and if translation still reaches the end it
  flushes mid-frame as before. Closing a menu also flushes a cache that is that full. The
  `GBAPROF` translate line counts flushes and the mid-frame ones.
- **The line renderer runs above the display task on core 1** (`GBSP_RENDER_PRIO` 7, was 5 under
  the display's 6). Below it, the renderer stalled while the display scaled and sent a frame, and
  core 0 waited for it whenever the game wrote video memory mid-frame (DMA to BG VRAM in MZM).
  The display only shows fewer of the frames drawn instead.
- **The audio ring has 14 DMA buffers** instead of 10 (~74 ms of lead instead of ~52).

In the busiest stretch of the loop (32 s, 240 MHz), core 0's wait for core 1 fell from 1.70 to
0.49 ms per frame, the work from 17.1 to 16.0 ms per frame, and the frame rate rose from 52 to
56 fps, with more frames drawn (31 against 27 per second). Over the whole 263 s capture the wait
fell from 2.38 to 0.86 ms per frame and the frame rate rose from 54.2 to 56.8 fps (45 frames drawn against
38), and both of its two flushes came between frames. The one in the busy stretch ran
52 frames in that second, as in the seconds before, and 45 in the next, which
retranslated 1577 blocks and read 4 ROM pages from the SD card. With an overclock the same burst
fits within the spare time per frame and the ring. At 240 MHz the heavy scenes run short of time
in every frame regardless; that is what Overclock is for.

### In-game saves

The cartridge's battery backup (SRAM, flash or EEPROM, `gamepak_backup`) is kept in
`/sd/retro-go/saves/gba/<rom>.sram`. This is the raw image, as other emulators keep it, so a save
can be copied in from or out to them. Save states do not include it, so it is read at every start,
a resume included. A devkit is switched off by pulling the cable, so the save does not wait for
a clean exit:

- **When it is written.** Every write the game makes to the backup is counted
  (`backup_changed` in `gba_memory.c`). Once the game has left the backup alone for 30 frames, `gbsp/main/main.c` writes it a step per
  frame, between frames, after a frame where the audio ring was nearly full and the work time
  averaged under a frame, like the cache flush above. The steps are: open a file, write 8 KB from
  internal RAM, close a file, or delete one. If no such frame comes within 3 s, it writes anyway.
  It also writes the whole save when a menu opens, since the menus can quit, and at exit. If the
  game writes again during a save, that save is abandoned and a new one starts once the game is
  quiet again.
- **Power-loss safety.** A save first writes `<rom>.sram.new`: the image, then a trailer with
  its size and CRC32. It then overwrites the `.sram` in place and deletes the `.new`. At every
  moment one complete copy is on the card. At the start, a `.new` with a valid trailer is the newer
  copy, and the `.sram` is rewritten from it; a `.new` without a valid trailer was cut short and is
  deleted. Nothing is renamed: a power loss inside a FAT rename can leave both names on one
  cluster chain, and deleting either name then frees the other's data. One exception: while the
  `.sram` is incomplete (after a power loss or a failed write), the `.new` is the only copy, so
  the next save goes straight to the `.sram`. For the one frame between the end of that write
  and the deletion of the `.new`, the `.new` is the older copy, and a power loss then goes back
  one save.
- **Write errors.** A failed save is tried again after 10 s. After three failures in a row
  while playing, the game pauses with an alert, once. A save that fails when a menu opens or at
  exit also shows an alert.
- **Read errors.** If a save is there but cannot be read after three tries, the game starts with
  in-game saves off for that session and shows an alert. Otherwise the game's next save would
  write a blank image over the one on the card.
- **128 KB flash.** A 128 KB save whose second half holds data selects the 128 KB flash chip,
  for games missing from the built-in database. Saves from other emulators are always 128 KB,
  mostly 0xFF, and keep the 64 KB chip.
- **Deleting.** The launcher's *Delete save* removes the `.new` too.

The log shows `Save <file>: N bytes` or `No save <file> yet` at the start, and
`Saved N bytes (type T) in X ms, longest step Y ms` after each save. Metroid: Zero Mission's
32 KB SRAM save took 13 steps over 229 ms, one per frame, and the longest step took 11 ms. The
audio ring has more lead than that at a calm moment.

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

Fixes made here, in addition to the audio changes described above:

- `gbsp-libretro/sound.c`: with the sound master disabled, the ring is drained to silence instead
  of being left full. Loading a state clears the ring and masks its indices, because a state saved
  by a build with a larger `BUFFER_SIZE` could otherwise index past it.
- `gbsp-libretro/gba_memory.c`: a ROM too large to load whole is paged in 32KB at a time through
  stdio, and stdio's default buffer here is 128 bytes. Each page cost 256 reads and one SD command
  per sector. The file now gets an 8KB buffer in DMA-capable internal RAM, so a page takes four
  multi-sector reads.

## SNES (snes9x)

### Audio

The light distortion had several causes. One was shared with every other emulator, and the rest
were in snes9x:

- **A frame dropped from every submit.** `drivers/audio/i2s.c` wrote each submit in 180-frame
  chunks, and the last chunk left out its last frame. Every system lost one sample per video
  frame (60 a second), a click at the frame rate. This is fixed for all of them; see the GBA
  section above.
- **Replay on underrun.** When the ring ran dry, the I2S DMA replayed its stale buffers in a loop.
  It now plays silence (`tx_desc_auto_clear`).
- **Audio out of order after an underrun.** The DMA passes the buffer it ran dry in, and the next
  submit wrote into the rest of it: that audio was lost, or played a turn of the ring later. The
  driver now finishes that buffer with silence first (all systems, see the GBA section above).
- **The mixer raced the emulator.** The audio task on core 1 mixed the frame
  (`S9xMixSamples`) while core 0 already ran the next one, and the SPC700 changed the sound state
  under the mixer: key on and off, the envelopes, the echo buffer. `snes9x/main/main_snes.c`
  now mixes on core 0 right after each frame, into one of three buffers. The audio task only
  submits them (priority 7, above the display task, 4 KB stack).
- **The echo was not clamped.** On the DSP the echo FIR output and the echo buffer are 16-bit.
  snes9x kept them in 32 bits, so with high echo feedback the echo built up past full scale
  and clipped harshly. Both are now
  clamped (`CLIP16` in `soundux.c`).
- **All-zero FIR taps played the echo at full volume.** `S9xSetFilterCoefficient` took
  taps 0..7 all at 0 to be the unfiltered path, which passes the echo at unity; the DSP outputs
  silence then. Only C0 = 127 with the other taps at 0 is that path now.
- **A short ring.** The default ring is 4 x 180 frames, 22.5 ms. The frame time varies with
  frameskip and with the in-game saves, so `snes9x/CMakeLists.txt` sets
  `RG_AUDIO_DMA_BUFFER_COUNT=10`: 56 ms, 7 KB of DMA memory.

Not changed: the DSP registers are still read once per frame, not per sample, and the sample
interpolation is snes9x's own, not the DSP's Gaussian filter. The *Audio filter* option (a
low-pass) is unchanged and is a useful A/B test. Changing *Speed* restarts the I2S clock, which
garbles the audio queued then (up to ~56 ms) once.

### Frame skipping driven by the audio

With the defects above fixed, the remaining distortion was underruns in busy scenes. There a
drawn frame costs 40-46 ms (emulation and rendering), more than twice the 16.7 ms of audio it
makes, and a skipped one about 8 ms. retro-go's automatic frameskip reacts to slow frames after
the fact, and a few drawn frames in a row empty the 56 ms ring: Super Metroid at 240 MHz had 74
underruns in 47 s, up to 39 in one second.

`snes9x/main/main_snes.c` now estimates the audio queued before each frame. That is what was
queued when the last frame's audio was sent, less the time since, plus a frame's worth. When a
send has to wait for room, the ring is full: its 9 buffers not playing, the frame the audio task
holds and the one in its queue, ~84 ms. A frame is drawn only when the audio queued outlasts its
drawing by 20 ms (`AUDIO_LEAD_MARGIN`). The cost of drawing is the last drawn frame's, or a
recent slower one's, decaying by an eighth per drawn frame. Otherwise the frame is skipped, up to
5 in a row (`AUDIO_SKIP_MAX`). Past that it is drawn anyway, unless the audio would run out during
its drawing: then it is skipped, up to 20 in a row (`AUDIO_SKIP_LIMIT`). That happens only when
the CPU cannot keep up even skipping. A simulation of such scenes had about half the silence,
with fewer frames drawn. retro-go's frameskip still sets the most frames drawn. With *Sound
emulation* off, nothing paces the frames but the frameskip, as before.

Measured, Super Metroid at 240 MHz, runs of 39 s and 29 minutes: no underruns, the game at full
speed. Calm scenes draw about 30 frames a second at frameskip 1, with 54-67 ms queued. Busy
scenes draw 10-25, 21 on average, with at least 12 ms queued. The in-game saves took 170-300 ms,
in steps of up to 14 ms, without an underrun.

The cost is the picture: busy scenes draw fewer frames. The renderer is the limit. **Overclock**
in the game options (260/280/300 MHz) leaves time for more drawn frames; see the GBA section for
its caveats.

Once a second the log prints the frames run and drawn, the frameskip, the time per frame spent in
the emulation, the mixing, the display, the saves and the wait for the audio task, the least
audio estimated queued before a frame's audio was sent, and the audio underruns and full waits
since the last line. Underruns mean the audio ran out; check them first when the sound breaks
up. *Audio wait* is how long the frame's audio waited for the audio task: the ring was full, and
the emulator is ahead. The first second after the start or a menu is not printed.

### In-game saves

The cartridge's battery SRAM is kept in `/sd/retro-go/saves/snes/<rom>.sram`, as a raw image like
other emulators' `.srm`. Before this change, the SRAM was only saved inside save states. It
follows the GBA design above (`.sram.new` with a size and CRC trailer, then the `.sram` in place,
then the `.new` deleted, nothing renamed, a step of up to 8 KB per frame between frames, a whole
save when a menu opens and at exit, the same retry and alert rules). Differences:

- **When it is written.** `CPU.SRAMModified` is harvested after every frame (`gfx.c` no longer
  clears it). The save starts once the game has left the SRAM alone for 30 frames. It steps in
  frames where the audio waited more than 2 ms, or in any frame once the game has been quiet for
  3 s.
- **Games that never stop writing.** Some use the SRAM as work RAM. A save writes a copy taken
  when it starts, so the game's writes do not abort it. After 10 s of unsaved writes, a save
  starts anyway, and it steps in any frame 3 s after that.
- **Writes the flag misses.** Banks 0x70-0x73 of a `MapExtraRAM` cartridge are plain pointers into
  the SRAM, and their writes do not set `CPU.SRAMModified`. When nothing is pending, the SRAM's
  CRC is compared with the card's once a second if the audio has lead, or every 10 s otherwise.
  Nothing counts those writes, so nothing shows when the game is done with them either. A change
  found that way is saved 10 s later, as for a game that never stops writing, or when a menu
  opens. For those cartridges (Derby Stallion 96, Thoroughbred Breeder 3, Sound Novel Tsukuru, RPG
  Tsukuru 2, Dezaemon), all 64 KB is saved, not just `SRAMMask + 1`.
- **Save states.** A state holds the SRAM as old as the state. Loading one keeps the game's SRAM,
  which is the battery file's or newer, as a cartridge would. One exception: if the game's SRAM is
  still blank (there is no save on the card, or it could not be read, and the game has not
  written yet), the state's SRAM is taken and written to the card. A state made before in-game
  saves existed holds the only copy, and the launcher's *Resume* loads it before the game runs, so
  resuming once moves that progress into the `.sram`.

Not handled: Dezaemon's banks 0x72-0x73 lie past the 64 KB SRAM buffer, as they did before.

The log shows `Save <file>: N bytes of SRAM` or `No save <file> yet` at the start (or
`The cartridge has no SRAM`), and `Saved N bytes of SRAM in X ms, longest step Y ms` after each
save.
