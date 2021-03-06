#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "driver/i2s.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_ota_ops.h"

#include "../components/smsplus/shared.h"

#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_sdcard.h"

#include "hourglass_empty_black_48dp.h"

#include <dirent.h>



#define AUDIO_SAMPLE_RATE (32000)

uint8_t* framebuffer[2];
int currentFramebuffer = 0;

uint32_t* audioBuffer = NULL;
int audioBufferCount = 0;

spi_flash_mmap_handle_t hrom;

QueueHandle_t vidQueue;
TaskHandle_t videoTaskHandle;

odroid_volume_level Volume;
odroid_battery_state battery;

bool scaling_enabled = true;
bool previous_scaling_enabled = true;

volatile bool videoTaskIsRunning = false;
void videoTask(void *arg)
{
    uint8_t* param;

    videoTaskIsRunning = true;

    while(1)
    {
        xQueuePeek(vidQueue, &param, portMAX_DELAY);

        if (param == 1)
            break;

        if (previous_scaling_enabled != scaling_enabled)
        {
            ili9341_write_frame_sms(NULL, NULL, cart.type != TYPE_SMS, false);
            previous_scaling_enabled = scaling_enabled;
        }

        ili9341_write_frame_sms(param, bitmap.pal.color, cart.type != TYPE_SMS, scaling_enabled);

        odroid_input_battery_level_read(&battery);

        xQueueReceive(vidQueue, &param, portMAX_DELAY);
    }


    // Draw hourglass
    send_reset_drawing((320 / 2 - 48 / 2), 96, 48, 48);

    // split in half to fit transaction size limit
    uint16_t* icon = image_hourglass_empty_black_48dp.pixel_data;

    send_continue_line(icon, 48, 24);
    send_continue_line(icon + 24 * 48, 48, 24);

    send_continue_wait();

    videoTaskIsRunning = false;
    vTaskDelete(NULL);

    while (1) {}
}

//Read an unaligned byte.
char unalChar(const unsigned char *adr) {
	//See if the byte is in memory that can be read unaligned anyway.
	if (!(((int)adr)&0x40000000)) return *adr;
	//Nope: grab a word and distill the byte.
	int *p=(int *)((int)adr&0xfffffffc);
	int v=*p;
	int w=((int)adr&3);
	if (w==0) return ((v>>0)&0xff);
	if (w==1) return ((v>>8)&0xff);
	if (w==2) return ((v>>16)&0xff);
	if (w==3) return ((v>>24)&0xff);

    abort();
    return 0;
}

const char* StateFileName = "/storage/smsplus.sav";
const char* StoragePath = "/storage";

static void SaveState()
{
    // Save sram
    odroid_input_battery_monitor_enabled_set(0);
    odroid_system_led_set(1);

    char* romName = odroid_settings_RomFilePath_get();
    if (romName)
    {
        char* fileName = odroid_util_GetFileName(romName);
        if (!fileName) abort();

        char* pathName = malloc(1024);
        if (!pathName) abort();

        strcpy(pathName, StoragePath);
        strcat(pathName, "/");
        strcat(pathName, fileName);
        strcat(pathName, ".sav");


        printf("SaveState: pathName='%s'\n", pathName);

        FILE* f = fopen(pathName, "w");

        if (f == NULL)
        {
            printf("SaveState: fopen save failed\n");
        }
        else
        {
            system_save_state(f);
            fclose(f);

            printf("SaveState: system_save_state OK.\n");
        }

        free(pathName);
        free(fileName);
    }
    else
    {
        FILE* f = fopen(StateFileName, "w");
        if (f == NULL)
        {
            printf("SaveState: fopen save failed\n");
        }
        else
        {
            system_save_state(f);
            fclose(f);

            printf("SaveState: system_save_state OK.\n");
        }
    }

    odroid_system_led_set(0);
    odroid_input_battery_monitor_enabled_set(1);
}

static void LoadState(const char* cartName)
{
    char* romName = odroid_settings_RomFilePath_get();
    if (romName)
    {
        char* fileName = odroid_util_GetFileName(romName);
        if (!fileName) abort();

        char* pathName = malloc(1024);
        if (!pathName) abort();

        strcpy(pathName, StoragePath);
        strcat(pathName, "/");
        strcat(pathName, fileName);
        strcat(pathName, ".sav");


        printf("LoadState: pathName='%s'\n", pathName);

        FILE* f = fopen(pathName, "r");

        if (f == NULL)
        {
            printf("LoadState: fopen load failed\n");
        }
        else
        {
            system_load_state(f);
            fclose(f);

            printf("LoadState: system_load_state OK.\n");
        }

        free(pathName);
        free(fileName);
    }
    else
    {
        FILE* f = fopen(StateFileName, "r");
        if (f == NULL)
        {
            printf("LoadState: fopen load failed\n");
        }
        else
        {
            system_load_state(f);
            fclose(f);

            printf("LoadState: system_load_state OK.\n");
        }
    }

    Volume = odroid_settings_Volume_get();
}

static void PowerDown()
{
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");
    odroid_audio_terminate();

    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) { vTaskDelay(1); }


    // state
    printf("PowerDown: Saving state.\n");
    SaveState();

    // LCD
    printf("PowerDown: Powerdown LCD panel.\n");
    ili9341_poweroff();

    odroid_system_sleep();

    // Should never reach here
    abort();
}

static void DoHome()
{
    esp_err_t err;
    uint16_t* param = 1;

    // Clear audio to prevent studdering
    printf("PowerDown: stopping audio.\n");
    odroid_audio_terminate();


    // Stop tasks
    printf("PowerDown: stopping tasks.\n");

    xQueueSend(vidQueue, &param, portMAX_DELAY);
    while (videoTaskIsRunning) { vTaskDelay(1); }


    // state
    printf("PowerDown: Saving state.\n");
    SaveState();


    // Set factory app
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (partition == NULL)
    {
        abort();
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK)
    {
        abort();
    }

    // Reset
    esp_restart();
}

void system_load_sram(void)
{
    printf("system_load_sram\n");
    //sram_load();
}

//char cartName[1024];
void app_main(void)
{
    printf("smsplus start.\n");

    nvs_flash_init();

    odroid_system_init();

    // Joystick.
    odroid_input_gamepad_init();
    odroid_input_battery_level_init();


    // Boot state overrides
    bool forceConsoleReset = false;


    ili9341_prepare();


    // Disable LCD CD to prevent garbage
    const gpio_num_t LCD_PIN_NUM_CS = GPIO_NUM_5;

    gpio_config_t io_conf = { 0 };
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LCD_PIN_NUM_CS);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;

    gpio_config(&io_conf);
    gpio_set_level(LCD_PIN_NUM_CS, 1);


    switch (esp_sleep_get_wakeup_cause())
    {
        case ESP_SLEEP_WAKEUP_EXT0:
        {
            printf("app_main: ESP_SLEEP_WAKEUP_EXT0 deep sleep reset\n");
            break;
        }

        case ESP_SLEEP_WAKEUP_EXT1:
        case ESP_SLEEP_WAKEUP_TIMER:
        case ESP_SLEEP_WAKEUP_TOUCHPAD:
        case ESP_SLEEP_WAKEUP_ULP:
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        {
            printf("app_main: Unexpected deep sleep reset\n");

            odroid_gamepad_state bootState = odroid_input_read_raw();

            if (bootState.values[ODROID_INPUT_MENU])
            {
                // Force return to factory app to recover from
                // ROM loading crashes

                // Set factory app
                const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
                if (partition == NULL)
                {
                    abort();
                }

                esp_err_t err = esp_ota_set_boot_partition(partition);
                if (err != ESP_OK)
                {
                    abort();
                }

                // Reset
                esp_restart();
            }

            if (bootState.values[ODROID_INPUT_START])
            {
                // Reset emulator if button held at startup to
                // override save state
                forceConsoleReset = true; //emu_reset();
            }
        }
            break;

        default:
            printf("app_main: Not a deep sleep reset\n");
            break;
    }


    void* const romAddress = (void*)0x3f800000;
    size_t cartSize = 1024 * 1024;

    char* cartName = odroid_settings_RomFilePath_get();
    if (!cartName)
    {
        printf("app_main: Reading cartName from flash.\n");

        // determine cart type
        cartName = (char*)malloc(1024);
        if (!cartName)
            abort();

        esp_err_t err1 = spi_flash_read(0x300000, cartName, 1024);
        if (err1 != ESP_OK)
        {
            printf("spi_flash_read failed. (%d)\n", err1);
            abort();
        }

        cartName[1023] = 0;


        // copy from flash
        spi_flash_mmap_handle_t hrom;

        const esp_partition_t* part = esp_partition_find_first(0x40, 0, NULL);
        if (part == 0)
        {
        	printf("esp_partition_find_first failed.\n");
        	abort();
        }

        void* flashAddress;
        esp_err_t err = esp_partition_mmap(part, 0, cartSize, SPI_FLASH_MMAP_DATA, (const void**)&flashAddress, &hrom);
        if (err != ESP_OK)
        {
            printf("esp_partition_mmap failed. (%d)\n", err);
            abort();
        }

        memcpy(romAddress, flashAddress, cartSize);
    }
    else
    {
        printf("app_main: loading from sdcard.\n");

        // copy from SD card
        esp_err_t r = odroid_sdcard_open("/sd");
        if (r != ESP_OK) abort();

        char* path = cartName; //(char*)malloc(1024);
        // if (!path) abort();
        //
        // strcpy(path, "/sd/roms/sms/");
        // strcat(path, cartName);

        cartSize = odroid_sdcard_copy_file_to_memory(path, romAddress);
        //free(path);
        if (cartSize == 0) abort();

        r = odroid_sdcard_close();
        if (r != ESP_OK) abort();
    }

    printf("app_main: cartName='%s'\n", cartName);

    if (strstr(cartName, ".sms") != 0 ||
        strstr(cartName, ".SMS") != 0)
    {
        cart.type = TYPE_SMS;
    }
    else
    {
        cart.type = TYPE_GG;
    }

    free(cartName);


    framebuffer[0] = heap_caps_malloc(256 * 192, MALLOC_CAP_8BIT | MALLOC_CAP_DMA); //malloc(256 * 192);
    if (!framebuffer[0]) abort();
    printf("app_main: framebuffer[0]=%p\n", framebuffer[0]);

    framebuffer[1] = heap_caps_malloc(256 * 192, MALLOC_CAP_8BIT | MALLOC_CAP_DMA); //malloc(256 * 192);
    if (!framebuffer[1]) abort();
    printf("app_main: framebuffer[1]=%p\n", framebuffer[1]);


    ili9341_init();
    ili9341_write_frame_sms(NULL, NULL, false, false);

    odroid_audio_init(AUDIO_SAMPLE_RATE);


    vidQueue = xQueueCreate(1, sizeof(uint16_t*));
    xTaskCreatePinnedToCore(&videoTask, "videoTask", 1024 * 4, NULL, 5, &videoTaskHandle, 1);


    sms.use_fm = 0;

#if 1
	sms.country = TYPE_OVERSEAS;
#else
    sms.country = TYPE_DOMESTIC;
#endif

	sms.dummy = framebuffer[0]; //A normal cart shouldn't access this memory ever. Point it to vram just in case.
	sms.sram = malloc(SRAM_SIZE);
    if (!sms.sram)
        abort();

    memset(sms.sram, 0xff, SRAM_SIZE);

    bitmap.width = 256;
	bitmap.height = 192;
	bitmap.pitch = 256;
	bitmap.depth = 8;
    bitmap.data = framebuffer[0];

    cart.pages = (cartSize / 0x4000);
    cart.rom = romAddress;


    system_init2(AUDIO_SAMPLE_RATE);


	printf("Initializing SPIFFS\n");

    esp_vfs_spiffs_conf_t conf = {
      .base_path = StoragePath,
      .partition_label = NULL,
      .max_files = 1,
      .format_if_mount_failed = true
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            printf("Failed to mount or format filesystem.\n");
            abort();
        } else if (ret == ESP_ERR_NOT_FOUND) {
            printf("Failed to find SPIFFS partition.\n");
            abort();
        } else {
            printf("Failed to initialize SPIFFS (%d).\n", ret);
            abort();
        }
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        printf("Failed to get SPIFFS partition information. \n");
		abort();
    } else {
        printf("Partition size: total: %d, used: %d\n", total, used);
    }


    // Restore state
    LoadState(cartName);

    if (forceConsoleReset)
    {
        // Reset emulator if button held at startup to
        // override save state
        system_reset();
    }


    odroid_gamepad_state previousState;
    odroid_input_gamepad_read(&previousState);

    uint startTime;
    uint stopTime;
    uint totalElapsedTime = 0;
    int frame = 0;
    uint16_t muteFrameCount = 0;
    uint16_t powerFrameCount = 0;

    bool ignoreMenuButton = previousState.values[ODROID_INPUT_MENU];

    while (true)
    {
        odroid_gamepad_state joystick;
        odroid_input_gamepad_read(&joystick);

        if (ignoreMenuButton)
        {
            ignoreMenuButton = previousState.values[ODROID_INPUT_MENU];
        }

        if (!ignoreMenuButton && previousState.values[ODROID_INPUT_MENU] && joystick.values[ODROID_INPUT_MENU])
        {
            ++powerFrameCount;
        }
        else
        {
            powerFrameCount = 0;
        }

        // Note: this will cause an exception on 2nd Core in Debug mode
        if (powerFrameCount > 60 * 2)
        {
            // Turn Blue LED on. Power state change turns it off
            odroid_system_led_set(1);
            PowerDown();
        }

        if (previousState.values[ODROID_INPUT_VOLUME] && !joystick.values[ODROID_INPUT_VOLUME])
        {
            odroid_audio_volume_change();
            printf("main: Volume=%d\n", odroid_audio_volume_get());
        }

        if (!ignoreMenuButton && previousState.values[ODROID_INPUT_MENU] && !joystick.values[ODROID_INPUT_MENU])
        {
            DoHome();
        }


        // Scaling
        if (joystick.values[ODROID_INPUT_START] && !previousState.values[ODROID_INPUT_RIGHT] && joystick.values[ODROID_INPUT_RIGHT])
        {
            scaling_enabled = !scaling_enabled;
        }


        startTime = xthal_get_ccount();


        int smsButtons=0;
    	if (joystick.values[ODROID_INPUT_UP]) smsButtons |= INPUT_UP;
    	if (joystick.values[ODROID_INPUT_DOWN]) smsButtons |= INPUT_DOWN;
    	if (joystick.values[ODROID_INPUT_LEFT]) smsButtons |= INPUT_LEFT;
    	if (joystick.values[ODROID_INPUT_RIGHT]) smsButtons |= INPUT_RIGHT;
    	if (joystick.values[ODROID_INPUT_A]) smsButtons |= INPUT_BUTTON1;
    	if (joystick.values[ODROID_INPUT_B]) smsButtons |= INPUT_BUTTON2;

        int smsSystem=0;
    	if (joystick.values[ODROID_INPUT_START]) smsSystem |= INPUT_START;
    	if (joystick.values[ODROID_INPUT_SELECT]) smsSystem |= INPUT_PAUSE;

    	input.pad[0]=smsButtons;
        input.system=smsSystem;


        if (0 || (frame % 2) == 0)
        {
            sms_frame(0);

            xQueueSend(vidQueue, &bitmap.data, portMAX_DELAY);

            currentFramebuffer = currentFramebuffer ? 0 : 1;
            bitmap.data = framebuffer[currentFramebuffer];
        }
        else
        {
            sms_frame(1);
        }

        // Create a buffer for audio if needed
        if (!audioBuffer || audioBufferCount < snd.bufsize)
        {
            if (audioBuffer)
                free(audioBuffer);

            size_t bufferSize = snd.bufsize * 2 * sizeof(int16_t);
            //audioBuffer = malloc(bufferSize);
            audioBuffer = heap_caps_malloc(bufferSize, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
            if (!audioBuffer)
                abort();

            audioBufferCount = snd.bufsize;

            printf("app_main: Created audio buffer (%d bytes).\n", bufferSize);
        }

        // Process audio
        for (int x = 0; x < snd.bufsize; x++)
        {
            uint32_t sample;

            if (muteFrameCount < 60 * 2)
            {
                // When the emulator starts, audible poping is generated.
                // Audio should be disabled during this startup period.
                sample = 0;
                ++muteFrameCount;
            }
            else
            {
                sample = (snd.buffer[0][x] << 16) + snd.buffer[1][x];
            }

            audioBuffer[x] = sample;
        }

        // send audio

        odroid_audio_submit((short*)audioBuffer, snd.bufsize);


        stopTime = xthal_get_ccount();

        previousState = joystick;


        int elapsedTime;
        if (stopTime > startTime)
          elapsedTime = (stopTime - startTime);
        else
          elapsedTime = ((uint64_t)stopTime + (uint64_t)0xffffffff) - (startTime);

        totalElapsedTime += elapsedTime;
        ++frame;

        if (frame == 60)
        {
          float seconds = totalElapsedTime / (CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ * 1000000.0f);
          float fps = frame / seconds;


          printf("HEAP:0x%x, FPS:%f, BATTERY:%d [%d]\n", esp_get_free_heap_size(), fps, battery.millivolts, battery.percentage);

          frame = 0;
          totalElapsedTime = 0;
        }
    }
}
