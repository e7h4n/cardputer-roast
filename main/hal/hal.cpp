/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "hal_config.h"
#include <mooncake_log.h>
#include <M5Unified.hpp>
#include <nvs_flash.h>
#include <memory>

static std::unique_ptr<Hal> _hal_instance;
static const std::string _tag = "HAL";

Hal& GetHAL()
{
    if (!_hal_instance) {
        mclog::tagInfo(_tag, "creating hal instance");
        _hal_instance = std::make_unique<Hal>();
    }
    return *_hal_instance.get();
}

void Hal::init()
{
    mclog::tagInfo(_tag, "init");

    auto config = M5.config();
    config.internal_mic = false;
    config.internal_spk = false;
    M5.begin(config);
    M5.Display.setBrightness(0);

    display_init();
    keyboard_init();
    nvs_init();
    spi_init();
    ext_init();
}

void Hal::update()
{
    M5.update();
    keyboard.update();
}

void Hal::feedTheDog()
{
    vTaskDelay(1);
}

void Hal::display_init()
{
    mclog::tagInfo(_tag, "display init");

    // RGB332 canvas uses ~32 KB, leaving room for resident ESP Hub TLS plus BLE.
    canvas.setColorDepth(8);
    if (!canvas.createSprite(display.width(), display.height())) mclog::tagError(_tag, "canvas alloc failed");
}

// https://github.com/78/xiaozhi-esp32/blob/main/main/main.cc
void Hal::nvs_init()
{
    mclog::tagInfo(_tag, "nvs init");

    esp_err_t ret = nvs_flash_init();
    // Never erase shared NVS: it also holds ESP Hub identity and Wi-Fi credentials.
    ESP_ERROR_CHECK(ret);
}

void Hal::ext_init()
{
    mclog::tagInfo(_tag, "external screen init");

    // After spi_init(): the bus is the SD card's, the panel only adds itself to it.
    if (!ext.init()) mclog::tagError(_tag, "external screen init failed");
    ext.setRotation(1);
    ext.fillScreen(TFT_BLACK);
}

void Hal::keyboard_init()
{
    mclog::tagInfo(_tag, "keyboard init");

    if (!keyboard.init()) mclog::tagError(_tag, "keyboard init failed");
}

/* -------------------------------------------------------------------------- */
/*                                     SPI                                    */
/* -------------------------------------------------------------------------- */
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>

static bool _spi_bus_initialized = false;

void Hal::spi_init()
{
    mclog::tagInfo(_tag, "spi init");

    esp_err_t ret;

    // spi_host_device_t host_id = SPI2_HOST;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num      = HAL_PIN_SPI_MOSI;
    bus_cfg.miso_io_num      = HAL_PIN_SPI_MISO;
    bus_cfg.sclk_io_num      = HAL_PIN_SPI_SCLK;
    bus_cfg.quadwp_io_num    = -1;
    bus_cfg.quadhd_io_num    = -1;
    bus_cfg.max_transfer_sz  = 4000;

    // Initialize SPI bus only if not already initialized
    if (!_spi_bus_initialized) {
        ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            mclog::tagError(_tag, "failed to initialize SPI bus");
            return;
        }
        _spi_bus_initialized = true;
        mclog::tagInfo(_tag, "spi bus initialized");
    } else {
        mclog::tagWarn(_tag, "spi bus already initialized, reusing");
    }
}

/* -------------------------------------------------------------------------- */
/*                                   SD Card                                  */
/* -------------------------------------------------------------------------- */
// https://github.com/espressif/esp-idf/blob/v5.3.3/examples/storage/sd_card/sdspi
// https://github.com/m5stack/M5PaperS3-UserDemo/blob/main/main/hal/hal.h
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>

#define MOUNT_POINT "/sdcard"

static sdmmc_card_t* _sd_card = nullptr;

void Hal::sd_card_init()
{
    mclog::tagInfo(_tag, "sd card init");

    if (!_spi_bus_initialized) {
        spi_init();
    }

    // If already mounted successfully, return
    if (_is_sd_card_mounted) {
        mclog::tagInfo(_tag, "sd card already mounted");
        return;
    }

    esp_err_t ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 5, .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false, .use_one_fat = false};

    const char mount_point[] = MOUNT_POINT;
    mclog::tagInfo(_tag, "initializing SD card");

    // Initialize SD card slot
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs               = HAL_PIN_SD_CARD_CS;
    slot_config.host_id               = (spi_host_device_t)host.slot;

    mclog::tagInfo(_tag, "mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &_sd_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            mclog::tagError(_tag, "failed to mount filesystem");
        } else {
            mclog::tagError(_tag, "failed to initialize the card, make sure SD card lines have pull-up resistors");
        }

        // Don't clean up SPI bus on failure - leave it for retry
        mclog::tagInfo(_tag, "sd card init failed, but spi bus remains initialized for retry");
        return;
    }

    mclog::tagInfo(_tag, "filesystem mounted successfully");

    sdmmc_card_print_info(stdout, _sd_card);

    _is_sd_card_mounted = true;
}

#include <diskio_sdmmc.h>
#include <ff.h>
#include <dirent.h>
#include <esp_heap_caps.h>

static std::string sd_fs_name(int t)
{
    switch (t) {
        case FS_FAT12: return "FAT12";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        default: return "FAT?";
    }
}

std::string Hal::sdCardInspect()
{
    std::string out;
    if (!_spi_bus_initialized) spi_init();
    if (!_is_sd_card_mounted) sd_card_init();
    if (_is_sd_card_mounted && _sd_card) {
        double gb = ((double)(uint64_t)_sd_card->csd.capacity * _sd_card->csd.sector_size) / 1e9;
        BYTE pdrv  = ff_diskio_get_pdrv_card(_sd_card);
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        FATFS* fs   = nullptr;
        DWORD fre   = 0;
        if (f_getfree(drv, &fre, &fs) == FR_OK && fs) {
            double tot  = (double)(fs->n_fatent - 2) * fs->csize * 512 / 1e9;
            double free = (double)fre * fs->csize * 512 / 1e9;
            out += fmt::format("card {} {:.1f}GB mounted {} cluster {}KB total {:.2f}GB free {:.2f}GB\n",
                               _sd_card->cid.name, gb, sd_fs_name(fs->fs_type), fs->csize / 2, tot, free);
        }
        DIR* d = opendir(MOUNT_POINT);
        int n  = 0;
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (n < 30) out += fmt::format("  {}{}\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
                n++;
            }
            closedir(d);
        }
        out += fmt::format("root entries: {}\n", n);
        return out;
    }
    // Not mountable: look at the raw layout without writing anything.
    sdmmc_host_t host              = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot     = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs                   = HAL_PIN_SD_CARD_CS;
    slot.host_id                   = (spi_host_device_t)host.slot;
    sdspi_dev_handle_t handle      = 0;
    sdspi_host_init();
    if (sdspi_host_init_device(&slot, &handle) != ESP_OK) {
        sdspi_host_deinit();
        return "sd: spi device init failed\n";
    }
    host.slot = handle;
    sdmmc_card_t card;
    if (sdmmc_card_init(&host, &card) != ESP_OK) {
        out = "no card detected\n";
    } else {
        double gb    = ((double)(uint64_t)card.csd.capacity * card.csd.sector_size) / 1e9;
        uint8_t* buf = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA);
        out += fmt::format("card {} {:.1f}GB not mountable as FAT\n", card.cid.name, gb);
        if (buf && sdmmc_read_sectors(&card, buf, 0, 1) == ESP_OK) {
            std::string oem((char*)buf + 3, 8);
            if (oem == "EXFAT   " || oem == "NTFS    ") {
                out += fmt::format("no partition table, filesystem {}\n", oem);
            } else if (buf[510] == 0x55 && buf[511] == 0xAA) {
                uint8_t type   = buf[450];
                uint32_t start = buf[454] | (buf[455] << 8) | (buf[456] << 16) | ((uint32_t)buf[457] << 24);
                out += fmt::format("MBR partition1 type 0x{:02X} start {}\n", type, start);
                if (start && sdmmc_read_sectors(&card, buf, start, 1) == ESP_OK)
                    out += fmt::format("partition boot sector OEM '{}'\n", std::string((char*)buf + 3, 8));
            } else {
                out += "no MBR signature (blank?)\n";
            }
        }
        if (buf) heap_caps_free(buf);
    }
    sdspi_host_remove_device(handle);
    sdspi_host_deinit();
    return out;
}

bool Hal::sdCardFormat(std::string& msg)
{
    if (!_spi_bus_initialized) spi_init();
    esp_vfs_fat_sdmmc_mount_config_t cfg = {.format_if_mount_failed   = true,
                                            .max_files                = 5,
                                            .allocation_unit_size     = 32 * 1024,
                                            .disk_status_check_enable = false,
                                            .use_one_fat              = false};
    if (!_is_sd_card_mounted) sd_card_init();
    esp_err_t r;
    if (_is_sd_card_mounted) {
        r = esp_vfs_fat_sdcard_format_cfg(MOUNT_POINT, _sd_card, &cfg);
        if (r != ESP_OK) _is_sd_card_mounted = false;
    } else {
        // An exFAT/blank card fails to mount; mounting with format_if_mount_failed partitions + formats it.
        sdmmc_host_t host          = SDSPI_HOST_DEFAULT();
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.gpio_cs               = HAL_PIN_SD_CARD_CS;
        slot.host_id               = (spi_host_device_t)host.slot;
        r = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &cfg, &_sd_card);
        _is_sd_card_mounted = (r == ESP_OK);
    }
    msg = r == ESP_OK ? "format ok" : fmt::format("format failed: {}", esp_err_to_name(r));
    return r == ESP_OK && _is_sd_card_mounted;
}
