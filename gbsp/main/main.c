#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "../components/gbsp-libretro/common.h"
#include "../components/gbsp-libretro/memmap.h"
#include "../components/gbsp-libretro/sound.h"
#include "../components/gbsp-libretro/gba_memory.h"
#include "../components/gbsp-libretro/gba_cc_lut.h"

#define AUDIO_SAMPLE_RATE (GBA_SOUND_FREQUENCY)
/* One frame at 60 fps is AUDIO_SAMPLE_RATE/60. The buffer is sized for a much
   slower frame so that a scene running at ~15 fps can still be padded to cover
   the audio the DAC consumes in that time (see the submit site below), even at
   the highest Speed option (2.5x), where the DAC plays 2.5 times as fast. */
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE * 5 / 2 / 15 + 1)
/* The I2S driver's DMA ring is RG_AUDIO_DMA_BUFFER_COUNT buffers (set in
   ../CMakeLists.txt) of AUDIO_DMA_BUFFER_LEN frames (DMA_BUFFER_LEN in
   retro-go's drivers/audio/i2s.c). A submit that has to wait for room
   returns when the DMA frees a buffer: all the others are full then
   (AUDIO_RING_WAIT), and the one just written is on average half full
   (AUDIO_RING_FULL, the most the estimate assumes, and the level the
   stretch fills the ring to). Aiming a quarter buffer lower stretched a
   little less, about 0.4% more frame rate in heavy scenes, but left more
   gaps under frequent short stalls in the simulations (see the
   esp32-s3-n16r8 target's docs). */
#ifndef RG_AUDIO_DMA_BUFFER_COUNT
#define RG_AUDIO_DMA_BUFFER_COUNT 4
#endif
#define AUDIO_DMA_BUFFER_LEN 180
#define AUDIO_RING_WAIT ((RG_AUDIO_DMA_BUFFER_COUNT - 1) * AUDIO_DMA_BUFFER_LEN)
#define AUDIO_RING_FULL (AUDIO_RING_WAIT + AUDIO_DMA_BUFFER_LEN / 2)
_Static_assert(AUDIO_RING_FULL <= AUDIO_BUFFER_LENGTH, "audio_ring_prefill() submits AUDIO_RING_FULL frames of the mix buffer");

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;

u32 skip_next_frame = 0;
int sprite_limit = 1;

gbsp_memory_t *gbsp_memory;
#ifdef HAVE_DYNAREC
/* the Xtensa dynarec (gbsp-libretro/xtensa): translation caches in PSRAM
   mapped executable, see components/xjit */
#include "xjit_exec.h"
#include "soc/soc.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
int dynarec_enable = 1;
extern u32 xt_exec_delta;
u32 execute_arm_translate(u32 cycles);
#endif
/* scanline renderer on core 1 (gbsp-libretro/video.cpp) */
extern int gbsp_render_core1;
void gbsp_render_start(void);
void gbsp_render_wait(void);
#ifdef GBAPROF
extern int64_t gbaprof_render_us, gbaprof_wait_us;
extern u32 gbaprof_lag159, gbaprof_syncs, gbaprof_lag80, gbaprof_wakes;
extern u32 gbaprof_instr;
extern u32 gbaprof_halt_cycles;   /* the GBA CPU halted, waiting for an interrupt (gbsp-libretro/main.c) */
u32 gbaprof_m4a_kind(void);       /* the native m4a mixer: 0 not found, 1 stereo, 2 mono (m4a_hle.h) */
u32 gbaprof_pageloads;
static int64_t gbaprof_sync_us;
#endif
extern u32 gamepak_buffer_count;

/* three frame buffers: core 1 draws into currentUpdate while the display task
   sends the last submitted one; when the display is still busy at the end of
   a frame, that frame is not shown and emulation goes on in the third buffer
   instead of waiting (a full-screen 2x update is ~16-17 ms on the 20 MHz bus) */
static rg_surface_t *updates[3];
static rg_surface_t *displaying;   /* last submitted */
static rg_surface_t *pending;      /* finished while the display was busy: sent as soon as it is free */
static rg_surface_t *frame_done;   /* the frame just finished (GBABENCH hash) */
static uint32_t frames_not_shown;

/* called by the scanline code every 32 lines (core 0): send the pending frame
   as soon as the display task has taken the previous one */
void gbsp_display_poll(void)
{
    if (pending && !rg_display_is_busy())
    {
        rg_display_submit(pending, 0);
        displaying = pending;
        pending = NULL;
    }
}
static rg_surface_t *currentUpdate;
static rg_app_t *app;

static const char *SETTING_SOUND_EMULATION = "sound";
static const char *SETTING_AUDIO_STRETCH = "stretch";
static const char *SETTING_FRAMESKIP = "frameskip";
static const char *SETTING_OVERCLOCK = "overclock";
/* 0 when no overclocked session is running, 1 while one runs and has not yet
   lasted OVERCLOCK_PROVEN_US, 2 after (see the start of the main loop) */
static const char *SETTING_OVERCLOCK_LIVE = "overclockLive";
#define OVERCLOCK_PROVEN_US (60 * 1000000LL)
static int overclock_live;
static bool audio_stretch = true;
/* with the lines drawn on core 1: after a frame slower than 1/60 s, the next
   one is emulated but not drawn (see the end of the main loop) */
static bool render_skip_auto = true;

/* millionths of a frame in the I2S DMA ring after the last submit, which
   returned at ring_time (us); estimated, see the submit. The unit keeps the
   drain exact: in whole frames each estimate would round, and the error
   would pile up over the submits. */
static int64_t ring_level_q, ring_time;
/* the driver's counters at the last submit, see the submit */
static int64_t seen_full_waits, seen_underruns;
/* frames per second the DAC plays: AUDIO_SAMPLE_RATE times the Speed
   option, which changes only in the menus (a reset puts it back to 1x).
   retro-go corrects the sample rate for the Overclock option. */
static int64_t audio_rate = AUDIO_SAMPLE_RATE;

/* the same at `now`, the DAC having played on since */
static int64_t audio_ring_level_q(int64_t now)
{
    int64_t level = ring_level_q - (now - ring_time) * audio_rate;
    return level > 0 ? level : 0;
}

/* The DMA ring runs dry at boot and in the menus (which silence it). Fill it
   with silence before the game goes on: the first frames then have the full
   lead to absorb a slow one, and the restoring force of the stretch (see the
   submit) has nothing to make up, which it would do by stretching them, a
   dip in pitch. Zeroes the first AUDIO_RING_FULL frames of buf. Returns when
   the submit did: the start of the next frame's work. Runs at boot and after
   every menu, so it also picks up the rate the Speed option set. */
static int64_t audio_ring_prefill(rg_audio_sample_t *buf)
{
    /* not rg_audio_get_sample_rate(): with Overclock on, that is the rate
       asked of the driver, corrected for the PLL that clocks the I2S */
    audio_rate = (int64_t)(AUDIO_SAMPLE_RATE * rg_system_get_app_speed() + 0.5f);
#if CONFIG_IDF_TARGET_ESP32
    /* the timer runs from the APB clock there, which Overclock moves (on
       the S3 it runs from the crystal) */
    if (rg_system_get_overclock() && rg_system_get_cpu_speed() > 0)
        audio_rate = audio_rate * 240 / rg_system_get_cpu_speed();
#endif
    memset(buf, 0, AUDIO_RING_FULL * sizeof(*buf));
    rg_audio_submit(buf, AUDIO_RING_FULL);
    ring_time = rg_system_timer();
    ring_level_q = AUDIO_RING_FULL * 1000000LL;
    /* the idle ring ran dry, and this may have waited: no news for the
       next submit */
    const rg_audio_counters_t counters = rg_audio_get_counters();
    seen_full_waits = counters.fullWaits;
    seen_underruns = counters.underruns;
    return ring_time;
}

/* Linear interpolation of `in` frames to `out` (in < out), in place: output
   frame i only reads input frames at or before i, so going backwards never
   reads a frame already overwritten. The position is 16.16 fixed point (in
   and out are at most AUDIO_BUFFER_LENGTH): the truncated step leaves the last
   output frame under 0.1 input frames short of the last input frame */
static void stretch_linear(rg_audio_sample_t *buf, size_t in, size_t out)
{
    const uint32_t step = ((uint32_t)(in - 1) << 16) / (out - 1);
    for (size_t i = out; i-- > 0;)
    {
        const uint32_t pos = i * step;
        const size_t k = pos >> 16;
        const int frac = (pos & 0xFFFF) >> 4;
        rg_audio_sample_t s = buf[k];
        if (frac)   /* then k + 1 < in */
        {
            s.left += ((buf[k + 1].left - s.left) * frac) >> 12;
            s.right += ((buf[k + 1].right - s.right) * frac) >> 12;
        }
        buf[i] = s;
    }
}

void netpacket_poll_receive()
{
}

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

#ifdef GBAPROF
/* sampling profiler: core 0's interrupted PC at every FreeRTOS tick (1 kHz).
   The port stores the task's SP (its exception frame) in the TCB on ISR
   entry; XT_STK_PC is word 1 of that frame. */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_freertos_hooks.h>
#include <esp_heap_caps.h>
#include <esp_debug_helpers.h>
#include <esp_memory_utils.h>
#define SAMP_N 8192
static struct samp_s { uint32_t pc, n, a0, ps; } *samp;   /* PSRAM: internal RAM is full */
static volatile bool samp_on;
static uint32_t samp_up2, samp_all;
uint32_t samp_jit_lo = 0x42400000, samp_jit_len = 0x1C00000;   /* the translated code */
static void IRAM_ATTR samp_tick(void)
{
    if (!samp_on || !samp)
        return;
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(0);
    if (!t)
        return;
    uint32_t *f = *(uint32_t **)t, pc = f[1], up = f[3];
    /* in _xtos_set_intlevel (ROM, the end of a critical section, where the
       ticks it held back land): key on the caller of vPortExitCritical */
    if (pc - 0x400559d0u < 0x40)
    {
        esp_backtrace_frame_t fr = {.pc = f[1], .sp = f[4], .next_pc = f[3]};
        for (int d = 0; d < 5 && esp_stack_ptr_is_sane(fr.sp); d++)
        {
            if (!esp_backtrace_get_next_frame(&fr))
                break;
            if (d == 1) pc = fr.pc | 1;   /* odd: marks a critical-section caller */
            if (d == 3) up = fr.pc;
            if (d == 4) samp_up2 = fr.pc;
        }
    }
    if (pc - samp_jit_lo < samp_jit_len)
        pc &= ~0xFFu;   /* translated code (PSRAM mapped executable): 256-byte buckets */
    samp_all++;
    uint32_t h = (pc >> 2) & (SAMP_N - 1);
    for (int i = 0; i < 16; i++, h = (h + 1) & (SAMP_N - 1))
        if (samp[h].pc == pc || samp[h].n == 0)
        {
            samp[h].pc = pc;
            samp[h].n++;
            samp[h].a0 = up;   /* the last caller seen */
            samp[h].ps = (*(uint32_t **)t)[2];
            return;
        }
}
/* core 1: which task runs at each tick */
#define C1_N 12
typedef struct { TaskHandle_t t; uint32_t n; } task_ticks_t;
static task_ticks_t c1[C1_N], c0[C1_N];
/* core 1 PCs (renderer, display): which code shares the instruction cache */
#define SAMP1_N 2048
static struct { uint32_t pc, n; } *samp1;
static void IRAM_ATTR c1_tick(void)
{
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(1);
    if (samp1 && samp_on && t)
    {
        uint32_t pc = (*(uint32_t **)t)[1], h = (pc >> 2) & (SAMP1_N - 1);
        for (int i = 0; i < 16; i++, h = (h + 1) & (SAMP1_N - 1))
            if (samp1[h].pc == pc || samp1[h].n == 0) { samp1[h].pc = pc; samp1[h].n++; break; }
    }
    for (int i = 0; i < C1_N; i++)
        if (c1[i].t == t || !c1[i].t) { c1[i].t = t; c1[i].n++; break; }
    t = xTaskGetCurrentTaskHandleForCore(0);
    for (int i = 0; i < C1_N; i++)
        if (c0[i].t == t || !c0[i].t) { c0[i].t = t; c0[i].n++; break; }
}
static void c1_dump(void)
{
    for (int c = 1; c >= 0; c--)
    {
        task_ticks_t *a = c ? c1 : c0;
        uint32_t tot = 0;
        for (int i = 0; i < C1_N; i++) tot += a[i].n;
        printf("CORE%d", c);
        for (int i = 0; i < C1_N && a[i].t; i++)
            printf(" %s:%.0f%%", pcTaskGetName(a[i].t), 100.0 * a[i].n / (tot ? tot : 1));
        printf("\n");
        memset(a, 0, sizeof(c1));
    }
}
static void samp_dump(void)
{
    uint32_t total = 0;
    if (!samp)
        return;
    samp_on = false;
    uint32_t jit = 0, jit_ram = 0;
#ifdef HAVE_DYNAREC
    extern u8 *ram_translation_cache;
    const uint32_t ram_code = (uint32_t)(uintptr_t)ram_translation_cache + xt_exec_delta;
#else
    const uint32_t ram_code = 0;
#endif
    for (int i = 0; i < SAMP_N; i++)
    {
        total += samp[i].n;
        if (samp[i].pc - samp_jit_lo < samp_jit_len) jit += samp[i].n;
        if (samp[i].pc >= (ram_code & ~0xFFu) && samp[i].pc < ram_code + RAM_TRANSLATION_CACHE_SIZE) jit_ram += samp[i].n;
    }
    for (int k = 0; k < 300; k++)
    {
        int best = -1;
        for (int i = 0; i < SAMP_N; i++)
            if (samp[i].n && !(samp[i].pc - samp_jit_lo < samp_jit_len) && (best < 0 || samp[i].n > samp[best].n))
                best = i;
        if (best < 0)
            break;
        printf("GBASAMPLE %08x %u %.2f a0 %08x ps %08x\n", (unsigned)samp[best].pc, (unsigned)samp[best].n, 100.0 * samp[best].n / total,
               (unsigned)samp[best].a0, (unsigned)samp[best].ps);
        samp[best].n = 0;
    }
    for (int k = 0; samp1 && k < 150; k++)
    {
        int best = -1;
        for (int i = 0; i < SAMP1_N; i++)
            if (samp1[i].n && (best < 0 || samp1[i].n > samp1[best].n))
                best = i;
        if (best < 0)
            break;
        printf("GBASAMPLE1 %08x %u\n", (unsigned)samp1[best].pc, (unsigned)samp1[best].n);
        samp1[best].n = 0;
    }
    printf("GBASAMPLE total %u of %u ticks (last depth-4 caller %08x)\n", (unsigned)total, (unsigned)samp_all, (unsigned)samp_up2);
    {
        printf("GBASAMPLE translated code %.1f%% (RAM cache code %.1f%%)\n", 100.0 * jit / (total ? total : 1), 100.0 * jit_ram / (total ? total : 1));
    }
}
#endif

/* the state buffer: malloc, or the last ROM cache block when PSRAM is short */
static void *state_buffer(bool *borrowed)
{
    void *buffer = malloc(GBA_STATE_MEM_SIZE);
    *borrowed = false;
    if (!buffer && (buffer = gamepak_borrow_block()))
        *borrowed = true;
    return buffer;
}

static void state_buffer_free(void *buffer, bool borrowed)
{
    if (borrowed)
        gamepak_return_block();
    else
        free(buffer);
}

static bool save_state_handler(const char *filename)
{
    bool borrowed;
    void *buffer = state_buffer(&borrowed);
    if (!buffer)
        return false;
    gba_save_state(buffer);
    bool success = rg_storage_write_file(filename, buffer, GBA_STATE_MEM_SIZE, 0);
    state_buffer_free(buffer, borrowed);
    return success;
}

static bool load_state_handler(const char *filename)
{
    size_t buffer_len = GBA_STATE_MEM_SIZE;
    bool borrowed;
    void *buffer = state_buffer(&borrowed);
    if (!buffer)
        return false;
    bool success = rg_storage_read_file(filename, &buffer, &buffer_len, RG_FILE_USER_BUFFER)
                    && gba_load_state(buffer);
    state_buffer_free(buffer, borrowed);
    return success;
}

static bool reset_handler(bool hard)
{
    reset_gba();
    return true;
}

/* In-game saves: the cartridge's battery backup (SRAM, flash or EEPROM) in
   gamepak_backup, kept in <rom>.sram in the saves folder, the raw image as
   other emulators keep it. Save states do not hold it, so it is read at every
   start, resume included. The game changes it (backup_dirty) and it is
   written once the game has left it alone for a moment, a step per frame
   between frames (see the main loop), whole when a menu opens and at exit: a
   devkit is switched off by pulling the cable, so the save cannot wait for a
   clean exit.
   A power loss at any point must leave one complete copy on the card. A save
   writes <rom>.sram.new, the image followed by a trailer with its size and
   CRC, then overwrites the .sram in place, then deletes the .new. At the
   start a .new with a valid trailer is the newer copy, and one without was
   cut short. Nothing is renamed: a power loss inside a FAT rename can leave
   both names on one cluster chain, and deleting either then frees the other's
   data.
   While the .sram is not complete (cut short, or a write failed) the .new is
   the only copy, so the next save goes straight to the .sram. From its end to
   the deletion of the .new, the next frame, the .new is the older copy: a
   power loss in that frame goes back to it, one save earlier */
static char *backup_path, *backup_new_path;   /* NULL: no saves (see backup_load) */
static bool backup_sram_ok = true;   /* the .sram is complete, so the .new may be rewritten */
static uint32_t backup_saved;    /* backup_dirty when the card last matched */
static int32_t backup_quiet;     /* frames since backup_dirty changed (negative: retry later) */
static uint32_t backup_seen;
static int backup_failures;      /* in a row, while the game runs */
#define BACKUP_QUIET_FRAMES 30   /* the game is done writing */
#define BACKUP_URGENT_FRAMES 180 /* no calm moment came: write anyway */
#define BACKUP_CHUNK 8192        /* about 5 ms of SD writing */
#define BACKUP_MAGIC 0x56534247  /* "GBSV" */

typedef struct { uint32_t magic, size, crc; } backup_trailer_t;

enum { BACKUP_BUSY, BACKUP_DONE, BACKUP_FAILED, BACKUP_CHANGED };
enum { SAVE_IDLE, SAVE_NEW, SAVE_SRAM, SAVE_CLEAN };
static struct
{
    int phase;           /* SAVE_IDLE, or the file a running save is at */
    int fd;              /* that file, once opened */
    uint32_t gen;        /* backup_dirty when it started */
    uint32_t size, done, crc;
    uint8_t *buf;        /* internal RAM: the SD driver writes a chunk in one go, from PSRAM a sector at a time */
    int64_t start_us, step_max_us;
} bsave = {.phase = SAVE_IDLE, .fd = -1};

/* reads a save into gamepak_backup: 1 if read, 0 if there is none (a .new
   without a valid trailer was cut short), -1 if it could not be read */
static int backup_read(const char *path, bool is_new, size_t *size_out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    size_t size = st.st_size;
    if (is_new)
    {
        if (size <= sizeof(backup_trailer_t) || size > sizeof(gamepak_backup) + sizeof(backup_trailer_t))
            return 0;
        size -= sizeof(backup_trailer_t);
    }
    else if (size == 0)
        return 0;
    size = RG_MIN(size, sizeof(gamepak_backup));

    for (int tries = 0; tries < 3; tries++)   /* an SD read error is no reason to drop a save */
    {
        backup_trailer_t trailer;
        FILE *fp = fopen(path, "rb");
        const bool ok = fp && fread(gamepak_backup, 1, size, fp) == size
                        && (!is_new || fread(&trailer, sizeof(trailer), 1, fp) == 1);
        if (fp)
            fclose(fp);
        if (!ok)
            continue;
        const uint32_t crc = is_new ? rg_crc32(0, gamepak_backup, size) : 0;
        if (is_new && (trailer.magic != BACKUP_MAGIC || trailer.size != size || trailer.crc != crc))
        {
            RG_LOGW("%s was cut short: trailer %08x %u %08x, data %u %08x", path, (unsigned)trailer.magic,
                    (unsigned)trailer.size, (unsigned)trailer.crc, (unsigned)size, (unsigned)crc);
            memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
            return 0;
        }
        *size_out = size;
        return 1;
    }
    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    return -1;
}

static void backup_load(void)
{
    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    char *new_path = malloc(strlen(path) + 5);
    if (!new_path)
        RG_PANIC("Out of memory");
    sprintf(new_path, "%s.new", path);
    char *dir = strdup(path);   /* not rg_dirname: it stops at 100 characters */
    if (dir && strrchr(dir, '/'))
    {
        *strrchr(dir, '/') = 0;
        if (!rg_storage_mkdir(dir))   /* the saves then fail, and say so */
            RG_LOGE("Unable to create the save folder %s", dir);
    }
    free(dir);

    size_t size = 0;
    const int from_new = backup_read(new_path, true, &size);
    const int from_sram = from_new > 0 ? 0 : backup_read(path, false, &size);
    if (from_new < 0 || from_sram < 0)
    {
        /* playing on would write over the save the card holds */
        RG_LOGE("Unable to read the save %s, in-game saves are off", from_new < 0 ? new_path : path);
        rg_gui_alert("In-game saves are off", "The save file could not be read from the SD card.");
        free(path);
        free(new_path);
    }
    else
    {
        backup_path = path;
        backup_new_path = new_path;
        if (from_new == 0)
            unlink(new_path);   /* none, or one that was cut short */
        if (size)
            RG_LOGI("Save %s: %u bytes", from_new > 0 ? new_path : path, (unsigned)size);
        else
            RG_LOGI("No save %s yet", path);
    }
    backup_loaded(size);
    backup_saved = backup_seen = backup_dirty;
    if (from_new > 0)
    {
        backup_sram_ok = false;   /* the power went while it was written: write it again */
        backup_saved--;
    }
}

static void backup_save_end(void)
{
    if (bsave.fd >= 0)
        close(bsave.fd);
    bsave.fd = -1;
    bsave.phase = SAVE_IDLE;
    free(bsave.buf);
    bsave.buf = NULL;
}

/* one step of a save, a few ms: open a file, write a chunk, close it, or
   delete the .new. While the .sram is not complete the .new holds the save,
   so a save then goes straight to the .sram */
static int backup_save_step(void)
{
    const int64_t t0 = rg_system_timer();
    int result = BACKUP_BUSY;

    if (bsave.phase == SAVE_IDLE)
    {
        bsave.phase = backup_sram_ok ? SAVE_NEW : SAVE_SRAM;
        bsave.gen = backup_dirty;
        bsave.size = backup_save_size ? backup_save_size : sizeof(gamepak_backup);
        bsave.start_us = t0;
        bsave.step_max_us = 0;
        bsave.buf = heap_caps_malloc(BACKUP_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    const char *path = bsave.phase == SAVE_NEW ? backup_new_path : backup_path;

    if (bsave.phase == SAVE_CLEAN)
    {
        /* it holds this same save, or the one before when this one went
           straight to the .sram: that one must go */
        if (unlink(backup_new_path) != 0 && errno != ENOENT)
        {
            RG_LOGE("Unable to delete %s (%d)", backup_new_path, errno);
            result = BACKUP_FAILED;
        }
        else
            result = BACKUP_DONE;
    }
    else if (backup_dirty != bsave.gen)
        result = BACKUP_CHANGED;   /* written to while being saved: start over once it is quiet */
    else if (bsave.fd < 0)
    {
        bsave.done = 0;
        bsave.crc = 0;
        /* the .sram in place, keeping its clusters (the .new holds the save meanwhile) */
        bsave.fd = open(path, O_WRONLY | O_CREAT | (bsave.phase == SAVE_NEW ? O_TRUNC : 0), 0666);
        if (bsave.fd < 0)
        {
            RG_LOGE("Unable to open %s (%d)", path, errno);
            result = BACKUP_FAILED;
        }
    }
    else if (bsave.done < bsave.size)
    {
        const size_t n = RG_MIN(BACKUP_CHUNK, bsave.size - bsave.done);
        const uint8_t *src = gamepak_backup + bsave.done;
        if (bsave.buf)
            src = memcpy(bsave.buf, src, n);
        if (bsave.phase == SAVE_NEW)
            bsave.crc = rg_crc32(bsave.crc, src, n);
        const ssize_t w = write(bsave.fd, src, n);
        if (w != (ssize_t)n)
        {
            RG_LOGE("Unable to write %s: %d of %u bytes (%d)", path, (int)w, (unsigned)n, w < 0 ? errno : ENOSPC);
            result = BACKUP_FAILED;
        }
        bsave.done += n;
    }
    else
    {
        bool ok = true;
        if (bsave.phase == SAVE_NEW)
        {
            const backup_trailer_t trailer = {BACKUP_MAGIC, bsave.size, bsave.crc};
            ok = write(bsave.fd, &trailer, sizeof(trailer)) == sizeof(trailer);
        }
        ok = fsync(bsave.fd) == 0 && ok;
        ok = close(bsave.fd) == 0 && ok;
        bsave.fd = -1;
        if (!ok)
        {
            RG_LOGE("Unable to write %s (%d)", path, errno);
            result = BACKUP_FAILED;
        }
        else if (bsave.phase == SAVE_NEW)
        {
            backup_sram_ok = false;   /* the .new holds the save now */
            bsave.phase = SAVE_SRAM;
        }
        else
        {
            backup_sram_ok = true;
            bsave.phase = SAVE_CLEAN;
        }
    }

    const int64_t t1 = rg_system_timer();
    if (bsave.step_max_us < t1 - t0)
        bsave.step_max_us = t1 - t0;
    if (result == BACKUP_DONE)
    {
        backup_saved = bsave.gen;
        RG_LOGI("Saved %u bytes (type %d) in %d ms, longest step %d ms", (unsigned)bsave.size, (int)backup_type,
                (int)((t1 - bsave.start_us) / 1000), (int)(bsave.step_max_us / 1000));
    }
    if (result != BACKUP_BUSY)
        backup_save_end();
    return result;
}

/* the whole save at once, when the game is not running. False if it failed */
static bool backup_save_now(void)
{
    int result = BACKUP_DONE;
    if (!backup_path)
        return true;
    if (bsave.phase != SAVE_IDLE)   /* one running: finish it */
        while ((result = backup_save_step()) == BACKUP_BUSY)
            continue;
    if (backup_dirty != backup_saved)
        while ((result = backup_save_step()) == BACKUP_BUSY)
            continue;
    if (result == BACKUP_FAILED)
        return false;
    backup_failures = 0;   /* the card holds it: in-game failures count from here */
    return true;
}

static void backup_alert_failed(void)
{
    rg_gui_alert("In-game save failed", "The save could not be written to the SD card.");
}

static void overclock_set_live(int live)
{
    overclock_live = live;
    rg_settings_set_number(NS_APP, SETTING_OVERCLOCK_LIVE, live);
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(displaying ? displaying : currentUpdate, 0);
    }
    else if (event == RG_EVENT_SHUTDOWN)
    {
        if (!backup_save_now())
            backup_alert_failed();
        if (overclock_live)
        {
            overclock_set_live(0);   /* a clean exit: the level is kept */
            rg_settings_commit();
        }
    }
}

#ifdef GBABENCH
#include "xtensa_perfmon_access.h"
#include "xtensa/xt_perf_consts.h"
static int bench_frame;
/* core-0 LX7 counters around the CPU emulation; 2 counters, 3 pairs taken in
   turn frame by frame (each total is scaled by 3 when printed) */
static const uint16_t perf_sel[3][2][2] = {
    {{XTPERF_CNT_CYCLES, XTPERF_MASK_CYCLES}, {XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL}},
    {{XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ALL}, {XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL}},
    {{XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS}, {XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_BUSY | XTPERF_MASK_I_STALL_IN_PIF}},
};
static uint64_t perf_sum[3][2];
static void perf_begin(int f)
{
    const int p = f % 3;
    xtensa_perfmon_stop();
    for (int i = 0; i < 2; i++)
    {
        xtensa_perfmon_init(i, perf_sel[p][i][0], perf_sel[p][i][1], 0, -1);
        xtensa_perfmon_reset(i);
    }
    xtensa_perfmon_start();
}
static void perf_end(int f)
{
    xtensa_perfmon_stop();
    for (int i = 0; i < 2; i++)
        perf_sum[f % 3][i] += xtensa_perfmon_value(i);
}
/* right held, B 4 frames in 16, A 10 frames in 120 (libretro bits) */
static int16_t bench_keys(int f)
{
    int16_t m = 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if ((f & 15) < 4) m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
    if (f % 120 < 10) m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
    return m;
}
#endif

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    // RG_LOGI("%u, %u, %u, %u", port, device, index, id);
#ifdef GBABENCH
    return bench_keys(bench_frame);
#endif
    uint32_t joystick = rg_input_read_gamepad();
    int16_t val = 0;
    if (joystick & RG_KEY_DOWN) val |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (joystick & RG_KEY_UP) val |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (joystick & RG_KEY_LEFT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (joystick & RG_KEY_RIGHT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (joystick & RG_KEY_START) val |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (joystick & RG_KEY_SELECT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (joystick & RG_KEY_B) val |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
    if (joystick & RG_KEY_A) val |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    if (joystick & RG_KEY_L) val |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
    if (joystick & RG_KEY_R) val |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
    return val;
}

void set_fastforward_override(bool fastforward)
{
}

static rg_gui_event_t sound_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_master_enable = !sound_master_enable;
        rg_settings_set_number(NS_APP, SETTING_SOUND_EMULATION, sound_master_enable);
    }

    strcpy(option->value, sound_master_enable ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t stretch_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        audio_stretch = !audio_stretch;
        rg_settings_set_number(NS_APP, SETTING_AUDIO_STRETCH, audio_stretch);
    }

    strcpy(option->value, audio_stretch ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t frameskip_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        render_skip_auto = !render_skip_auto;
        rg_settings_set_number(NS_APP, SETTING_FRAMESKIP, render_skip_auto);
    }

    strcpy(option->value, render_skip_auto ? _("Auto") : _("Off"));

    return RG_DIALOG_VOID;
}

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &sound_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio stretch"), "-", RG_DIALOG_FLAG_NORMAL, &stretch_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Frameskip"), "-", RG_DIALOG_FLAG_NORMAL, &frameskip_toggle_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

#ifdef HAVE_DYNAREC
/* the dynarec translates a block's branch targets recursively: deeper than
   the main task's stack, so the emulator runs on its own task */
static void gbsp_main(void);
static void gbsp_task(void *arg)
{
    gbsp_main();
}
void app_main(void)
{
    if (xTaskCreatePinnedToCore(gbsp_task, "gbsp", 20 * 1024, NULL, uxTaskPriorityGet(NULL), NULL, 0) != pdPASS)
        gbsp_main();   /* no memory for the stack: try on the main task */
    vTaskDelete(NULL);
}
static void gbsp_main(void)
#else
void app_main(void)
#endif
{
    const rg_config_t config = {
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60,
        .storageRequired = true,
        .romRequired = true,
        .handlers.loadState = &load_state_handler,
        .handlers.saveState = &save_state_handler,
        .handlers.reset = &reset_handler,
        .handlers.screenshot = &screenshot_handler,
        .handlers.event = &event_handler,
        .handlers.options = &options_handler,
    };
    app = rg_system_init(&config);
    rg_system_set_tick_rate(60);

    sound_master_enable = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, true);
    audio_stretch = rg_settings_get_number(NS_APP, SETTING_AUDIO_STRETCH, true);
    render_skip_auto = rg_settings_get_number(NS_APP, SETTING_FRAMESKIP, true);
#ifdef GBABENCH
    render_skip_auto = false; /* every frame drawn: a reproducible hash and work figures */
#endif

#ifdef HAVE_DYNAREC
    /* the dynarec's IWRAM (64 KB with its SMC tags) takes the internal RAM */
    updates[0] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_SLOW);
#else
    updates[0] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
#endif
    updates[0]->height = GBA_SCREEN_HEIGHT;
    /* second buffer (PSRAM: internal RAM is full): the display task on core 1
       sends one frame while the next is drawn into the other, otherwise the
       top of the next frame shows up in the one being sent */
    updates[1] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_SLOW);
    if (updates[1])
        updates[1]->height = GBA_SCREEN_HEIGHT;
    updates[2] = updates[1] ? rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_SLOW) : NULL;
    if (updates[2])
        updates[2]->height = GBA_SCREEN_HEIGHT;
    currentUpdate = updates[0];

    gba_screen_pixels = currentUpdate->data;

    gbsp_memory = rg_alloc(sizeof(*gbsp_memory), MEM_ANY);
    RG_LOGI("gbsp_memory=%p", gbsp_memory);

    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);
#ifdef HAVE_DYNAREC
    {
        static xj_exec_t jit;   /* before the ROM cache takes the rest of PSRAM */
#ifdef XT_IRAM_CACHE
        /* internal RAM: written through the data bus alias (byte stores), run
           through the instruction bus */
        /* the exec heap is tiny: take plain internal RAM in SRAM1, which is
           also mapped on the instruction bus (memory protection off) */
        static uint8_t iram_cache[ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE] __attribute__((aligned(64)));   /* .bss: not fragmented */
        jit.size = sizeof(iram_cache);
        jit.data = iram_cache;
        if (jit.data && (uintptr_t)jit.data >= SOC_DIRAM_DRAM_LOW && (uintptr_t)jit.data + jit.size <= SOC_DIRAM_DRAM_HIGH)
            jit.exec = MAP_DRAM_TO_IRAM((uint32_t)(uintptr_t)jit.data);
        else
#else
        if (!xj_exec_alloc_psram(&jit, ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE))
#endif
        {
            static char msg[128];
            snprintf(msg, sizeof(msg), "No memory for the translation caches (exec largest %u, exec free %u, internal free %u)",
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC), (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            RG_PANIC(msg);
        }
        rom_translation_cache = jit.data;
        ram_translation_cache = jit.data + ROM_TRANSLATION_CACHE_SIZE;
        rom_translation_ptr = rom_translation_cache;
        ram_translation_ptr = ram_translation_cache;
        xt_exec_delta = jit.exec - (u32)(uintptr_t)jit.data;
#ifdef GBAPROF
        {
            extern uint32_t samp_jit_lo, samp_jit_len;
            samp_jit_lo = jit.exec;
            samp_jit_len = ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE;
        }
#endif
        RG_LOGI("dynarec: translation caches %u KB at %p (exec %08lx)",
                (unsigned)((ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE) / 1024), jit.data, (unsigned long)jit.exec);
    }
#endif
#ifdef HAVE_DYNAREC
    {
        extern void gbsp_rvram_alloc(void);   /* the renderer's VRAM copy, before the ROM cache takes PSRAM */
        gbsp_rvram_alloc();
    }
#endif
    init_gamepak_buffer();
    RG_LOGI("ROM cache: %u blocks of 1 MB (free: internal %u KB, largest %u KB, PSRAM %u KB)", (unsigned)gamepak_buffer_count,
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    init_sound();
    // load_bios(RG_BASE_PATH_BIOS "/gba_bios.bin");

    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
    if (load_gamepak(NULL, app->romPath, FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }

    gbsp_render_start();
    RG_LOGI("line renderer on core 1: %s", gbsp_render_core1 ? "yes" : "no");
    RG_LOGI("reset_gba");
    reset_gba();
    backup_load();   /* not in the save state: read on a resume too */

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        RG_LOGI("load_state");
        rg_emu_load_state(app->saveSlot);
    }

    /* retro-go forgets the Overclock level at a reset, and a GBA game needs
       it: keep the level chosen in the menus. Applied once the ROM and the
       save state are read (at the stock SD clock), before the ring prefill,
       which takes the audio rate. In case the level was the cause, it is
       dropped (to be chosen again) when the last overclocked session did not
       exit cleanly: the reset reason alone does not tell, as a panic reboots
       into retro-go's crash dialog, which goes to the launcher, and this app
       then starts from there. Except after a power loss once that session
       had run a while: that is how a devkit is switched off. */
    int overclock = rg_settings_get_number(NS_APP, SETTING_OVERCLOCK, 0);
    int64_t overclock_since = 0;
    {
        const esp_reset_reason_t reason = esp_reset_reason();
        const int live = rg_settings_get_number(NS_APP, SETTING_OVERCLOCK_LIVE, 0);
        if (overclock && (live == 1 || (live == 2 && reason != ESP_RST_POWERON)
                          || reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT
                          || reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT))
        {
            RG_LOGW("last session %d, reset reason %d: Overclock %d not applied", live, (int)reason, overclock);
            overclock = 0;
            rg_settings_set_number(NS_APP, SETTING_OVERCLOCK, 0);
        }
        overclock_set_live(overclock ? 1 : 0);
        rg_settings_commit();   /* on the card before the clock goes up */
        if (overclock)
        {
            rg_system_set_overclock(overclock);
            overclock_since = rg_system_timer();
        }
    }

    RG_LOGI("emulation loop");

    static rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH];
    int64_t work_start = audio_ring_prefill(mixbuffer);  /* end of the last submit */
    int64_t work_ema_us = 1000000 / 60;  /* average work time per frame */
    int64_t work_prev_us = 1000000 / 15; /* the last frame's; none: take it as slow */

    while (true)
    {
        // RG_TIMER_INIT();
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (!backup_save_now())   /* the menus can quit */
                backup_alert_failed();
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
            if (rg_system_get_overclock() != overclock)
            {
                overclock = rg_system_get_overclock();
                rg_settings_set_number(NS_APP, SETTING_OVERCLOCK, overclock);
                overclock_set_live(overclock ? 1 : 0);   /* a new level starts over */
                overclock_since = rg_system_timer();
                rg_settings_commit();
            }
#ifdef ROM_FLUSH_SOFT
            /* a flush due soon (see below) costs nothing noticeable here */
            if (rom_translation_ptr - rom_translation_cache > ROM_TRANSLATION_CACHE_SIZE - ROM_FLUSH_SOFT)
                flush_translation_cache_rom();
#endif
            /* the time in the menu is not work */
            work_start = audio_ring_prefill(mixbuffer);
            work_prev_us = 1000000 / 15;
            continue;
        }

#ifdef ROM_FLUSH_SOFT
        /* The ROM translation cache fills up in a minute or two, then it is
           flushed and the code running is translated again: a burst of tens
           of ms over the next frames. Translation would flush it when it
           reaches the end, mid-frame, wherever that happens. Instead flush it
           a little earlier, here with no translated code running, after a
           frame where the audio ring was nearly full and the work time
           average below a frame: a calm moment, where the ring has the most
           lead to ride out the burst. If none comes, at ROM_FLUSH_URGENT. */
        {
            const size_t used = rom_translation_ptr - rom_translation_cache;
            if (used > ROM_TRANSLATION_CACHE_SIZE - ROM_FLUSH_SOFT
                && (used > ROM_TRANSLATION_CACHE_SIZE - ROM_FLUSH_URGENT
                    || (ring_level_q >= (int64_t)(AUDIO_RING_FULL - AUDIO_DMA_BUFFER_LEN) * 1000000
                        && work_ema_us < app->frameTime)))
                flush_translation_cache_rom();
        }
#endif
        /* the level has run a while: from now on a power loss keeps it (see
           above). Written at a calm moment too, the SD card takes a few ms */
        if (overclock_live == 1 && startTime - overclock_since > OVERCLOCK_PROVEN_US
            && ring_level_q >= (int64_t)(AUDIO_RING_FULL - AUDIO_DMA_BUFFER_LEN) * 1000000
            && work_ema_us < app->frameTime)
        {
            overclock_set_live(2);
            rg_settings_commit();
        }
        /* in-game saves (see backup_load): once the game has stopped
           writing, the save goes to the card a step per frame, here with no
           translated code running, at calm moments like the flush above */
        if (backup_dirty != backup_seen)
        {
            backup_seen = backup_dirty;
            backup_quiet = 0;
        }
        else if (backup_quiet < BACKUP_URGENT_FRAMES)
            backup_quiet++;
        if (backup_path
            && (bsave.phase != SAVE_IDLE || (backup_dirty != backup_saved && backup_quiet >= BACKUP_QUIET_FRAMES))
            && (bsave.phase == SAVE_CLEAN   /* without delay, see above */
                || backup_quiet >= BACKUP_URGENT_FRAMES
                || (ring_level_q >= (int64_t)(AUDIO_RING_FULL - AUDIO_DMA_BUFFER_LEN) * 1000000
                    && work_ema_us < app->frameTime)))
        {
            const int result = backup_save_step();
            if (result == BACKUP_DONE)
                backup_failures = 0;
            else if (result == BACKUP_FAILED)
            {
                backup_quiet = -10 * 60;   /* try again in 10 s */
                if (++backup_failures == 3)   /* failing for half a minute: say so, once */
                {
                    backup_alert_failed();
                    work_start = audio_ring_prefill(mixbuffer);   /* as after a menu */
                    work_prev_us = 1000000 / 15;
                    continue;
                }
            }
        }
        update_input();
        rumble_frame_reset();
        clear_gamepak_stickybits();
#ifdef GBAPROF
        const bool drawn = !skip_next_frame;
        const int64_t t_exec = rg_system_timer();
        gbaprof_render_us = 0;
#endif
#ifdef GBABENCH
        const int64_t tb_exec = rg_system_timer();
        perf_begin(bench_frame);
#endif
#ifdef HAVE_DYNAREC
        execute_arm_translate(execute_cycles);
#else
        execute_arm(execute_cycles);
#endif
#ifdef GBABENCH
        perf_end(bench_frame);
#endif
#ifdef GBABENCH
        const int64_t tb_render = rg_system_timer();
        int64_t tb_display = tb_render;
#endif
        // RG_TIMER_LAP("execute_arm");
#ifdef GBAPROF
        const int64_t t_disp = rg_system_timer();
#endif

        if (!skip_next_frame)
        {
            gbsp_render_wait();   /* core 1 finishes the frame's last lines */
#ifdef GBABENCH
            tb_display = rg_system_timer();
#endif
            frame_done = currentUpdate;
            if (updates[2])
            {
                if (pending)            /* never sent: this newer frame replaces it */
                    frames_not_shown++;
                pending = currentUpdate;
                gbsp_display_poll();
                for (int i = 0; i < 3; i++)
                    if (updates[i] != displaying && updates[i] != pending && updates[i] != frame_done)
                    {
                        currentUpdate = updates[i];
                        break;
                    }
                gba_screen_pixels = currentUpdate->data;
            }
            else if (updates[1])
            {
#ifdef GBAPROF
                const int64_t t_sync = rg_system_timer();
#endif
                while (rg_display_is_busy())   /* the other buffer has been sent */
                    rg_task_yield();
#ifdef GBAPROF
                gbaprof_sync_us += rg_system_timer() - t_sync;
#endif
                rg_display_submit(currentUpdate, 0);
                currentUpdate = updates[currentUpdate == updates[0]];
                gba_screen_pixels = currentUpdate->data;
            }
            else
                rg_display_submit(currentUpdate, 0);
        }
#ifdef GBAPROF
        const int64_t t_snd = rg_system_timer();
#endif

#ifdef GBABENCH
        const int64_t tb_sound = rg_system_timer();
#endif
        size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
        // RG_TIMER_LAP("sound_read_samples");
#ifdef GBABENCH
        {
            static int64_t work_us, exec_us, rwait_us, disp_us, snd_us;
            exec_us += tb_render - tb_exec;
            rwait_us += tb_display - tb_render;
            disp_us += tb_sound - tb_display;
            snd_us += rg_system_timer() - tb_sound;
            static uint32_t acc;
            work_us += rg_system_timer() - startTime;
            /* the frame just submitted (the buffers were swapped) */
            const rg_surface_t *shown = frame_done ? frame_done : currentUpdate;
            uint32_t h = 2166136261u;
            for (int y = 0; y < GBA_SCREEN_HEIGHT; y++)
            {
                const uint16_t *line = (const uint16_t *)((const uint8_t *)shown->data + y * shown->stride);
                for (int x = 0; x < GBA_SCREEN_WIDTH; x++)
                    h = (h ^ line[x]) * 16777619u;
            }
            acc = acc * 31 + h;
            if (++bench_frame % 300 == 0)
            {
                printf("GBABENCH frames %d work %.2f ms/frame hash %08lx | exec %.2f render-wait %.2f display %.2f sound %.2f | not shown %u\n",
                       bench_frame, work_us / 1000.0 / 300, (unsigned long)acc, exec_us / 300000.0, rwait_us / 300000.0,
                       disp_us / 300000.0, snd_us / 300000.0, (unsigned)frames_not_shown);
                frames_not_shown = 0;
                {
                    /* per frame, in thousands (Mcycles/1000); each pair ran one frame in three */
                    const double k = 3.0 / 300 / 1000;
                    printf("GBABENCH perf: kcycles %.0f kinstr %.0f (%.2f cyc/instr) | I-stall %.0f (cache-miss %.0f, busy/PIF %.0f) D-stall %.0f\n",
                           perf_sum[0][0] * k, perf_sum[0][1] * k, perf_sum[0][1] ? (double)perf_sum[0][0] / perf_sum[0][1] : 0.0,
                           perf_sum[1][0] * k, perf_sum[2][0] * k, perf_sum[2][1] * k, perf_sum[1][1] * k);
                    memset(perf_sum, 0, sizeof(perf_sum));
                }
                {
                    static rg_display_counters_t last;
                    rg_display_counters_t c = rg_display_get_counters();
                    int shown = (int)((c.fullFrames + c.partFrames) - (last.fullFrames + last.partFrames));
                    printf("GBABENCH display: %d frames sent (%d full), %.2f ms each\n", shown, (int)(c.fullFrames - last.fullFrames),
                           shown ? (c.busyTime - last.busyTime) / 1000.0 / shown : 0.0);
                    last = c;
                }
                work_us = exec_us = rwait_us = disp_us = snd_us = 0;
            }
        }
#endif
#ifdef GBAPROF
        {
            /* cpu = execute_arm minus the scanline renderer inside it */
            static int64_t cpu_us, render_us, disp_us, snd_us, t_last, instr;
            static int frames, drawn_n;
            const int64_t now = rg_system_timer();
            cpu_us += (t_disp - t_exec) - (gbsp_render_core1 ? 0 : gbaprof_render_us);
            render_us += gbaprof_render_us;
            disp_us += t_snd - t_disp;
            snd_us += now - t_snd;
            frames++;
            drawn_n += drawn;
            instr += gbaprof_instr;
            gbaprof_instr = 0;
            if (now - t_last >= 1000000)
            {
                static int seconds;
                if (++seconds == 4)
                {
                    samp = heap_caps_calloc(SAMP_N, sizeof(*samp), MALLOC_CAP_SPIRAM);
                    samp1 = heap_caps_calloc(SAMP1_N, sizeof(*samp1), MALLOC_CAP_SPIRAM);
                    esp_register_freertos_tick_hook_for_cpu(samp_tick, 0);
                    esp_register_freertos_tick_hook_for_cpu(c1_tick, 1);
                    samp_on = true;
                }
                else if (seconds == 24)
                    samp_dump();
                printf("GBAWAIT ms/frame: wait for core 1 %.2f (%.1f syncs), display sync %.2f | core 1 lines behind at line 80: %.1f, 159: %.1f, wakes %.1f\n",
                       gbaprof_wait_us / 1000.f / frames, (float)gbaprof_syncs / frames, gbaprof_sync_us / 1000.f / frames, (float)gbaprof_lag80 / frames, (float)gbaprof_lag159 / frames, (float)gbaprof_wakes / frames);
                gbaprof_wait_us = gbaprof_sync_us = 0;
                gbaprof_lag159 = gbaprof_syncs = gbaprof_lag80 = gbaprof_wakes = 0;
                {
                    extern int64_t gbaprof_wait_by[8];
                    printf("GBAWAIT by cause ms/frame: other %.2f cpuBG %.2f cpuOBJ %.2f dmaBG %.2f dmaOBJ %.2f oam %.2f pal %.2f end %.2f\n",
                           gbaprof_wait_by[0] / 1000.f / frames, gbaprof_wait_by[1] / 1000.f / frames, gbaprof_wait_by[2] / 1000.f / frames,
                           gbaprof_wait_by[3] / 1000.f / frames, gbaprof_wait_by[4] / 1000.f / frames, gbaprof_wait_by[5] / 1000.f / frames,
                           gbaprof_wait_by[6] / 1000.f / frames, gbaprof_wait_by[7] / 1000.f / frames);
                    memset(gbaprof_wait_by, 0, sizeof(gbaprof_wait_by));
                }
                c1_dump();
#ifdef HAVE_DYNAREC
                {
                    extern u32 xt_prof_syncs, xt_prof_sync_cycles, flush_ram_count;
                    static u32 last_flush;
                    extern u32 gbaprof_notify_cycles, gbaprof_notifies;
                    printf("GBAJIT per second: %u cache syncs (%.2f ms/frame), %u RAM flushes, %u notifies (%.2f ms/frame)\n", (unsigned)xt_prof_syncs,
                           xt_prof_sync_cycles / (rg_system_get_cpu_speed() * 1000.f) / frames, (unsigned)(flush_ram_count - last_flush),
                           (unsigned)gbaprof_notifies, gbaprof_notify_cycles / (rg_system_get_cpu_speed() * 1000.f) / frames);
                    gbaprof_notify_cycles = gbaprof_notifies = 0;
                    extern u32 xt_prof_translate_cycles, xt_prof_translate_blocks;
                    static u32 last_rom_flush, last_rom_mid;
                    printf("GBAJIT translate: %u blocks, %.2f ms/frame | ROM flushes %u (mid-frame %u)\n", (unsigned)xt_prof_translate_blocks,
                           xt_prof_translate_cycles / (rg_system_get_cpu_speed() * 1000.f) / frames,
                           (unsigned)(flush_rom_count - last_rom_flush), (unsigned)(flush_rom_mid_frame - last_rom_mid));
                    last_rom_flush = flush_rom_count;
                    last_rom_mid = flush_rom_mid_frame;
                    xt_prof_translate_cycles = xt_prof_translate_blocks = 0;
                    printf("GBAJIT code: ROM cache %u KB, RAM cache %u KB\n",
                           (unsigned)((rom_translation_ptr - rom_translation_cache) / 1024), (unsigned)((ram_translation_ptr - ram_translation_cache) / 1024));
                    xt_prof_syncs = xt_prof_sync_cycles = 0;
                    last_flush = flush_ram_count;
                }
#endif
                printf("GBAPROF %d frames (%d drawn): ms/frame cpu %.2f sound %.2f display %.2f | render %.2f per drawn frame | %d instr/frame, %.0f cycles/instr | %u ROM pages loaded | GBA CPU halted %.0f%% | m4a %u\n",
                       frames, drawn_n, cpu_us / 1000.f / frames, snd_us / 1000.f / frames, disp_us / 1000.f / frames,
                       drawn_n ? render_us / 1000.f / drawn_n : 0.f, (int)(instr / frames), instr ? cpu_us * (double)rg_system_get_cpu_speed() / instr : 0.0, (unsigned)gbaprof_pageloads,
                       100.0 * gbaprof_halt_cycles / (frames * 280896.0), (unsigned)gbaprof_m4a_kind());
                gbaprof_pageloads = gbaprof_halt_cycles = 0;
                cpu_us = render_us = disp_us = snd_us = instr = 0;
                frames = drawn_n = 0;
                t_last = now;
            }
        }
#endif

        rg_system_tick(rg_system_timer() - startTime);

        /* Keep the DAC fed when the GBA runs below 60 fps.

           rg_audio_submit() blocks while the I2S ring is full, and that is
           what paces this loop. A GBA frame yields ~549 samples (16.75 ms of
           audio): a frame that takes longer to emulate drains the ring, and
           once it is empty the DAC plays a gap of silence. With "Audio
           stretch" on, the samples are stretched to cover the time instead:
           the stream does not break, but the pitch drops with the frame rate.

           The amount is sized from the work time (the loop period without
           the blocking in submit). The full period would contain the blocking
           this very decision causes, and the stretch would run away (an
           earlier attempt, see the esp32-s3-n16r8 target's docs). It comes
           from:
           - an average of the work time, so the pitch does not jump with
             every frame that is a little faster or slower than the last;
           - plus an eighth of what the ring lacked after the last submit.
             The work time misses what runs outside it (the stretch and the
             submit's own conversion) and the average lags a rising load, so
             on its own the ring would slowly drain under a heavy load. This
             pulls it back to AUDIO_RING_FULL. It reads the level after the
             last submit, not the current one, which would add this frame's
             work time a second time and make the pitch wobble with it;
           - this frame's work time instead of the average, plus the same
             eighth, when the ring holds less than a frame of audio and this
             frame was slower than the average: a load that rises suddenly
             would otherwise empty the ring before the average catches up.
             Only after a slow frame, or with the ring already a buffer short:
             one slow frame in a calm scene has the full ring to absorb it,
             and stretching it to its whole length would dip the pitch for it
             and fill the ring, costing the next frames time blocked in submit.
           Too little runs the ring dry, a gap; too much fills it, and the
           next submit blocks, which costs frame rate. */
        const int64_t now = rg_system_timer();
        int64_t work_us = now - work_start;
        if (work_us > 1000000 / 15)
            work_us = 1000000 / 15;
        /* updated with the stretch off too, ready for when it is turned on */
        work_ema_us += (work_us - work_ema_us) / 8;
        if (audio_stretch)
        {
            /* the level never exceeds AUDIO_RING_FULL, see below */
            const int64_t restore = (AUDIO_RING_FULL - ring_level_q / 1000000) / 8;
            const int64_t work_samples = work_us * audio_rate / 1000000;
            int64_t target = work_ema_us * audio_rate / 1000000 + restore;
            if (audio_ring_level_q(now) < AUDIO_SAMPLE_RATE / 60 * 1000000LL
                && (work_prev_us > 1000000 / 60 || ring_level_q < (AUDIO_RING_FULL - AUDIO_DMA_BUFFER_LEN) * 1000000LL)
                && work_samples + restore > target)
                target = work_samples + restore;
            if (target > AUDIO_BUFFER_LENGTH)
                target = AUDIO_BUFFER_LENGTH;
            if (frames_count > 0 && frames_count < target)
            {
                stretch_linear(mixbuffer, frames_count, target);
                frames_count = target;
            }
        }
        work_prev_us = work_us;

        const int64_t submit_start = rg_system_timer();
        rg_audio_submit(mixbuffer, frames_count);
        /* restart the clock after the blocking call, never across it */
        work_start = rg_system_timer();

        /* The ring level comes from the clock: what was in it after the last
           submit, minus what the DAC played since, plus what was just
           written. The driver counts the two events that prove it wrong:
           - the submit waited for room: all buffers but the one being
             written were full, so the ring holds at least AUDIO_RING_WAIT.
             Only raised to that: when the clock says more, it is closer.
             (How long the submit took is no sign of a wait: another task
             can hold it up as long with the ring far from full, and taking
             that for a full ring stops the restoring force, overestimates
             that pile up under a heavy load until the ring runs dry.)
           - the ring ran dry (the driver tells only when the DMA reaches the
             end of the buffer it ran dry in, up to one buffer late; a gap
             that ends before, the submit having written past that buffer,
             goes unreported): it holds about what was just written, less
             what played during the submit, within a buffer (the DMA may have
             played silence, not this audio, meanwhile; the driver finishes
             the buffer it ran dry in with silence first, so none is lost).
           A driver without the counters leaves the clock alone. */
        const rg_audio_counters_t counters = rg_audio_get_counters();
        ring_level_q = audio_ring_level_q(work_start) + (int64_t)frames_count * 1000000;
        if (counters.fullWaits != seen_full_waits)
        {
            if (ring_level_q < AUDIO_RING_WAIT * 1000000LL)
                ring_level_q = AUDIO_RING_WAIT * 1000000LL;
        }
        else if (counters.underruns != seen_underruns)
        {
            ring_level_q = (int64_t)frames_count * 1000000 - (work_start - submit_start) * audio_rate;
            if (ring_level_q < 0)
                ring_level_q = 0;
        }
        if (ring_level_q > AUDIO_RING_FULL * 1000000LL)
            ring_level_q = AUDIO_RING_FULL * 1000000LL;
        seen_full_waits = counters.fullWaits;
        seen_underruns = counters.underruns;
        ring_time = work_start;

        /* With the lines drawn on core 1, ignore app->frameskip (which
           retro-go raises when the game runs below full speed, and the
           Overclock option sets). A skipped frame still runs the CPU, the
           sound and the VRAM copy, but core 0 no longer waits for core 1 to
           draw lines before a write to the video memory and at the end of
           the frame, and core 1 leaves PSRAM to core 0. So with
           render_skip_auto, a frame whose work time (as for the stretch)
           exceeded a frame at the Speed setting has the next one skipped,
           never two in a row: at least half the frames are drawn. */
        if (gbsp_render_core1)
            skip_next_frame = render_skip_auto && !skip_next_frame && work_us > app->frameTime;
        else if (skip_next_frame == 0)
            skip_next_frame = app->frameskip;
        else if (skip_next_frame > 0)
            skip_next_frame--;
    }

    RG_PANIC("GBsP Ended");
}
