#include <rg_system.h>
#include <snes9x.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#define AUDIO_SAMPLE_RATE   (32040)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 50 + 1)

// #define FRAME_DOUBLE_BUFFERING
#define USE_AUDIO_TASK
// The audio is mixed on core 0 after each frame, then the audio task submits it on core 1: while it submits one
// buffer and one waits in its queue, the next frame's is mixed into the third
#define AUDIO_BUFFER_COUNT 3

// Drawing a busy scene's frame takes about twice the time the audio of a frame lasts, emulating one without drawing
// it about half. So a frame is drawn only when the audio already queued (estimated, see the end of the main loop)
// outlasts its drawing by AUDIO_LEAD_MARGIN (us, the ring plays in DMA buffers of 5.6 ms, and frames vary), else
// skipped, up to AUDIO_SKIP_MAX in a row. Past that it is drawn anyway, unless the audio would run out during its
// drawing: then skipped up to AUDIO_SKIP_LIMIT in a row (only when the CPU can't keep up even skipping, the sound
// gaps are then fewer, the picture slower). The retro-go frameskip still sets the most frames drawn
#define AUDIO_LEAD_MARGIN 20000
#define AUDIO_SKIP_MAX 5
#define AUDIO_SKIP_LIMIT (4 * AUDIO_SKIP_MAX)
// The I2S driver's DMA ring (see retro-go's drivers/audio/i2s.c, DMA_BUFFER_LEN must match)
#ifndef RG_AUDIO_DMA_BUFFER_COUNT
#define RG_AUDIO_DMA_BUFFER_COUNT 4
#endif
#define AUDIO_DMA_BUFFER_LEN 180

typedef struct
{
	char name[16];
	struct {
		uint16_t snes9x_mask;
		uint16_t local_mask;
		uint16_t mod_mask;
	} keys[16];
} keymap_t;

enum {
    KEYMAP_TYPE_A = 0,
    KEYMAP_TYPE_B,
    KEYMAP_TYPE_C,
    KEYMAP_REGULAR
};

static const keymap_t KEYMAPS[] = {
	[KEYMAP_TYPE_A] = {"Type A", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_START, 0},
		{SNES_Y_MASK, RG_KEY_SELECT, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	[KEYMAP_TYPE_B] = {"Type B", {
		{SNES_A_MASK, RG_KEY_START, 0},
		{SNES_B_MASK, RG_KEY_A, 0},
		{SNES_X_MASK, RG_KEY_SELECT, 0},
		{SNES_Y_MASK, RG_KEY_B, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	[KEYMAP_TYPE_C] = {"Type C", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, 0, 0},
		{SNES_Y_MASK, 0, 0},
		{SNES_TL_MASK, 0, 0},
		{SNES_TR_MASK, 0, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
    [KEYMAP_REGULAR] = {"Regular", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_X, 0},
		{SNES_Y_MASK, RG_KEY_Y, 0},
		{SNES_TL_MASK, RG_KEY_L, 0},
		{SNES_TR_MASK, RG_KEY_R, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
};

static const size_t KEYMAPS_COUNT = (sizeof(KEYMAPS) / sizeof(keymap_t));

static const char *SNES_BUTTONS[] = {
	"None", "None", "None", "None", "R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"
};

#define AUDIO_LOW_PASS_RANGE ((60 * 65536) / 100)

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_audio_sample_t *audioBuffers[AUDIO_BUFFER_COUNT];
static int audioBufferIndex;
static int samplesPerFrame;

#ifdef USE_AUDIO_TASK
static rg_task_t *audio_task_handle;
#endif

static bool sound_enabled = true;
static bool lowpass_filter = false;

static int keymap_id = 0;
static keymap_t keymap;

static const char *SETTING_KEYMAP = "keymap";
static const char *SETTING_SOUND_EMULATION = "apu";
static const char *SETTING_SOUND_FILTER = "filter";
// --- MAIN

static void update_keymap(int id)
{
    keymap_id = id % KEYMAPS_COUNT;
    keymap = KEYMAPS[keymap_id];
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    return S9xSaveState(filename);
}

static bool reset_handler(bool hard)
{
    S9xReset();
    return true;
}

// In-game saves: the cartridge's battery-backed SRAM, kept in <rom>.sram in the saves folder, the raw image as other
// emulators keep it (their .srm). It is read at every start, resume included, and the save states hold it too but
// loading one keeps the SRAM the game has (see load_state_handler): the battery file is what counts, as with a
// cartridge. The game writes it (CPU.SRAMModified, counted per frame in sram_dirty) and it goes to the card once the
// game has left it alone for a moment, a step per frame between frames (see the main loop), whole when a menu opens
// and at exit: a devkit is switched off by pulling the cable, so the save cannot wait for a clean exit. A save writes
// a copy taken when it starts, so the game can go on writing meanwhile, and a game that never leaves it alone (some
// use it as work RAM) is saved every SRAM_STALE_FRAMES all the same.
// A power loss at any point must leave one complete copy on the card. A save writes <rom>.sram.new, the image
// followed by a trailer with its size and CRC, then overwrites the .sram in place, then deletes the .new. At the
// start a .new with a valid trailer is the newer copy, and one without was cut short. Nothing is renamed: a power
// loss inside a FAT rename can leave both names on one cluster chain, and deleting either then frees the other's
// data. While the .sram is not complete (cut short, or a write failed) the .new is the only copy, so the next save
// goes straight to the .sram. From its end to the deletion of the .new, the next frame, the .new is the older copy:
// a power loss in that frame goes back to it, one save earlier. This is gbsp's design (gbsp/main/main.c).
static char *sram_path, *sram_new_path; // NULL: no saves (see sram_load)
static uint8_t *sram_copy;              // what a save writes, taken when it starts, or the game's while a state loads
static size_t sram_size;                // 0: the cartridge has none
static bool sram_file_ok = true;        // the .sram is complete, so the .new may be rewritten
static uint32_t sram_dirty;             // frames in which the game wrote to it
static uint32_t sram_saved;             // sram_dirty when the card last matched
static uint32_t sram_saved_crc;         // and what it holds, for the writes CPU.SRAMModified misses
static uint32_t sram_blank_crc;         // an SRAM the game has not written yet (see load_state_handler)
static int32_t sram_quiet;              // frames since the game wrote to it (negative: retry later)
static int32_t sram_unsaved;            // frames since the card matched it or a save ended
static int sram_failures;               // in a row, while the game runs
#define SRAM_QUIET_FRAMES 30            // the game is done writing
#define SRAM_URGENT_FRAMES 180          // no calm moment came: write anyway
#define SRAM_STALE_FRAMES 600           // the game never stops writing: save what it has
#define SRAM_CHUNK 8192                 // about 5 ms of SD writing
#define SRAM_MAGIC 0x56534E53           // "SNSV"

typedef struct { uint32_t magic, size, crc; } sram_trailer_t;

enum { SRAM_BUSY, SRAM_DONE, SRAM_FAILED };
enum { SAVE_IDLE, SAVE_NEW, SAVE_SRAM, SAVE_CLEAN };
static struct
{
    int phase;           // SAVE_IDLE, or the file a running save is at
    int fd;              // that file, once opened
    uint32_t gen;        // sram_dirty when it started
    uint32_t done, crc;
    uint8_t *buf;        // internal RAM: the SD driver writes a chunk in one go, from PSRAM a sector at a time
    int64_t start_us, step_max_us;
} ssave = {.phase = SAVE_IDLE, .fd = -1};

// Reads a save into Memory.SRAM: 1 if read, 0 if there is none (a .new without a valid trailer was cut short),
// -1 if it could not be read
static int sram_read(const char *path, bool is_new)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    size_t size = st.st_size;
    if (is_new)
    {
        // A .new is one this game wrote, its size is the SRAM's
        if (size != sram_size + sizeof(sram_trailer_t))
        {
            RG_LOGW("%s is %u bytes, not a save of this game's %u", path, (unsigned)size, (unsigned)sram_size);
            return 0;
        }
        size = sram_size;
    }
    else if (size == 0)
        return 0;
    // One from another emulator can be larger (or smaller): the SRAM is what the game sees of it
    size = RG_MIN(size, sram_size);

    for (int tries = 0; tries < 3; tries++) // an SD read error is no reason to drop a save
    {
        sram_trailer_t trailer;
        FILE *fp = fopen(path, "rb");
        const bool ok = fp && fread(Memory.SRAM, 1, size, fp) == size
                        && (!is_new || fread(&trailer, sizeof(trailer), 1, fp) == 1);
        if (fp)
            fclose(fp);
        if (!ok)
            continue;
        const uint32_t crc = is_new ? rg_crc32(0, Memory.SRAM, size) : 0;
        if (is_new && (trailer.magic != SRAM_MAGIC || trailer.size != size || trailer.crc != crc))
        {
            RG_LOGW("%s was cut short: trailer %08x %u %08x, data %u %08x", path, (unsigned)trailer.magic,
                    (unsigned)trailer.size, (unsigned)trailer.crc, (unsigned)size, (unsigned)crc);
            memset(Memory.SRAM, 0x60, SRAM_SIZE);
            return 0;
        }
        return 1;
    }
    memset(Memory.SRAM, 0x60, SRAM_SIZE);
    return -1;
}

// After LoadROM, which sets the SRAM size, and before a resume loads its state
static void sram_load(void)
{
    // Uninitialised until now, and a game sees this in an SRAM it has not written yet (snes9x's SRAMInitialValue)
    memset(Memory.SRAM, 0x60, SRAM_SIZE);
    sram_size = Memory.SRAMMask ? Memory.SRAMMask + 1 : 0;
    if (Memory.Map[0x700] == Memory.SRAM) // MapExtraRAM: the game sees all of it, as plain RAM (64 KB carts)
        sram_size = SRAM_SIZE;
    if (!sram_size)
    {
        RG_LOGI("The cartridge has no SRAM, no in-game saves");
        return;
    }
    sram_blank_crc = rg_crc32(0, Memory.SRAM, sram_size);
    sram_copy = rg_alloc(sram_size, MEM_SLOW);

    char *path = rg_emu_get_path(RG_PATH_SAVE_SRAM, app->romPath);
    char *new_path = malloc(strlen(path) + 5);
    if (!new_path)
        RG_PANIC("Out of memory");
    sprintf(new_path, "%s.new", path);
    char *dir = strdup(path); // not rg_dirname: it stops at 100 characters
    if (dir && strrchr(dir, '/'))
    {
        *strrchr(dir, '/') = 0;
        if (!rg_storage_mkdir(dir)) // the saves then fail, and say so
            RG_LOGE("Unable to create the save folder %s", dir);
    }
    free(dir);

    const int from_new = sram_read(new_path, true);
    const int from_sram = from_new > 0 ? 0 : sram_read(path, false);
    if (from_new < 0 || from_sram < 0)
    {
        // Playing on would write over the save the card holds
        RG_LOGE("Unable to read the save %s, in-game saves are off", from_new < 0 ? new_path : path);
        rg_gui_alert("In-game saves are off", "The save file could not be read from the SD card.");
        free(path);
        free(new_path);
    }
    else
    {
        sram_path = path;
        sram_new_path = new_path;
        if (from_new == 0)
            unlink(new_path); // none, or one that was cut short
        if (from_new > 0 || from_sram > 0)
            RG_LOGI("Save %s: %u bytes of SRAM", from_new > 0 ? new_path : path, (unsigned)sram_size);
        else
            RG_LOGI("No save %s yet, %u bytes of SRAM", path, (unsigned)sram_size);
    }
    sram_saved = sram_dirty;
    sram_saved_crc = rg_crc32(0, Memory.SRAM, sram_size);
    if (from_new > 0)
    {
        sram_file_ok = false; // the power went while it was written: write it again
        sram_saved--;
    }
}

static void sram_save_end(void)
{
    if (ssave.fd >= 0)
        close(ssave.fd);
    ssave.fd = -1;
    ssave.phase = SAVE_IDLE;
    free(ssave.buf);
    ssave.buf = NULL;
}

// One step of a save, a few ms: open a file, write a chunk, close it, or delete the .new. While the .sram is not
// complete the .new holds the save, so a save then goes straight to the .sram
static int sram_save_step(void)
{
    const int64_t t0 = rg_system_timer();
    int result = SRAM_BUSY;

    if (ssave.phase == SAVE_IDLE)
    {
        ssave.phase = sram_file_ok ? SAVE_NEW : SAVE_SRAM;
        ssave.gen = sram_dirty;
        memcpy(sram_copy, Memory.SRAM, sram_size);
        ssave.start_us = t0;
        ssave.step_max_us = 0;
    #ifdef ESP_PLATFORM
        ssave.buf = heap_caps_malloc(SRAM_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    #endif
    }
    const char *path = ssave.phase == SAVE_NEW ? sram_new_path : sram_path;

    if (ssave.phase == SAVE_CLEAN)
    {
        // It holds this same save, or the one before when this one went straight to the .sram: that one must go
        if (unlink(sram_new_path) != 0 && errno != ENOENT)
        {
            RG_LOGE("Unable to delete %s (%d)", sram_new_path, errno);
            result = SRAM_FAILED;
        }
        else
            result = SRAM_DONE;
    }
    else if (ssave.fd < 0)
    {
        ssave.done = 0;
        ssave.crc = 0;
        // The .sram in place, keeping its clusters (the .new holds the save meanwhile)
        ssave.fd = open(path, O_WRONLY | O_CREAT | (ssave.phase == SAVE_NEW ? O_TRUNC : 0), 0666);
        if (ssave.fd < 0)
        {
            RG_LOGE("Unable to open %s (%d)", path, errno);
            result = SRAM_FAILED;
        }
    }
    else if (ssave.done < sram_size)
    {
        const size_t n = RG_MIN(SRAM_CHUNK, sram_size - ssave.done);
        const uint8_t *src = sram_copy + ssave.done;
        if (ssave.buf)
            src = memcpy(ssave.buf, src, n);
        ssave.crc = rg_crc32(ssave.crc, src, n);
        const ssize_t w = write(ssave.fd, src, n);
        if (w != (ssize_t)n)
        {
            RG_LOGE("Unable to write %s: %d of %u bytes (%d)", path, (int)w, (unsigned)n, w < 0 ? errno : ENOSPC);
            result = SRAM_FAILED;
        }
        ssave.done += n;
    }
    else
    {
        bool ok = true;
        if (ssave.phase == SAVE_NEW)
        {
            const sram_trailer_t trailer = {SRAM_MAGIC, sram_size, ssave.crc};
            ok = write(ssave.fd, &trailer, sizeof(trailer)) == sizeof(trailer);
        }
        ok = fsync(ssave.fd) == 0 && ok;
        ok = close(ssave.fd) == 0 && ok;
        ssave.fd = -1;
        if (!ok)
        {
            RG_LOGE("Unable to write %s (%d)", path, errno);
            result = SRAM_FAILED;
        }
        else if (ssave.phase == SAVE_NEW)
        {
            sram_file_ok = false; // the .new holds the save now
            ssave.phase = SAVE_SRAM;
        }
        else
        {
            sram_file_ok = true;
            sram_saved_crc = ssave.crc;
            ssave.phase = SAVE_CLEAN;
        }
    }

    const int64_t t1 = rg_system_timer();
    if (ssave.step_max_us < t1 - t0)
        ssave.step_max_us = t1 - t0;
    if (result == SRAM_DONE)
    {
        sram_saved = ssave.gen;
        RG_LOGI("Saved %u bytes of SRAM in %d ms, longest step %d ms", (unsigned)sram_size,
                (int)((t1 - ssave.start_us) / 1000), (int)(ssave.step_max_us / 1000));
    }
    if (result != SRAM_BUSY)
        sram_save_end();
    return result;
}

// The whole save at once, when the game is not running. False if it failed
static bool sram_save_now(void)
{
    int result = SRAM_DONE;
    if (!sram_path)
        return true;
    if (ssave.phase != SAVE_IDLE) // one running: finish it, it writes the copy taken when it started
        while ((result = sram_save_step()) == SRAM_BUSY)
            continue;
    // A write CPU.SRAMModified does not see (the SRAM MapExtraRAM maps as plain RAM) shows here, one made since that
    // copy included
    if (sram_dirty == sram_saved && rg_crc32(0, Memory.SRAM, sram_size) != sram_saved_crc)
        sram_dirty++;
    if (sram_dirty != sram_saved)
        while ((result = sram_save_step()) == SRAM_BUSY)
            continue;
    if (result == SRAM_FAILED)
        return false;
    sram_failures = 0; // the card holds it: in-game failures count from here
    sram_unsaved = 0;
    return true;
}

static void sram_alert_failed(void)
{
    rg_gui_alert("In-game save failed", "The save could not be written to the SD card.");
}

static bool load_state_handler(const char *filename)
{
    // The state's SRAM is as old as the state, the game's is the battery file's or newer (see sram_load). Unless the
    // game's is blank: no save on the card or none that could be read, and the game has not written it yet. The
    // state's is then the game's, and goes to the card: the states made before the in-game saves hold the only copy,
    // and Resume loads one before the game starts. No save runs here, the menus finish one first
    const bool keep = sram_copy && rg_crc32(0, Memory.SRAM, sram_size) != sram_blank_crc;
    if (keep)
        memcpy(sram_copy, Memory.SRAM, sram_size);
    bool ret = S9xLoadState(filename);
    if (keep)
        memcpy(Memory.SRAM, sram_copy, sram_size);
    else if (sram_path && ssave.phase == SAVE_IDLE && rg_crc32(0, Memory.SRAM, sram_size) != sram_saved_crc)
        sram_dirty++;
    CPU.SRAMModified = false; // the state's
    return ret;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
    else if (event == RG_EVENT_SHUTDOWN)
    {
        if (!sram_save_now())
            sram_alert_failed();
    }
}

static rg_gui_event_t apu_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        sound_enabled = !sound_enabled;
        rg_settings_set_number(NS_APP, SETTING_SOUND_EMULATION, sound_enabled);
    }

    strcpy(option->value, sound_enabled ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t lowpass_filter_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        lowpass_filter = !lowpass_filter;
        rg_settings_set_number(NS_APP, SETTING_SOUND_FILTER, lowpass_filter);
    }

    strcpy(option->value, lowpass_filter ? _("On") : _("Off"));
    return RG_DIALOG_VOID;
}

static rg_gui_event_t change_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (event == RG_DIALOG_PREV && --keymap_id < 0)
            keymap_id = KEYMAPS_COUNT - 1;
        if (event == RG_DIALOG_NEXT && ++keymap_id > KEYMAPS_COUNT - 1)
            keymap_id = 0;
        update_keymap(keymap_id);
        rg_settings_set_number(NS_APP, SETTING_KEYMAP, keymap_id);
        return RG_DIALOG_REDRAW;
    }

    if (event == RG_DIALOG_ENTER)
    {
        return RG_DIALOG_CANCEL;
    }

    if (option->arg == -1)
    {
        strcat(strcat(strcpy(option->value, "< "), keymap.name), " >");
    }
    else if (option->arg >= 0)
    {
        int local_button = keymap.keys[option->arg].local_mask;
        int mod_button = keymap.keys[option->arg].mod_mask;
        int snes9x_button = log2(keymap.keys[option->arg].snes9x_mask); // convert bitmask to bit number

        if (snes9x_button < 4 || (local_button & (RG_KEY_UP|RG_KEY_DOWN|RG_KEY_LEFT|RG_KEY_RIGHT)))
        {
            option->flags = RG_DIALOG_FLAG_HIDDEN;
            return RG_DIALOG_VOID;
        }

        if (keymap.keys[option->arg].mod_mask)
            sprintf(option->value, "%s + %s", rg_input_get_key_name(mod_button), rg_input_get_key_name(local_button));
        else
            sprintf(option->value, "%s", rg_input_get_key_name(local_button));

        option->label = SNES_BUTTONS[snes9x_button];
        option->flags = RG_DIALOG_FLAG_NORMAL;
    }

    return RG_DIALOG_VOID;
}

static rg_gui_event_t menu_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {-1, _("Profile"), "-", RG_DIALOG_FLAG_NORMAL, &change_keymap_cb},
            {-2, "", NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {-3, "snes9x  ", "handheld", RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {1, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {2, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {3, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {4, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {5, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {6, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {7, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {8, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {9, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {10, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {11, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {12, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {13, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {14, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {15, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, keymap.name);
    return RG_DIALOG_VOID;
}

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = currentUpdate->data;
    GFX.SubScreen = malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED);
    GFX.ZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    GFX.SubZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
}

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0)
        return 0;

    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joypad = 0;

    for (int i = 0; i < RG_COUNT(keymap.keys); ++i)
    {
        uint32_t bitmask = keymap.keys[i].local_mask | keymap.keys[i].mod_mask;
        if (bitmask && bitmask == (joystick & bitmask))
        {
            joypad |= keymap.keys[i].snes9x_mask;
        }
    }

    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

// A frame's audio, into the next buffer. On core 0 between frames: the mixer reads and updates the sound state the
// SPC700 changes as it runs (key on and off, the echo buffer), mixing it on core 1 meanwhile glitched the sound
static rg_audio_sample_t *mix_samples(int32_t count)
{
    rg_audio_sample_t *buffer = audioBuffers[audioBufferIndex];
    audioBufferIndex = (audioBufferIndex + 1) % AUDIO_BUFFER_COUNT;
    if (!sound_enabled)
        memset(buffer, 0, count * sizeof(rg_audio_sample_t));
    else if (lowpass_filter)
        S9xMixSamplesLowPass((int16_t *)buffer, count << 1, AUDIO_LOW_PASS_RANGE);
    else
        S9xMixSamples((int16_t *)buffer, count << 1);
    return buffer;
}

#ifdef USE_AUDIO_TASK
static void audio_task(void *arg)
{
    rg_task_msg_t msg;
    while (rg_task_receive(&msg, -1))
    {
        if (msg.type == RG_TASK_MSG_STOP)
            break;
        rg_audio_submit(msg.dataPtr, samplesPerFrame);
    }
}
#endif

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &apu_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio filter"), "-", RG_DIALOG_FLAG_NORMAL, &lowpass_filter_cb};
    *dest++ = (rg_gui_option_t){0, _("Controls"),     "-", RG_DIALOG_FLAG_NORMAL, &menu_keymap_cb};
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void app_main(void)
{
    const rg_config_t config = {
        .sampleRate = AUDIO_SAMPLE_RATE,
        .frameRate = 60, // Will be adjusted later if a PAL ROM is loaded
        .storageRequired = true,
        .romRequired = true,
        .handlers.loadState = &load_state_handler,
        .handlers.saveState = &save_state_handler,
        .handlers.reset = &reset_handler,
        .handlers.screenshot = &screenshot_handler,
        .handlers.event = &event_handler,
        .handlers.options = &options_handler,
        .mallocAlwaysInternal = 0x10000,
    };
    app = rg_system_init(&config);

    // Load settings
    sound_enabled = rg_settings_get_number(NS_APP, SETTING_SOUND_EMULATION, 1);
    lowpass_filter = rg_settings_get_number(NS_APP, SETTING_SOUND_FILTER, 0);
    int default_keymap = (rg_input_key_is_present(RG_KEY_X|RG_KEY_Y|RG_KEY_L|RG_KEY_R)) ? KEYMAP_REGULAR : KEYMAP_TYPE_A;
    update_keymap(rg_settings_get_number(NS_APP, SETTING_KEYMAP, default_keymap));

    // Allocate surfaces and audio buffers
    updates[0] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, 0);
    updates[0]->height = SNES_HEIGHT;
#ifdef FRAME_DOUBLE_BUFFERING
    updates[1] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, 0);
    updates[1]->height = SNES_HEIGHT;
#else
    updates[1] = updates[0];
#endif
    currentUpdate = updates[0];

    for (int i = 0; i < AUDIO_BUFFER_COUNT; ++i)
        audioBuffers[i] = (rg_audio_sample_t *)calloc(AUDIO_BUFFER_LENGTH, 4);

    if (!updates[0] || !updates[1] || !audioBuffers[0] || !audioBuffers[1] || !audioBuffers[2])
        RG_PANIC("Failed to allocate buffers!");

#ifdef USE_AUDIO_TASK
    // Set up multicore audio. Above the display task's priority, on the same core: the audio cannot wait
    audio_task_handle = rg_task_create("snes_audio", &audio_task, NULL, 4096, 1, RG_TASK_PRIORITY_7, 1);
    RG_ASSERT(audio_task_handle, "Failed to create audio task!");
#endif

    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = true;

    if (!S9xInitDisplay())
        RG_PANIC("Display init failed!");

    if (!S9xInitMemory())
        RG_PANIC("Memory init failed!");

    if (!S9xInitAPU())
        RG_PANIC("APU init failed!");

    if (!S9xInitSound(0, 0))
        RG_PANIC("Sound init failed!");

    if (!S9xInitGFX())
        RG_PANIC("Graphics init failed!");

    S9xSetPlaybackRate(Settings.SoundPlaybackRate);

    const char *filename = app->romPath;

    if (rg_extension_match(filename, "zip"))
    {
        if (!rg_storage_unzip_file(filename, NULL, (void **)&Memory.ROM, &Memory.ROM_AllocSize, RG_FILE_USER_BUFFER))
            RG_PANIC("ROM file unzipping failed!");
        filename = NULL;
    }

    if (!LoadROM(filename))
        RG_PANIC("ROM loading failed!");

    sram_load();

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    rg_system_set_tick_rate(Memory.ROMFramesPerSecond);
    app->frameskip = 3;

    samplesPerFrame = (int)roundf((float)app->sampleRate / app->tickRate);
    bool menuCancelled = false;
    bool menuPressed = false;
    int skipFrames = 0;
    int64_t audioWait = 0; // how long the last frame's audio waited for the audio task: the audio is well ahead
    int sramPoll = 0;
    // Once a second on the console: what a frame costs, and whether the audio kept up. Not the second after the
    // start or a menu: the audio events of the time the game did not run are counted in it
    rg_audio_counters_t statsAudio = rg_audio_get_counters();
    int64_t statsStart = rg_system_timer(), statsSave = 0, statsEmu = 0, statsMix = 0, statsDisplay = 0, statsWait = 0;
    int64_t statsLead = INT64_MAX;
    int statsFrames = 0, statsDrawn = 0;
    bool statsShow = false;
    // The audio queued when the last frame ended, at leadTime (us, see the end of the loop). The most: when a send
    // waits, the audio task waits for room in the DMA ring, all its buffers but the one playing are full, the task
    // holds a frame's audio and the queue the one sent. Without the task the submit itself waited
#ifdef USE_AUDIO_TASK
    const int leadFullFrames = (RG_AUDIO_DMA_BUFFER_COUNT - 1) * AUDIO_DMA_BUFFER_LEN + 2 * samplesPerFrame;
#else
    const int leadFullFrames = (RG_AUDIO_DMA_BUFFER_COUNT - 1) * AUDIO_DMA_BUFFER_LEN;
#endif
    int64_t audioLead = 0, leadTime = rg_system_timer();
    int64_t drawCost = app->frameTime; // the work of a drawn frame: the last one's, or a recent slower one's
    int skipped = 0; // frames skipped in a row

    while (1)
    {
        const int64_t startTime = rg_system_timer();
        uint32_t joystick = rg_input_read_gamepad();
        bool drawFrame = (skipFrames == 0);
        bool slowFrame = false;

        if (menuPressed && !(joystick & RG_KEY_MENU))
        {
            if (!menuCancelled)
            {
                rg_task_delay(50);
                if (!sram_save_now()) // the menus can quit
                    sram_alert_failed();
                rg_gui_game_menu();
                menuPressed = false;
                menuCancelled = false;
                statsStart = rg_system_timer();
                statsShow = false;
                continue;
            }
            menuCancelled = false;
        }
        else if (joystick & RG_KEY_OPTION)
        {
            if (!sram_save_now())
                sram_alert_failed();
            rg_gui_options_menu();
            statsStart = rg_system_timer();
            statsShow = false;
            continue;
        }

        menuPressed = joystick & RG_KEY_MENU;

        if (menuPressed && (joystick & ~RG_KEY_MENU))
        {
            menuCancelled = true;
        }

        // In-game saves (see sram_load): once the game has stopped writing, or has gone on for SRAM_STALE_FRAMES, a
        // step per frame, here between frames, when the audio is well ahead (an SD write takes a few ms) or no such
        // moment came for a while
        const bool sramDue = sram_quiet >= SRAM_QUIET_FRAMES || sram_unsaved >= SRAM_STALE_FRAMES;
        const bool sramUrgent = sram_quiet >= SRAM_URGENT_FRAMES
                                || sram_unsaved >= SRAM_STALE_FRAMES + SRAM_URGENT_FRAMES;
        if (sram_path && (ssave.phase != SAVE_IDLE || (sram_dirty != sram_saved && sramDue))
            && (ssave.phase == SAVE_CLEAN || sramUrgent || audioWait > 2000))
        {
            const int result = sram_save_step();
            if (result != SRAM_BUSY)
                sram_unsaved = 0; // what the game wrote meanwhile goes in the next save
            if (result == SRAM_DONE)
                sram_failures = 0;
            else if (result == SRAM_FAILED)
            {
                sram_quiet = -10 * 60; // try again in 10 s
                if (++sram_failures == 3) // failing for half a minute: say so, once
                {
                    sram_alert_failed();
                    statsStart = rg_system_timer();
                    statsShow = false;
                }
            }
        }
        // A write CPU.SRAMModified does not see (the SRAM MapExtraRAM maps as plain RAM): once a second when the audio
        // is well ahead, else every 10 s. Nothing counts those writes, so whether the game is done with them is not
        // known either: such a change is saved as one by a game that never stops writing, SRAM_STALE_FRAMES later
        else if (sram_path && sram_dirty == sram_saved && ++sramPoll >= (audioWait > 2000 ? 60 : 600))
        {
            sramPoll = 0;
            if (rg_crc32(0, Memory.SRAM, sram_size) != sram_saved_crc)
            {
                sram_dirty++;
                sram_quiet = RG_MIN(sram_quiet, SRAM_QUIET_FRAMES - SRAM_STALE_FRAMES);
            }
        }

        // Here, after any save step: the audio queued now (see AUDIO_LEAD_MARGIN). With the sound off only silence is
        // queued, and nothing paces the frames but the frameskip, as before
        if (sound_enabled && drawFrame)
        {
            const int64_t lead = audioLead - (rg_system_timer() - leadTime);
            if (lead - drawCost < AUDIO_LEAD_MARGIN
                && (skipped < AUDIO_SKIP_MAX || (lead < drawCost && skipped < AUDIO_SKIP_LIMIT)))
                drawFrame = false;
        }

        IPPU.RenderThisFrame = drawFrame;
        GFX.Screen = currentUpdate->data;

        const int64_t emuStart = rg_system_timer();
        S9xMainLoop();
        const int64_t emuEnd = rg_system_timer();

        // The core flags each write to the SRAM, the frontend counts the frames with one
        if (CPU.SRAMModified)
        {
            CPU.SRAMModified = false;
            sram_dirty++;
            sram_quiet = RG_MIN(sram_quiet, 0);
        }
        else if (sram_quiet < SRAM_URGENT_FRAMES)
            sram_quiet++;
        if (sram_dirty == sram_saved)
            sram_unsaved = 0;
        else if (sram_unsaved < SRAM_STALE_FRAMES + SRAM_URGENT_FRAMES)
            sram_unsaved++;

        rg_audio_sample_t *audio = mix_samples(samplesPerFrame);
        const int64_t mixEnd = rg_system_timer();

        if (drawFrame)
        {
            slowFrame = rg_display_is_busy();
            rg_display_submit(currentUpdate, 0);
            currentUpdate = updates[currentUpdate == updates[0]];
        }

        const int64_t workEnd = rg_system_timer();
        rg_system_tick(workEnd - startTime);
        const bool sent = sound_enabled || app->frameTime - (workEnd - startTime) > 2000;
        bool full = false; // the send waited for room (see the end of the loop)
        if (sent)
        {
        #ifdef USE_AUDIO_TASK
            // Not from how long it took: another task can hold this one up as long. The queue still holds the last
            // frame's audio only while the audio task waits for room in the DMA ring
            rg_task_msg_t msg = {.type = 0, .dataPtr = audio};
            full = !rg_task_send(audio_task_handle, &msg, 0);
            if (full)
                rg_task_send(audio_task_handle, &msg, -1);
        #else
            rg_audio_submit(audio, samplesPerFrame);
        #endif
        }
        const int64_t frameEnd = rg_system_timer();
        audioWait = frameEnd - workEnd;
    #ifndef USE_AUDIO_TASK
        full = sent && audioWait > 1000;
    #endif

        // The audio queued (see AUDIO_LEAD_MARGIN): the last frame's, less what played since, plus this frame's, or
        // the most when the send waited for room. Ran dry, or since a menu (which silences it): this frame's
        const int64_t leadLeft = audioLead - (frameEnd - leadTime);
        const int64_t audioRate = (int64_t)(AUDIO_SAMPLE_RATE * app->speed + 0.5f); // frames per s, the Speed option's
        const int64_t leadFull = leadFullFrames * 1000000LL / audioRate;
        if (full)
            audioLead = leadFull;
        else if (sent)
            audioLead = RG_MIN(RG_MAX(leadLeft, 0) + samplesPerFrame * 1000000LL / audioRate, leadFull);
        else
            audioLead = RG_MAX(leadLeft, 0);
        leadTime = frameEnd;

        statsFrames++;
        statsDrawn += drawFrame;
        statsSave += emuStart - startTime;
        statsEmu += emuEnd - emuStart;
        statsMix += mixEnd - emuEnd;
        statsDisplay += workEnd - mixEnd;
        statsWait += audioWait;
        statsLead = RG_MIN(statsLead, leadLeft);
        if (frameEnd - statsStart >= 1000000)
        {
            const rg_audio_counters_t audioNow = rg_audio_get_counters();
            if (statsShow)
                RG_LOGI("%d frames, %d drawn, frameskip %d, in us per frame: emu %d, mix %d, display %d, save %d, "
                        "audio wait %d. Audio: least queued %d ms, %d underruns, %d full waits", statsFrames,
                        statsDrawn, app->frameskip, (int)(statsEmu / statsFrames), (int)(statsMix / statsFrames),
                        (int)(statsDisplay / statsFrames), (int)(statsSave / statsFrames),
                        (int)(statsWait / statsFrames), (int)(statsLead / 1000),
                        (int)(audioNow.underruns - statsAudio.underruns),
                        (int)(audioNow.fullWaits - statsAudio.fullWaits));
            statsShow = true;
            statsAudio = audioNow;
            statsStart = frameEnd;
            statsFrames = statsDrawn = 0;
            statsSave = statsEmu = statsMix = statsDisplay = statsWait = 0;
            statsLead = INT64_MAX;
        }

        // After a drawn frame, the frames to skip at least (see AUDIO_LEAD_MARGIN for the others)
        if (drawFrame)
        {
            skipped = 0;
            // At most what still leaves the margin with the ring full: one stall can't hold the drawing back for long
            const int64_t cost = RG_MIN(workEnd - emuStart, leadFull - AUDIO_LEAD_MARGIN);
            drawCost = cost > drawCost ? cost : drawCost + (cost - drawCost) / 8;
            int elapsed = rg_system_timer() - startTime;
            if (app->frameskip > 0)
                skipFrames = app->frameskip;
            else if (elapsed > app->frameTime + 1500) // Allow some jitter
                skipFrames = 1; // (elapsed / frameTime)
            else if (slowFrame)
                skipFrames = 1;
        }
        else
        {
            skipped++;
            if (skipFrames > 0)
                skipFrames--;
        }
    }
}
