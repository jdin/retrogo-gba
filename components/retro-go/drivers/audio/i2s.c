#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_INT_DAC || RG_AUDIO_USE_EXT_DAC

#ifndef ESP_PLATFORM
#error "I2S support can only be built inside esp-idf!"
#elif !CONFIG_IDF_TARGET_ESP32 && RG_AUDIO_USE_INT_DAC
#error "Your chip has no DAC! Please set RG_AUDIO_USE_INT_DAC to 0 in your target file."
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <driver/gpio.h>
#include <driver/i2s.h>

#ifdef RG_GPIO_SND_AMP_ENABLE_INVERT
#define MUTE_ENABLE 1
#define MUTE_DISABLE 0
#else
#define MUTE_ENABLE 0
#define MUTE_DISABLE 1
#endif

// Most applications submit at most 640 audio frames per call to driver_submit (32000/50); gbsp
// stretches a slow frame's audio to up to a few thousand, and a submit larger than the ring simply
// blocks until the DMA has played the rest. Using a single large buffer risks blocking the call
// needlessly because some apps submit more than once per cycle or there could be occasional jitter
// (early submission). An app whose frame time varies a lot can ask for more lead with
// RG_AUDIO_DMA_BUFFER_COUNT.
#ifndef RG_AUDIO_DMA_BUFFER_COUNT
#define RG_AUDIO_DMA_BUFFER_COUNT 4
#endif
#define DMA_BUFFER_COUNT RG_AUDIO_DMA_BUFFER_COUNT
#define DMA_BUFFER_LEN 180 // gbsp/main/main.c's AUDIO_DMA_BUFFER_LEN must match
// The driver posts an event per DMA buffer played, ~3 per 60 Hz frame. 32 hold more than the whole
// ring's worth between two submissions, so an underrun event isn't dropped for lack of space
#define EVENT_QUEUE_LEN 32

static struct {
    const char *last_error;
    int device;
    int volume;
    bool muted;
    QueueHandle_t events;
    int64_t full_waits; // Never reset, the caller compares them between submissions
    int64_t underruns;
    size_t tail; // Frames written into the DMA buffer being filled, 0 at a buffer boundary
} state;

static const rg_audio_frame_t silence[DMA_BUFFER_LEN];

static bool driver_init(int device, int sample_rate)
{
    state.last_error = NULL;
    state.device = device;
    state.events = NULL;
    state.tail = 0;

    if (state.device == 0)
    {
    #if RG_AUDIO_USE_INT_DAC
        esp_err_t ret = i2s_driver_install(I2S_NUM_0, &(i2s_config_t){
            .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN,
            .sample_rate = sample_rate,
            .bits_per_sample = 16,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_MSB,
            .intr_alloc_flags = 0, // ESP_INTR_FLAG_LEVEL1
            .dma_buf_count = DMA_BUFFER_COUNT,
            .dma_buf_len = DMA_BUFFER_LEN,
        }, EVENT_QUEUE_LEN, &state.events);
        if (ret == ESP_OK)
            ret = i2s_set_dac_mode(RG_AUDIO_USE_INT_DAC);
        if (ret != ESP_OK)
            state.last_error = esp_err_to_name(ret);
    #else
        state.last_error = "This device does not support internal DAC mode!";
    #endif
    }
    else if (state.device == 1)
    {
    #if RG_AUDIO_USE_EXT_DAC
        esp_err_t ret = i2s_driver_install(I2S_NUM_0, &(i2s_config_t){
            .mode = I2S_MODE_MASTER | I2S_MODE_TX,
            .sample_rate = sample_rate,
            .bits_per_sample = 16,
            .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
            .communication_format = I2S_COMM_FORMAT_STAND_I2S,
            .intr_alloc_flags = 0, // ESP_INTR_FLAG_LEVEL1
            .dma_buf_count = DMA_BUFFER_COUNT,
            .dma_buf_len = DMA_BUFFER_LEN,
            .tx_desc_auto_clear = true, // On underrun play silence, not the stale buffers in a loop
        #if CONFIG_IDF_TARGET_ESP32
            .use_apll = true, // External DAC may care about accuracy
        #endif
        }, EVENT_QUEUE_LEN, &state.events);
        if (ret == ESP_OK)
        {
            ret = i2s_set_pin(I2S_NUM_0, &(i2s_pin_config_t) {
                .mck_io_num = GPIO_NUM_NC,
                .bck_io_num = RG_GPIO_SND_I2S_BCK,
                .ws_io_num = RG_GPIO_SND_I2S_WS,
                .data_out_num = RG_GPIO_SND_I2S_DATA,
                .data_in_num = GPIO_NUM_NC
            });
        }
        if (ret != ESP_OK)
            state.last_error = esp_err_to_name(ret);
    #else
        state.last_error = "This device does not support external DAC mode!";
    #endif
    }
    #ifdef RG_GPIO_SND_AMP_ENABLE
        gpio_reset_pin(RG_GPIO_SND_AMP_ENABLE);
        gpio_set_level(RG_GPIO_SND_AMP_ENABLE, MUTE_ENABLE);
        gpio_set_direction(RG_GPIO_SND_AMP_ENABLE, GPIO_MODE_OUTPUT);
    #endif
    return state.last_error == NULL;
}

static bool driver_set_sample_rates(int sampleRate)
{
    if (i2s_set_sample_rates(I2S_NUM_0, sampleRate) != ESP_OK)
        return false;
    state.tail = 0; // It restarts the DMA, and the next write takes a fresh buffer
    return true;
}

static bool driver_deinit(void)
{
    i2s_driver_uninstall(I2S_NUM_0); // Also deletes the event queue
    state.events = NULL;
    if (state.device == 0)
    {
    #if RG_AUDIO_USE_INT_DAC
        i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    #endif
    }
    else if (state.device == 1)
    {
    #if RG_AUDIO_USE_EXT_DAC
        gpio_reset_pin(RG_GPIO_SND_I2S_BCK);
        gpio_reset_pin(RG_GPIO_SND_I2S_DATA);
        gpio_reset_pin(RG_GPIO_SND_I2S_WS);
    #endif
    }
    #ifdef RG_GPIO_SND_AMP_ENABLE
    gpio_reset_pin(RG_GPIO_SND_AMP_ENABLE);
    #endif
    return true;
}

// TX_Q_OVF: the DMA finished a buffer while no other one had been filled, the audio ran out
static bool driver_take_underruns(void)
{
    bool ran_out = false;
    i2s_event_t event;
    while (state.events && xQueueReceive(state.events, &event, 0) == pdTRUE)
    {
        if (event.type == I2S_EVENT_TX_Q_OVF)
        {
            state.underruns++;
            ran_out = true;
        }
    }
    return ran_out;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    float volume = state.muted ? 0.f : (state.volume * 0.01f);
    bool use_internal_dac = state.device == 0;
    rg_audio_frame_t buffer[DMA_BUFFER_LEN];
    bool waited = false;
    size_t pos = 0;

    // Ran out since the last submission: the DMA has played the buffer being filled as far as it was written, and
    // cleared it. Its rest is no longer next in the ring, what went there would play out of order, a lap later, or be
    // overwritten. Finish it with silence (never blocks, it is free): the audio resumes in a fresh buffer, in order
    if (driver_take_underruns() && state.tail)
    {
        size_t written = 0;
        i2s_write(I2S_NUM_0, silence, (DMA_BUFFER_LEN - state.tail) * 4, &written, 0);
        state.tail = (state.tail + written / 4) % DMA_BUFFER_LEN;
    }

    for (size_t i = 0; i < count; ++i)
    {
        int left = frames[i].left * volume;
        int right = frames[i].right * volume;

        if (use_internal_dac)
        {
        #if RG_AUDIO_USE_INT_DAC == 1
            left = ((left + right) >> 1) + 0x8000; // the internal DAC expects unsigned data
            right = 0;
        #elif RG_AUDIO_USE_INT_DAC == 2
            left = 0;
            right = ((left + right) >> 1) + 0x8000; // the internal DAC expects unsigned data
        #elif RG_AUDIO_USE_INT_DAC == 3
            // In two channel mode we use left and right as a differential mono output to increase resolution.
            int sample = (left + right) >> 1;
            if (sample > 0x7F00)
            {
                left = 0x8000 + (sample - 0x7F00);
                right = -0x8000 + 0x7F00;
            }
            else if (sample < -0x7F00)
            {
                left = 0x8000 + (sample + 0x7F00);
                right = -0x8000 + -0x7F00;
            }
            else
            {
                left = 0x8000;
                right = -0x8000 + sample;
            }
        #endif
        }

        // Clipping   (not necessary, we have (int16 * vol) and volume is never more than 1.0)
        // if (left > 32767) left = 32767; else if (left < -32768) left = -32767;
        // if (right > 32767) right = 32767; else if (right < -32768) right = -32767;

        // Queue
        buffer[pos].left = left;
        buffer[pos].right = right;

        // Advance pos first: the last frame must be counted before the final write
        if (++pos == RG_COUNT(buffer) || i == count - 1)
        {
            size_t written = 0, more = 0;
            // Try without a timeout first: a short write means that every DMA buffer was full
            esp_err_t ret = i2s_write(I2S_NUM_0, (void *)buffer, pos * 4, &written, 0);
            if (ret == ESP_OK && written < pos * 4)
            {
                waited = true;
                ret = i2s_write(I2S_NUM_0, (char *)buffer + written, pos * 4 - written, &more, 1000);
                written += more;
            }
            // The legacy driver returns ESP_OK on timeout, so check the length too
            if (ret != ESP_OK || written != pos * 4)
                RG_LOGW("I2S Submission error! Written: %d/%d\n", (int)written, (int)(pos * 4));
            state.tail = (state.tail + written / 4) % DMA_BUFFER_LEN;
            pos = 0;
        }
    }
    if (waited)
        state.full_waits++;

    // Counted now too: the caller sees at once that the ring ran out during this submission. Its writes came after
    // that and are in order, nothing to finish (unless it ran out in the few us before the first one)
    driver_take_underruns();
    return true;
}

static bool driver_set_mute(bool mute)
{
    if (mute && state.tail)
    {
        // Finish the buffer being written with silence: the first submit after the menu then
        // starts a fresh one, so an app that refills the ring then knows how much it holds.
        // Never blocks, the rest of that buffer is free.
        size_t written = 0;
        i2s_write(I2S_NUM_0, silence, (DMA_BUFFER_LEN - state.tail) * 4, &written, 0);
        state.tail = (state.tail + written / 4) % DMA_BUFFER_LEN;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    #ifdef RG_GPIO_SND_AMP_ENABLE
    gpio_set_level(RG_GPIO_SND_AMP_ENABLE, mute ? MUTE_ENABLE : MUTE_DISABLE);
    #endif
    state.muted = mute;
    return true;
}

static bool driver_set_volume(int volume)
{
    state.volume = volume;
    return true;
}

static const char *driver_get_error(void)
{
    return state.last_error;
}

static void driver_get_counters(rg_audio_counters_t *counters)
{
    counters->fullWaits = state.full_waits;
    counters->underruns = state.underruns;
}

const rg_audio_driver_t rg_audio_driver_i2s = {
    .name = "i2s",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
    .set_mute = driver_set_mute,
    .set_volume = driver_set_volume,
    .set_sample_rate = driver_set_sample_rates,
    .get_error = driver_get_error,
    .get_counters = driver_get_counters,
};

#endif // RG_AUDIO_USE_INT_DAC || RG_AUDIO_USE_EXT_DAC
