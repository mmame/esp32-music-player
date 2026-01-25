#include "file_manager_ui.h"
#include "audio_player_ui.h"
#include "wifi_config_ui.h"
#include "sunton_esp32_8048s050c.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include "ff.h"

static const char *TAG = "FileManager";

#define MOUNT_POINT "/sdcard"
#define MAX_FILES 100
#define MAX_FILENAME_LEN 64
#define MAX_PATH_LEN 256
#define TRANSITION_IGNORE_MS 300  // Ignore events for 300ms after screen transition

// SD card handle
static sdmmc_card_t *card = NULL;
static bool sd_mounted = false;
static char current_path[MAX_PATH_LEN] = "";

// UI elements
static lv_obj_t *file_manager_screen = NULL;
static lv_obj_t *file_list = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *disk_space_label = NULL;
static lv_obj_t *refresh_btn = NULL;
static lv_obj_t *context_menu = NULL;
static lv_obj_t *rename_dialog = NULL;
static lv_obj_t *rename_textarea = NULL;
static int selected_file_idx = -1;

// File item structure
typedef struct {
    char name[MAX_FILENAME_LEN];
    bool is_dir;
    size_t size;
} file_item_t;

static file_item_t files[MAX_FILES];
static int file_count = 0;

// Forward declarations
static void refresh_btn_event_cb(lv_event_t *e);
static void file_list_event_cb(lv_event_t *e);
static void show_context_menu(int file_idx, lv_obj_t *btn);
static void close_context_menu(void);
static void context_menu_bg_click_cb(lv_event_t *e);
static void context_menu_rename_cb(lv_event_t *e);
static void context_menu_delete_cb(lv_event_t *e);
static void show_rename_dialog(int file_idx);
static void rename_confirm_cb(lv_event_t *e);
static void rename_cancel_cb(lv_event_t *e);
static void delete_file_confirm_cb(lv_event_t *e);
static void delete_file_cancel_cb(lv_event_t *e);
static bool rename_file(const char *old_name, const char *new_name);
static bool delete_file(const char *filename);

bool file_manager_sd_init(void)
{
    if (sd_mounted) {
        ESP_LOGI(TAG, "SD card already mounted");
        return true;
    }

    esp_err_t ret;
    
    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

    ESP_LOGI(TAG, "Initializing SD card using SPI peripheral");

    // Use SPI mode
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .data_io_default_level = false,
        .max_transfer_sz = 4000,
        .flags = 0,
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags = 0
    };

    ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus: %s", esp_err_to_name(ret));
        return false;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = (spi_host_device_t)host.slot;

    ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s", esp_err_to_name(ret));
        }
        return false;
    }

    sd_mounted = true;
    ESP_LOGI(TAG, "SD card mounted successfully");

    // Card info
    sdmmc_card_print_info(stdout, card);

    return true;
}

void file_manager_sd_deinit(void)
{
    if (!sd_mounted) {
        return;
    }

    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    sd_mounted = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

// Context menu functions
static void close_context_menu(void)
{
    if (context_menu) {
        // Delete the parent (background overlay) which will also delete the menu
        lv_obj_t *parent = lv_obj_get_parent(context_menu);
        if (parent) {
            lv_obj_delete(parent);
        }
        context_menu = NULL;
    }
}

static void context_menu_rename_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int file_idx = (int)(intptr_t)lv_event_get_user_data(e);
        close_context_menu();
        show_rename_dialog(file_idx);
    }
}

// Delete confirmation callbacks (defined before use)
static void delete_file_confirm_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int file_idx = (int)(intptr_t)lv_event_get_user_data(e);
        
        if (file_idx >= 0 && file_idx < file_count) {
            if (delete_file(files[file_idx].name)) {
                lv_label_set_text(status_label, "File deleted");
                file_manager_refresh();
            } else {
                lv_label_set_text(status_label, "Delete failed!");
            }
        }
        
        // Close dialog
        lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e)));
        lv_msgbox_close(mbox);
    }
}

static void delete_file_cancel_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t *mbox = lv_obj_get_parent(lv_obj_get_parent((lv_obj_t *)lv_event_get_target(e)));
        lv_msgbox_close(mbox);
    }
}

static void context_menu_delete_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        int file_idx = (int)(intptr_t)lv_event_get_user_data(e);
        close_context_menu();
        
        if (file_idx >= 0 && file_idx < file_count) {
            // Create delete confirmation
            lv_obj_t *mbox = lv_msgbox_create(NULL);
            lv_msgbox_add_title(mbox, "Delete?");
            
            char text[128];
            snprintf(text, sizeof(text), "Delete %s?", files[file_idx].name);
            lv_msgbox_add_text(mbox, text);
            
            lv_obj_t *btn_yes = lv_msgbox_add_footer_button(mbox, "Yes");
            lv_obj_t *btn_no = lv_msgbox_add_footer_button(mbox, "No");
            lv_msgbox_add_close_button(mbox);
            
            lv_obj_add_event_cb(btn_yes, delete_file_confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)file_idx);
            lv_obj_add_event_cb(btn_no, delete_file_cancel_cb, LV_EVENT_CLICKED, NULL);
        }
    }
}

static void context_menu_bg_click_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_t *target = (lv_obj_t *)lv_event_get_target(e);
        lv_obj_t *bg = lv_obj_get_parent(context_menu);
        // Only close if clicked on the background overlay itself, not on the menu or its children
        if (target == bg) {
            close_context_menu();
        }
    }
}

static void show_context_menu(int file_idx, lv_obj_t *btn)
{
    if (file_idx < 0 || file_idx >= file_count) {
        return;
    }
    
    // Close existing menu if any
    close_context_menu();
    
    // Create semi-transparent background overlay
    lv_obj_t *bg = lv_obj_create(lv_screen_active());
    lv_obj_set_size(bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_50, 0);
    lv_obj_set_style_border_width(bg, 0, 0);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, context_menu_bg_click_cb, LV_EVENT_CLICKED, NULL);
    
    // Create context menu container
    context_menu = lv_obj_create(bg);
    lv_obj_set_size(context_menu, 250, 200);
    lv_obj_center(context_menu);
    lv_obj_set_style_bg_color(context_menu, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_color(context_menu, lv_color_hex(0x00AAFF), 0);
    lv_obj_set_style_border_width(context_menu, 2, 0);
    lv_obj_set_style_radius(context_menu, 10, 0);
    lv_obj_set_style_pad_all(context_menu, 10, 0);
    lv_obj_clear_flag(context_menu, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(context_menu);
    lv_label_set_text(title, files[file_idx].name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
    lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(title, 230);
    
    // Rename button
    lv_obj_t *btn_rename = lv_button_create(context_menu);
    lv_obj_set_size(btn_rename, 230, 50);
    lv_obj_align(btn_rename, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(btn_rename, lv_color_hex(0x0066AA), 0);
    lv_obj_set_style_radius(btn_rename, 8, 0);
    lv_obj_add_event_cb(btn_rename, context_menu_rename_cb, LV_EVENT_CLICKED, (void *)(intptr_t)file_idx);
    
    lv_obj_t *label_rename = lv_label_create(btn_rename);
    lv_label_set_text(label_rename, LV_SYMBOL_EDIT " Rename");
    lv_obj_set_style_text_font(label_rename, &lv_font_montserrat_28, 0);
    lv_obj_center(label_rename);
    
    // Delete button
    lv_obj_t *btn_delete = lv_button_create(context_menu);
    lv_obj_set_size(btn_delete, 230, 50);
    lv_obj_align(btn_delete, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0xAA0000), 0);
    lv_obj_set_style_radius(btn_delete, 8, 0);
    lv_obj_add_event_cb(btn_delete, context_menu_delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)file_idx);
    
    lv_obj_t *label_delete = lv_label_create(btn_delete);
    lv_label_set_text(label_delete, LV_SYMBOL_TRASH " Delete");
    lv_obj_set_style_text_font(label_delete, &lv_font_montserrat_28, 0);
    lv_obj_center(label_delete);
    
    ESP_LOGI(TAG, "Context menu shown for file: %s", files[file_idx].name);
}

// Rename dialog functions
static void rename_confirm_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        const char *new_name = lv_textarea_get_text(rename_textarea);
        
        if (selected_file_idx >= 0 && selected_file_idx < file_count && new_name && strlen(new_name) > 0) {
            if (rename_file(files[selected_file_idx].name, new_name)) {
                lv_label_set_text(status_label, "File renamed");
                file_manager_refresh();
            } else {
                lv_label_set_text(status_label, "Rename failed!");
            }
        }
        
        if (rename_dialog) {
            lv_obj_delete(rename_dialog);
            rename_dialog = NULL;
            rename_textarea = NULL;
        }
        selected_file_idx = -1;
    }
}

static void rename_cancel_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        if (rename_dialog) {
            lv_obj_delete(rename_dialog);
            rename_dialog = NULL;
            rename_textarea = NULL;
        }
        selected_file_idx = -1;
    }
}

static void show_rename_dialog(int file_idx)
{
    if (file_idx < 0 || file_idx >= file_count) {
        return;
    }
    
    selected_file_idx = file_idx;
    
    // Create full-screen rename dialog
    rename_dialog = lv_obj_create(lv_screen_active());
    lv_obj_set_size(rename_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(rename_dialog, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(rename_dialog, LV_OPA_90, 0);
    lv_obj_set_style_border_width(rename_dialog, 0, 0);
    lv_obj_set_style_radius(rename_dialog, 0, 0);
    lv_obj_set_style_pad_all(rename_dialog, 20, 0);
    lv_obj_clear_flag(rename_dialog, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(rename_dialog);
    lv_label_set_text(title, "Rename File/Folder");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    
    // Text input with high contrast
    rename_textarea = lv_textarea_create(rename_dialog);
    lv_obj_set_size(rename_textarea, LV_PCT(90), 60);
    lv_obj_align(rename_textarea, LV_ALIGN_TOP_MID, 0, 90);
    lv_textarea_set_text(rename_textarea, files[file_idx].name);
    lv_textarea_set_one_line(rename_textarea, true);
    lv_obj_set_style_text_font(rename_textarea, &lv_font_montserrat_28, 0);
    // High contrast: white text on dark background
    lv_obj_set_style_text_color(rename_textarea, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(rename_textarea, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_border_color(rename_textarea, lv_color_hex(0x00AAFF), 0);
    lv_obj_set_style_border_width(rename_textarea, 3, 0);
    lv_obj_set_style_pad_all(rename_textarea, 10, 0);
    // Enable and style cursor for visibility - thin cursor
    lv_textarea_set_cursor_click_pos(rename_textarea, true);
    lv_obj_set_style_anim_duration(rename_textarea, 500, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(rename_textarea, lv_color_hex(0xFFFFFF), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(rename_textarea, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_border_width(rename_textarea, 0, LV_PART_CURSOR);
    lv_obj_set_style_width(rename_textarea, 2, LV_PART_CURSOR);  // Thin 2px cursor
    
    // Action buttons below text input, above keyboard
    lv_obj_t *btn_ok = lv_button_create(rename_dialog);
    lv_obj_set_size(btn_ok, 180, 60);
    lv_obj_align(btn_ok, LV_ALIGN_TOP_LEFT, 30, 170);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x00AA00), 0);
    lv_obj_set_style_radius(btn_ok, 10, 0);
    lv_obj_add_event_cb(btn_ok, rename_confirm_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *label_ok = lv_label_create(btn_ok);
    lv_label_set_text(label_ok, LV_SYMBOL_OK " OK");
    lv_obj_set_style_text_font(label_ok, &lv_font_montserrat_28, 0);
    lv_obj_center(label_ok);
    
    lv_obj_t *btn_cancel = lv_button_create(rename_dialog);
    lv_obj_set_size(btn_cancel, 180, 60);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_RIGHT, -30, 170);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0xAA0000), 0);
    lv_obj_set_style_radius(btn_cancel, 10, 0);
    lv_obj_add_event_cb(btn_cancel, rename_cancel_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *label_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(label_cancel, LV_SYMBOL_CLOSE " Cancel");
    lv_obj_set_style_text_font(label_cancel, &lv_font_montserrat_28, 0);
    lv_obj_center(label_cancel);
    
    // Create on-screen keyboard below buttons
    lv_obj_t *kb = lv_keyboard_create(rename_dialog);
    lv_obj_set_size(kb, LV_PCT(95), LV_PCT(45));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_keyboard_set_textarea(kb, rename_textarea);
    
    ESP_LOGI(TAG, "Rename dialog shown for: %s", files[file_idx].name);
}

// File operation functions
static bool delete_file(const char *filename)
{
    if (!sd_mounted) {
        ESP_LOGE(TAG, "SD card not mounted");
        return false;
    }

    const char *base_path = (current_path[0] == '\0') ? MOUNT_POINT : current_path;
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", base_path, filename);
    
    if (remove(filepath) == 0) {
        ESP_LOGI(TAG, "File deleted: %s", filename);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to delete file: %s", filename);
        return false;
    }
}

static bool rename_file(const char *old_name, const char *new_name)
{
    if (!sd_mounted) {
        ESP_LOGE(TAG, "SD card not mounted");
        return false;
    }

    const char *base_path = (current_path[0] == '\0') ? MOUNT_POINT : current_path;
    char old_path[512];
    char new_path[512];
    snprintf(old_path, sizeof(old_path), "%s/%s", base_path, old_name);
    snprintf(new_path, sizeof(new_path), "%s/%s", base_path, new_name);
    
    if (rename(old_path, new_path) == 0) {
        ESP_LOGI(TAG, "File renamed: %s -> %s", old_name, new_name);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to rename file: %s", old_name);
        return false;
    }
}

static void scan_files(void)
{
    file_count = 0;
    
    if (!sd_mounted) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }

    // Use current path, default to root if empty
    const char *scan_path = (current_path[0] == '\0') ? MOUNT_POINT : current_path;
    
    DIR *dir = opendir(scan_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", scan_path);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && file_count < MAX_FILES) {
        // Skip hidden files and current/parent directory entries
        if (entry->d_name[0] == '.') {
            continue;
        }

        size_t name_len = strlen(entry->d_name);
        if (name_len >= MAX_FILENAME_LEN) name_len = MAX_FILENAME_LEN - 1;
        memcpy(files[file_count].name, entry->d_name, name_len);
        files[file_count].name[name_len] = '\0';
        files[file_count].is_dir = (entry->d_type == DT_DIR);

        // Get file size
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", scan_path, entry->d_name);
        
        struct stat st;
        if (stat(filepath, &st) == 0) {
            files[file_count].size = st.st_size;
        } else {
            files[file_count].size = 0;
        }

        file_count++;
    }

    closedir(dir);
    
    // Sort files alphabetically (same as audio player)
    if (file_count > 1) {
        for (int i = 0; i < file_count - 1; i++) {
            for (int j = i + 1; j < file_count; j++) {
                // Compare directories first (dirs before files), then alphabetically
                bool swap = false;
                if (files[i].is_dir && !files[j].is_dir) {
                    swap = false;  // Keep directories first
                } else if (!files[i].is_dir && files[j].is_dir) {
                    swap = true;  // Move directory before file
                } else {
                    // Both are dirs or both are files, sort alphabetically
                    if (strcasecmp(files[i].name, files[j].name) > 0) {
                        swap = true;
                    }
                }
                
                if (swap) {
                    file_item_t temp = files[i];
                    files[i] = files[j];
                    files[j] = temp;
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "Found %d files/folders in %s", file_count, scan_path);
}

static void update_disk_space_label(void)
{
    if (!disk_space_label || !sd_mounted) {
        return;
    }
    
    FATFS *fs;
    DWORD free_clusters;
    
    // Get free clusters using FatFs API
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res == FR_OK) {
        // Calculate total and free space in bytes
        uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
        uint64_t free_sectors = free_clusters * fs->csize;
        
        // Convert to bytes (sector size is 512 bytes on SD cards)
        uint64_t total_bytes = total_sectors * 512;
        uint64_t free_bytes = free_sectors * 512;
        uint64_t used_bytes = total_bytes - free_bytes;
        
        // Convert to MB for display
        uint32_t total_mb = total_bytes / (1024 * 1024);
        uint32_t free_mb = free_bytes / (1024 * 1024);
        uint32_t used_mb = used_bytes / (1024 * 1024);
        
        char space_text[100];
        snprintf(space_text, sizeof(space_text), "Used: %lu MB / %lu MB (Free: %lu MB)", 
                 used_mb, total_mb, free_mb);
        
        lv_label_set_text(disk_space_label, space_text);
    } else {
        lv_label_set_text(disk_space_label, "Disk space: Unknown");
    }
}

static void update_file_list(void)
{
    lv_obj_clean(file_list);

    if (!sd_mounted) {
        lv_obj_t *label = lv_label_create(file_list);
        lv_label_set_text(label, "SD Card not mounted");
        lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
        return;
    }
    
    // Update status label with current path
    char status_text[MAX_PATH_LEN + 32];
    if (current_path[0] == '\0') {
        snprintf(status_text, sizeof(status_text), "SD: /");
    } else {
        // Show path relative to mount point
        const char *rel_path = current_path + strlen(MOUNT_POINT);
        snprintf(status_text, sizeof(status_text), "SD: %s", rel_path[0] ? rel_path : "/");
    }
    lv_label_set_text(status_label, status_text);

    // Add "Up" button if not in root directory (always show, even if no files)
    if (current_path[0] != '\0') {
        lv_obj_t *btn = lv_button_create(file_list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 60);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x004488), 0);
        lv_obj_set_style_radius(btn, 5, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, LV_SYMBOL_UP " ..");
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        // Store -1 to indicate "go up"
        lv_obj_set_user_data(btn, (void *)(intptr_t)-1);
        lv_obj_add_event_cb(btn, file_list_event_cb, LV_EVENT_CLICKED, NULL);
    }

    if (file_count == 0) {
        lv_obj_t *label = lv_label_create(file_list);
        lv_label_set_text(label, "No files found");
        lv_obj_set_style_text_color(label, lv_color_hex(0x888888), 0);
        return;
    }

    for (int i = 0; i < file_count; i++) {
        lv_obj_t *btn = lv_button_create(file_list);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 60);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
        lv_obj_set_style_radius(btn, 5, 0);

        lv_obj_t *label = lv_label_create(btn);
        
        char text[256];
        if (files[i].is_dir) {
            snprintf(text, sizeof(text), LV_SYMBOL_DIRECTORY " %.63s", files[i].name);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFAA00), 0);
        } else {
            // Format file size
            char size_str[32];
            if (files[i].size < 1024) {
                snprintf(size_str, sizeof(size_str), "%zu B", files[i].size);
            } else if (files[i].size < 1024 * 1024) {
                snprintf(size_str, sizeof(size_str), "%.1f KB", files[i].size / 1024.0f);
            } else {
                snprintf(size_str, sizeof(size_str), "%.1f MB", files[i].size / (1024.0f * 1024.0f));
            }
            
            snprintf(text, sizeof(text), LV_SYMBOL_FILE " %.63s (%s)", files[i].name, size_str);
            lv_obj_set_style_text_color(label, lv_color_hex(0xCCCCCC), 0);
        }
        
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);

        // Store file index as user data
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, file_list_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, file_list_event_cb, LV_EVENT_LONG_PRESSED, NULL);
    }

    // Update status
    if (card != NULL) {
        char status_text[128];
        uint64_t card_size = ((uint64_t)card->csd.capacity) * card->csd.sector_size;
        snprintf(status_text, sizeof(status_text), "SD: %.1f GB | Files: %d", 
                 card_size / (1024.0 * 1024.0 * 1024.0), file_count);
        lv_label_set_text(status_label, status_text);
    }
}

void file_manager_refresh(void)
{
    scan_files();
    update_file_list();
    update_disk_space_label();
}

static void refresh_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    if (code == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Refreshing file list");
        file_manager_refresh();
    }
}

static void file_manager_gesture_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    
    // Log ALL events to see what's happening
    ESP_LOGI(TAG, "Event code: %d (PRESSED=%d, RELEASED=%d, GESTURE=%d)", 
             code, LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_GESTURE);
    
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        
        ESP_LOGI(TAG, "Gesture detected, direction: %d (LEFT=%d, RIGHT=%d, TOP=%d, BOTTOM=%d)", 
                 dir, LV_DIR_LEFT, LV_DIR_RIGHT, LV_DIR_TOP, LV_DIR_BOTTOM);
        
        if (dir == LV_DIR_RIGHT) {
            // Swipe right to return to audio player
            ESP_LOGI(TAG, "Swipe RIGHT detected, returning to player");
            file_manager_hide();
        } else if (dir == LV_DIR_LEFT) {
            // Swipe left to WiFi config
            ESP_LOGI(TAG, "Swipe LEFT detected, showing WiFi config");
            wifi_config_show();
        }
    }
}

static void file_list_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
    int file_idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    
    // Handle long press to show context menu
    if (code == LV_EVENT_LONG_PRESSED) {
        // Don't show context menu for "go up" button
        if (file_idx >= 0 && file_idx < file_count) {
            ESP_LOGI(TAG, "Long press detected on: %s", files[file_idx].name);
            show_context_menu(file_idx, btn);
        }
        return;
    }
    
    if (code == LV_EVENT_CLICKED) {
        // Check for "go up" button (index -1)
        if (file_idx == -1) {
            // Go up one directory
            if (current_path[0] != '\0') {
                char *last_slash = strrchr(current_path, '/');
                if (last_slash != NULL) {
                    // If this is a direct child of mount point, go to root
                    if (last_slash == current_path + strlen(MOUNT_POINT)) {
                        current_path[0] = '\0';  // Back to root
                    } else {
                        *last_slash = '\0';  // Truncate at last slash
                    }
                    ESP_LOGI(TAG, "Navigate up to: %s", current_path[0] ? current_path : "/");
                    file_manager_refresh();
                }
            }
            return;
        }
        
        if (file_idx >= 0 && file_idx < file_count) {
            // Check if it's a directory
            if (files[file_idx].is_dir) {
                // Navigate into directory
                char new_path[512];
                if (current_path[0] == '\0') {
                    // Currently in root
                    snprintf(new_path, sizeof(new_path), "%s/%s", MOUNT_POINT, files[file_idx].name);
                } else {
                    // In a subdirectory
                    snprintf(new_path, sizeof(new_path), "%s/%s", current_path, files[file_idx].name);
                }
                
                // Update current path
                strncpy(current_path, new_path, MAX_PATH_LEN - 1);
                current_path[MAX_PATH_LEN - 1] = '\0';
                
                ESP_LOGI(TAG, "Navigate to directory: %s", current_path);
                file_manager_refresh();
            } else {
                // For files, show file info or play
                ESP_LOGI(TAG, "File clicked: %s", files[file_idx].name);
                // Future: Add file preview or play functionality
            }
        }
    }
}

void file_manager_ui_init(lv_obj_t *parent)
{
    // Create file manager screen as an independent screen, not a child of parent
    file_manager_screen = lv_obj_create(NULL);  // NULL creates a new independent screen
    lv_obj_set_size(file_manager_screen, SUNTON_ESP32_LCD_WIDTH, SUNTON_ESP32_LCD_HEIGHT);
    lv_obj_set_style_bg_color(file_manager_screen, lv_color_hex(0x000000), 0);
    lv_obj_add_flag(file_manager_screen, LV_OBJ_FLAG_CLICKABLE);  // Enable event reception
    lv_obj_clear_flag(file_manager_screen, LV_OBJ_FLAG_SCROLLABLE);  // Disable scroll

    // Initialize to root directory
    current_path[0] = '\0';

    // Title
    lv_obj_t *title = lv_label_create(file_manager_screen);
    lv_label_set_text(title, "File Manager");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // Status bar
    status_label = lv_label_create(file_manager_screen);
    lv_label_set_text(status_label, "SD: Not mounted");
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF00), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 70);
    
    // Disk space label
    disk_space_label = lv_label_create(file_manager_screen);
    lv_label_set_text(disk_space_label, "");
    lv_obj_set_style_text_font(disk_space_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(disk_space_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(disk_space_label, LV_ALIGN_TOP_MID, 0, 100);

    // File list container (scrollable)
    file_list = lv_obj_create(file_manager_screen);
    lv_obj_set_size(file_list, SUNTON_ESP32_LCD_WIDTH - 40, 230);
    lv_obj_align(file_list, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(file_list, lv_color_hex(0x111111), 0);
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Enable vertical scrolling
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);

    // Button row (centered - only Format and Refresh)
    // Button row - only Refresh button
    int btn_width = 200;
    int start_x = (SUNTON_ESP32_LCD_WIDTH - btn_width) / 2;
    int btn_y = 390;

    // Refresh button
    refresh_btn = lv_button_create(file_manager_screen);
    lv_obj_set_size(refresh_btn, btn_width, 60);
    lv_obj_set_pos(refresh_btn, start_x, btn_y);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(0x0066AA), 0);
    lv_obj_set_style_radius(refresh_btn, 10, 0);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_set_style_text_font(refresh_label, &lv_font_montserrat_28, 0);
    lv_obj_center(refresh_label);
    
    // Add swipe gesture support to screen
    lv_obj_add_event_cb(file_manager_screen, file_manager_gesture_event_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(file_manager_screen, file_manager_gesture_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(file_manager_screen, file_manager_gesture_event_cb, LV_EVENT_RELEASED, NULL);

    ESP_LOGI(TAG, "File manager UI initialized");
}

void file_manager_show(void)
{
    if (file_manager_screen) {
        // Try to mount SD card if not mounted
        if (!sd_mounted) {
            file_manager_sd_init();
        }
        
        // Refresh file list
        file_manager_refresh();
        
        lv_obj_remove_flag(file_manager_screen, LV_OBJ_FLAG_HIDDEN);
        lv_screen_load(file_manager_screen);
        
        ESP_LOGI(TAG, "File manager shown");
    }
}

void file_manager_hide(void)
{
    if (file_manager_screen) {
        // Return to audio player screen
        lv_obj_t *audio_screen = audio_player_get_screen();
        if (audio_screen) {
            lv_screen_load(audio_screen);
            audio_player_show();  // Trigger auto-play if enabled
            ESP_LOGI(TAG, "Returned to audio player");
        }
    }
}
