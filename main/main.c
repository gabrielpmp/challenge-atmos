#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#ifdef CONFIG_IDF_TARGET_ESP32
#include "driver/sdmmc_host.h"
#endif

TaskHandle_t sdTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;

static const char *TAG = "example";

#define BUFFSIZE 1024

static char ota_write_data[BUFFSIZE + 1] = { 0 };

#define MOUNT_POINT "/sdcard"
sdmmc_card_t* card;
const char mount_point[] = MOUNT_POINT;
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
bool is_spi_started = false;

#define USE_SPI_MODE

// DMA channel to be used by the SPI peripheral
#ifndef SPI_DMA_CHAN
#define SPI_DMA_CHAN    1
#endif //SPI_DMA_CHAN

#ifdef USE_SPI_MODE
// Pin mapping when using SPI mode.
// With this mapping, SD card can be used both in SPI and 1-line SD mode.
// Note that a pull-up on CS line is required in SD mode.
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#endif //USE_SPI_MODE

// Card Detect and Write Protect pins used in the SD card
#define PIN_NUM_CD   25
#define PIN_NUM_WP   26
#define ESP_INTR_FLAG_DEFAULT 0

#define BLINK_GPIO   2
#define BUTTON_GPIO  4
#define CONFIG_EXAMPLE_SKIP_VERSION_CHECK

volatile int is_sd_present = false;
// Flag to detect SD card mounting status
int is_sd_card_mounted = false;
// Flag to prevent repeated application of update on Write Protected cards
int is_ota_already_done = false;

static void IRAM_ATTR gpio_isr_handler(void* arg){
    uint32_t gpio_num = (uint32_t) arg;
    is_sd_present = !gpio_get_level(gpio_num);
}

void start_spi_bus(){
    // Reduce default speed due to peripheral limitations
    esp_err_t ret;
    host.max_freq_khz = 8000;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }
    is_spi_started = true;
    return;
}

int start_sd_card(){
    esp_err_t ret;
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    //Moved to global
    //sdmmc_card_t* card;
    //const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

#ifndef USE_SPI_MODE
    ESP_LOGI(TAG, "Using SDMMC peripheral");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    // To use 1-line SD mode, uncomment the following line:
    // slot_config.width = 1;

    // GPIOs 15, 2, 4, 12, 13 should have external 10k pull-ups.
    // Internal pull-ups are not sufficient. However, enabling internal pull-ups
    // does make a difference some boards, so we do that here.
    gpio_set_pull_mode(15, GPIO_PULLUP_ONLY);   // CMD, needed in 4- and 1- line modes
    gpio_set_pull_mode(2, GPIO_PULLUP_ONLY);    // D0, needed in 4- and 1-line modes
    gpio_set_pull_mode(4, GPIO_PULLUP_ONLY);    // D1, needed in 4-line mode only
    gpio_set_pull_mode(12, GPIO_PULLUP_ONLY);   // D2, needed in 4-line mode only
    gpio_set_pull_mode(13, GPIO_PULLUP_ONLY);   // D3, needed in 4- and 1-line modes

    ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
#else
    ESP_LOGI(TAG, "Using SPI peripheral");

    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
#endif //USE_SPI_MODE

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
            return 0;
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
            return 0;
        }
        return 0;
    }

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);
    return 1;
}

static bool diagnostic(void){
    gpio_config_t io_conf;
    io_conf.intr_type    = GPIO_PIN_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BUTTON_GPIO);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Diagnostics (5 sec)...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    bool diagnostic_is_ok = gpio_get_level(BUTTON_GPIO);

    gpio_reset_pin(BUTTON_GPIO);
    return diagnostic_is_ok;
}

void toggleLED(void * parameter){
    while (1) {
        printf("Turning off the LED\n");
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        /* Blink on (output high) */
	    printf("Turning on the LED\n");
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

static void otaTask(void * parameter){
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA example");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);
    assert(update_partition != NULL);

    FILE* update = fopen(MOUNT_POINT"/update.bin", "rb");
    assert(update != NULL);
    ESP_LOGI(TAG, "Opened update file!");

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        fclose(update);
        vTaskResume(sdTaskHandle);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");
    
    // Check filesize to match read size later
    struct stat st_func;
    stat(MOUNT_POINT"/update.bin", &st_func);
    int data_read;
    int binary_file_length = 0;
    bool image_header_was_checked = false;

    do {
        /* Read file in chunks into the OTA buffer */
        data_read = fread(ota_write_data, 1, BUFFSIZE, update);

        if (data_read > 0) {
            if (image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                    // check current version with downloading
                    memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                    }

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                    }

                    // check current version with last invalid partition
                    if (last_invalid_app != NULL) {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                            ESP_LOGW(TAG, "New version is the same as invalid version.");
                            ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                            ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                            fclose(update);
                            vTaskResume(sdTaskHandle);
                            vTaskDelete(NULL);
                        }
                    }
#ifndef CONFIG_EXAMPLE_SKIP_VERSION_CHECK
                    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
                        ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
                        fclose(update);
                        vTaskResume(sdTaskHandle);
                        vTaskDelete(NULL);
                    }
#endif

                    image_header_was_checked = true;

                } else {
                    ESP_LOGE(TAG, "received package is not fit len");
                    fclose(update);
                    vTaskResume(sdTaskHandle);
                    vTaskDelete(NULL);
                }
            }
            err = esp_ota_write( update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
                fclose(update);
                vTaskResume(sdTaskHandle);
                vTaskDelete(NULL);
            } 
            binary_file_length += data_read;
            ESP_LOGI(TAG, "Written image length %d", binary_file_length);
        }

    // Keep looping until the whole file is sent or if SD card is removed
    } while (data_read != 0 && is_sd_present);

    // Check if read size and original size are compatible
    if (!is_sd_present){
        ESP_LOGE(TAG, "SD Card removed! Aborting ...");
        fclose(update);
        vTaskResume(sdTaskHandle);
        vTaskDelete(NULL);
    }

    // Check if read size and original size are compatible
    if (binary_file_length != (int)st_func.st_size){
        ESP_LOGE(TAG, "File not read successfully! Aborting ...");
        fclose(update);
        vTaskResume(sdTaskHandle);
        vTaskDelete(NULL);
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        fclose(update);
        vTaskDelete(NULL);
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        fclose(update);
        vTaskResume(sdTaskHandle);
        vTaskDelete(NULL);
    }

    fclose(update);
    ESP_LOGI(TAG, "Done! Unmounting ...");
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
#ifdef USE_SPI_MODE
    //deinitialize the bus after all devices are removed
    spi_bus_free(host.slot);
#endif
    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();
    return ;
}

static void sdHandleTask(void * parameter){
    struct stat st_sd;
    while (1){
        if(is_sd_present && !is_sd_card_mounted){
            // If the SD card is present and has not been mounted, try to mount it
            ESP_LOGI(TAG, "SD CARD FOUND!");
            ESP_LOGI(TAG, "MOUNTING ...");
            if(!is_spi_started){
                start_spi_bus();
            }
            is_sd_card_mounted = start_sd_card();
        } else if (!is_sd_present && is_sd_card_mounted){
            // If the SD card is removed and is still mounted, unmount it
            ESP_LOGE(TAG, "SD CARD REMOVED!");
            ESP_LOGE(TAG, "UNMOUNTING ...");
            esp_vfs_fat_sdcard_unmount(mount_point, card);
            ESP_LOGI(TAG, "Card unmounted");
            is_sd_card_mounted = false;
#ifdef USE_SPI_MODE
            //deinitialize the bus after all devices are removed
            spi_bus_free(host.slot);
            is_spi_started = false;
#endif
        } else if(is_sd_present && is_sd_card_mounted){
            // If the SD card is present and mounted, look for update file
            if(!is_ota_already_done){
                if (stat(MOUNT_POINT"/update.bin", &st_sd) == 0) {
                    // Log if it is found
                    ESP_LOGI(TAG, "UPDATE FILE FOUND!!");
                    // Print its size in bytes
                    printf("SIZE OF FILE: %lu\n", (unsigned long)st_sd.st_size);
                    ESP_LOGI(TAG, "STARTING UPDATE PROCESS ...");
                    xTaskCreate(otaTask, "otaTask", 8192, NULL, 5, &otaTaskHandle);
                    vTaskSuspend( NULL );
                } else {
                    // Log if it is not found
                    ESP_LOGE(TAG, "NO UPDATE FILE FOUND!");
                }
            }
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{   
    // Configuration of GPIO pin to detect first insertion of SD card
    // and write protect detection
    gpio_config_t io_conf;
    // Enable CD interrupt on both edges
    // Rising edge  ---> Card removed
    // Falling edge ---> Card inserted
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    // Bit mask of the Card Detection (CD) pin
    io_conf.pin_bit_mask = ((1ULL<<PIN_NUM_CD) | (1ULL<<PIN_NUM_WP));
    // Set as input mode    
    io_conf.mode = GPIO_MODE_INPUT;
    // Enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    gpio_set_pull_mode(PIN_NUM_CD, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_WP, GPIO_PULLUP_ONLY);

    // Install GPIO ISR service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

    // If card already present, try to mount
    if(gpio_get_level(PIN_NUM_CD) == 0){
        is_sd_present = true;
        ESP_LOGI(TAG, "SD CARD FOUND!");
        ESP_LOGI(TAG, "MOUNTING ...");
        if(!is_spi_started){
            start_spi_bus();
        }
        is_sd_card_mounted = start_sd_card();
    }

    // Adds ISR handler to detect SD card insertion/removal
    gpio_isr_handler_add(PIN_NUM_CD, gpio_isr_handler, (void*) PIN_NUM_CD);

    // Check running partition to check if OTA was performed correctly
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    // Check current OTA state to validate new image
    esp_ota_img_states_t ota_state;
    struct stat st;
    int cleanup_update = false;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "WP: %u", gpio_get_level(PIN_NUM_WP));
            // Test image to check if working properly
            bool diagnostic_is_ok = diagnostic();
            if (diagnostic_is_ok) {
                ESP_LOGI(TAG, "Diagnostics completed successfully! Continuing execution ...");
                esp_ota_mark_app_valid_cancel_rollback();
                if (gpio_get_level(PIN_NUM_WP) == 0) {
                    ESP_LOGE(TAG, "SD card is write protected! Cannot erase file ...");
                    // Set flag to prevent redetection of previously applied update
                    is_ota_already_done = true;
                } else if (stat(MOUNT_POINT"/update.bin", &st) == 0) {
                    // Set flag to perform cleanup outside OTA validity check
                    cleanup_update = true;
                }
            } else {
                // OTA Validity test failed, perform rollback to previous valid version
                ESP_LOGE(TAG, "Diagnostics failed! Start rollback to the previous version ...");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }

    // Perform cleanup in the SD card of firmware file just applied
    if(cleanup_update){
        unlink(MOUNT_POINT"/update.bin");
        ESP_LOGI(TAG, "Removing done!");
        cleanup_update = false;
    }

    Select GPIO to blink LED
    gpio_pad_select_gpio(BLINK_GPIO);
    // Set GPIO to blink LED as output
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
    // Create blink LED task
    xTaskCreate(toggleLED, "toggleLED", 2048, NULL, 1, NULL);

    // Create task to handle SD Card
    xTaskCreate(sdHandleTask, "sdHandleTask", 8192, NULL, 1, &sdTaskHandle);

}