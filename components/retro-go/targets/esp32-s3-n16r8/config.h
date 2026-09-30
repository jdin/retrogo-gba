/****************************************************************************
 * Target definition for ESP32-S3-N16R8                                     *
 * ESP32-S3-WROOM-1 N16R8 (16MB quad flash + 8MB octal PSRAM)               *
 * 2.8" 320x240 TFT SPI display driven by an ST7789(V) controller           *
 ****************************************************************************/
#define RG_TARGET_NAME             "ESP32-S3-N16R8"


/****************************************************************************
 * Status LED                                                               *
 ****************************************************************************/
#define RG_LED_DRIVER               1   // 1 = GPIO
#define RG_GPIO_LED                 GPIO_NUM_38
// #define RG_GPIO_LED_INVERT          // Uncomment if the LED is active LOW


/****************************************************************************
 * Storage                                                                  *
 ****************************************************************************/
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT
#define RG_GPIO_SDSPI_MISO          GPIO_NUM_9
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_11
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_13
#define RG_GPIO_SDSPI_CS            GPIO_NUM_10
// #define RG_STORAGE_FLASH_PARTITION  "vfs"


/****************************************************************************
 * Audio                                                                    *
 ****************************************************************************/
#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_EXT_DAC        1   // 0 = Disable, 1 = Enable
#define RG_AUDIO_USE_BUZZER_PIN     0   // See drivers/audio/buzzer.c for details
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_41
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_42
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_40
// #define RG_GPIO_SND_AMP_ENABLE      GPIO_NUM_18


/****************************************************************************
 * Video                                                                    *
 ****************************************************************************/
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_40M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATION          3   // Possible values are 0-7 (you'll have to experiment)
#define RG_SCREEN_RGB_BGR           0   // Possible values are 0-1 (change if colors are bad)
#define RG_SCREEN_PIXEL_FORMAT      0   // Possible values are 0=565_BE, 1=565_LE
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0} // left, top, right, bottom
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0} // left, top, right, bottom
#define RG_SCREEN_PARTIAL_UPDATES   1

// The driver emits COLMOD and MADCTL (from RG_SCREEN_ROTATION / RG_SCREEN_RGB_BGR) before this
// runs, and SLPOUT + DISPON after, so only the ST7789 specific tuning belongs here.
#define RG_SCREEN_INIT()                                                                                   \
    ILI9341_CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);  /* PORCTRL: porch setting */                         \
    ILI9341_CMD(0xB7, 0x35);                          /* GCTRL: gate control (VGH 13.26V, VGL -10.43V) */  \
    ILI9341_CMD(0xBB, 0x28);                          /* VCOMS: VCOM setting (1.1V) */                     \
    ILI9341_CMD(0xC0, 0x2C);                          /* LCMCTRL: LCM control */                           \
    ILI9341_CMD(0xC2, 0x01);                          /* VDVVRHEN: VDV & VRH from command */               \
    ILI9341_CMD(0xC3, 0x0B);                          /* VRHS: VAP 4.1V / VAN -4.1V */                     \
    ILI9341_CMD(0xC4, 0x20);                          /* VDVS: VDV setting (0V) */                         \
    ILI9341_CMD(0xC6, 0x0F);                          /* FRCTRL2: 60Hz frame rate in normal mode */        \
    ILI9341_CMD(0xD0, 0xA4, 0xA1);                    /* PWCTRL1: power control (AVDD 6.8V, AVCL -4.8V) */ \
    ILI9341_CMD(0xE0, 0xD0, 0x01, 0x08, 0x0F, 0x11, 0x2A, 0x36, 0x55, 0x44, 0x3A, 0x0B, 0x06, 0x11, 0x20); \
    ILI9341_CMD(0xE1, 0xD0, 0x02, 0x07, 0x0A, 0x0B, 0x18, 0x34, 0x43, 0x4A, 0x2B, 0x1B, 0x1C, 0x22, 0x1F); \
    ILI9341_CMD(0x21);                                /* INVON: ST7789 panels need inversion on */         \
    ILI9341_CMD(0x13);                                /* NORON: normal display mode on */
#define RG_SCREEN_DEINIT() \
    /* Nothing to do */
#define RG_GPIO_LCD_MISO            GPIO_NUM_NC
#define RG_GPIO_LCD_MOSI            GPIO_NUM_12
#define RG_GPIO_LCD_CLK             GPIO_NUM_48
#define RG_GPIO_LCD_CS              GPIO_NUM_NC
#define RG_GPIO_LCD_DC              GPIO_NUM_47
#define RG_GPIO_LCD_BCKL            GPIO_NUM_39
// #define RG_GPIO_LCD_BCKL_INVERT     // Uncomment if the LED is active LOW
#define RG_GPIO_LCD_RST             GPIO_NUM_3


/****************************************************************************
 * Input                                                                    *
 ****************************************************************************/
// Refer to rg_input.h to see all available RG_KEY_* and RG_GAMEPAD_*_MAP types
#define RG_GAMEPAD_ADC_MAP {\
    {RG_KEY_UP,    ADC_UNIT_1, ADC_CHANNEL_5, ADC_ATTEN_DB_11, 3072, 4096},\
    {RG_KEY_RIGHT, ADC_UNIT_1, ADC_CHANNEL_6, ADC_ATTEN_DB_11, 1024, 3071},\
    {RG_KEY_DOWN,  ADC_UNIT_1, ADC_CHANNEL_5, ADC_ATTEN_DB_11, 1024, 3071},\
    {RG_KEY_LEFT,  ADC_UNIT_1, ADC_CHANNEL_6, ADC_ATTEN_DB_11, 3072, 4096},\
}
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_SELECT, .num = GPIO_NUM_16, .pullup = 1, .level = 0},\
    {RG_KEY_START,  .num = GPIO_NUM_17, .pullup = 1, .level = 0},\
    {RG_KEY_MENU,   .num = GPIO_NUM_18, .pullup = 1, .level = 0},\
    {RG_KEY_OPTION, .num = GPIO_NUM_8,  .pullup = 1, .level = 0},\
    {RG_KEY_A,      .num = GPIO_NUM_15, .pullup = 1, .level = 0},\
    {RG_KEY_B,      .num = GPIO_NUM_5,  .pullup = 1, .level = 0},\
    {RG_KEY_X,      .num = GPIO_NUM_21, .pullup = 1, .level = 0},\
    {RG_KEY_Y,      .num = GPIO_NUM_14, .pullup = 1, .level = 0},\
    {RG_KEY_L,      .num = GPIO_NUM_1,  .pullup = 1, .level = 0},\
    {RG_KEY_R,      .num = GPIO_NUM_2,  .pullup = 1, .level = 0},\
}


/****************************************************************************
 * Battery                                                                  *
 ****************************************************************************/
#define RG_BATTERY_DRIVER           1
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_3
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3500.f) / (4200.f - 3500.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)
