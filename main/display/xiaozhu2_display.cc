#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"
#include "mbedtls/md.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <cctype>
#include <src/misc/cache/lv_cache.h>
#include <cmath>
#include <math.h>

#include "board.h"
#include "xiaozhu2_display.h"
#include "display.h"
#include "application.h"
#include <dirent.h>
#include <system_info.h>
#include "jpg/jpeg_to_image.h"

#define TAG "Xiaozhu2Display"

#define FFT_SIZE 512
static int current_heights[40] = {0};
static float avg_power_spectrum[FFT_SIZE/2]={-25.0f};

void Xiaozhu2Display::wallpaper_timer_cb(lv_timer_t *timer) {
    Xiaozhu2Display* board = (Xiaozhu2Display *) lv_timer_get_user_data(timer);
    if (!board) return;
    board->random_change_wallpaper();
}

void Xiaozhu2Display::wallpaper_start_auto_change(uint32_t interval_ms) {
    if (interval_ms == 0) {
        interval_ms = 1000 * 60 * 60;
    }
    wallpaper_switch_interval_ms_ = interval_ms;
    if (wallpaper_timer_ == nullptr) {
        wallpaper_timer_ = lv_timer_create(wallpaper_timer_cb, interval_ms, this);
    } else {
        lv_timer_set_period(wallpaper_timer_, interval_ms);
    }
}

void Xiaozhu2Display::SetupXiaozhu2UI() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_color(status_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_text_color(status_bar_, lvgl_theme->text_color(), 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(status_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(status_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    // 设置状态栏的内容垂直居中
    lv_obj_set_flex_align(status_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    
    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0); // 添加左边距，与前面的元素分隔

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    // lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    idle_screen_container_ = lv_obj_create(screen);
    lv_obj_set_pos(idle_screen_container_, 0, 0);
    lv_obj_set_size(idle_screen_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(idle_screen_container_, 0, 0);
    lv_obj_set_style_pad_all(idle_screen_container_, 0, 0);
    lv_obj_set_style_border_width(idle_screen_container_, 0, 0);
    lv_obj_set_style_pad_row(idle_screen_container_, 0, 0);
    lv_obj_set_style_border_color(idle_screen_container_, lvgl_theme->border_color(), 0);
    lv_obj_set_flag(idle_screen_container_, LV_OBJ_FLAG_HIDDEN, true);
    lv_obj_set_flag(idle_screen_container_, LV_OBJ_FLAG_SCROLLABLE, false);

    idle_screen_wallpaper_ = lv_img_create(idle_screen_container_);
    lv_obj_set_size(idle_screen_wallpaper_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(idle_screen_wallpaper_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(idle_screen_wallpaper_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_move_background(idle_screen_wallpaper_);

    idle_screen_status_label_ = lv_label_create(idle_screen_container_);
    lv_obj_set_pos(idle_screen_status_label_, LV_PCT(60), LV_PCT(20));
    lv_obj_set_size(idle_screen_status_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(idle_screen_status_label_, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_align(idle_screen_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(idle_screen_status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(idle_screen_status_label_, Lang::Strings::INITIALIZING);
    lv_obj_set_flag(idle_screen_status_label_, LV_OBJ_FLAG_HIDDEN, true);

    idle_screen_weather_label_ = lv_label_create(idle_screen_container_);
    lv_obj_set_size(idle_screen_weather_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(idle_screen_weather_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(idle_screen_weather_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(idle_screen_weather_label_, "");
    lv_obj_set_flag(idle_screen_weather_label_, LV_OBJ_FLAG_HIDDEN, true);

    idle_screen_content_ = lv_obj_create(idle_screen_container_);
    lv_obj_set_pos(idle_screen_content_, 0, LV_PCT(60));
    lv_obj_set_size(idle_screen_content_, LV_PCT(100), LV_PCT(40));
    lv_obj_set_scrollbar_mode(idle_screen_content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(idle_screen_content_, 0, 0);
    lv_obj_set_style_pad_all(idle_screen_content_, 0, 0);
    lv_obj_set_style_border_width(idle_screen_content_, 0, 0);
    lv_obj_set_style_bg_color(idle_screen_content_, lvgl_theme->chat_background_color(), 0);
    lv_obj_set_flex_flow(idle_screen_content_, LV_FLEX_FLOW_COLUMN); // 垂直布局（从上到下）
    lv_obj_set_flex_align(idle_screen_content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_AROUND);
    lv_obj_set_style_flex_main_place(idle_screen_content_, LV_FLEX_ALIGN_SPACE_EVENLY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_flex_cross_place(idle_screen_content_, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_flex_track_place(idle_screen_content_, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(idle_screen_content_, LV_OPA_TRANSP, 0);

    idle_screen_music_info_label_ = lv_label_create(idle_screen_content_);
    lv_label_set_text(idle_screen_music_info_label_, "");
    lv_obj_set_width(idle_screen_music_info_label_, width_);
    lv_label_set_long_mode(idle_screen_music_info_label_, LV_LABEL_LONG_SCROLL_CIRCULAR); // 自动换行
    lv_obj_set_style_text_align(idle_screen_music_info_label_, LV_TEXT_ALIGN_CENTER, 0); // 设置文本居中对齐
    lv_obj_set_style_text_color(idle_screen_music_info_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_flag(idle_screen_music_info_label_, LV_OBJ_FLAG_HIDDEN, true);

    idle_screen_music_lyrics_label_ = lv_label_create(idle_screen_content_);
    lv_label_set_text(idle_screen_music_lyrics_label_, "");
    lv_obj_set_width(idle_screen_music_lyrics_label_, width_ * 0.9);
    lv_label_set_long_mode(idle_screen_music_lyrics_label_, LV_LABEL_LONG_WRAP); // 自动换行
    lv_obj_set_style_text_align(idle_screen_music_lyrics_label_, LV_TEXT_ALIGN_CENTER, 0); // 设置文本居中对齐
    lv_obj_set_style_text_color(idle_screen_music_lyrics_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_flag(idle_screen_music_lyrics_label_, LV_OBJ_FLAG_HIDDEN, true);
}

void Xiaozhu2Display::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    SetupXiaozhu2UI();

    // Apply persisted device params after LVGL objects are created.
    Settings settings("device_params");
    auto params_json = settings.GetString("device_params", "{}");
    if (!params_json.empty() && params_json != "{}") {
        PreviewDeviceParams(params_json);
    }
}

Xiaozhu2Display::Xiaozhu2Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height,
                           int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
   : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 5;
    port_cfg.task_stack = 8192;
// #if CONFIG_SOC_CPU_CORES_NUM > 1
//     port_cfg.task_affinity = 1;
// #endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    fft_real = (float*)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    fft_imag = (float*)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    hanning_window_float = (float*)heap_caps_malloc(FFT_SIZE * sizeof(float), MALLOC_CAP_SPIRAM);
    
    // 创建窗函数
    for (int i = 0; i < FFT_SIZE; i++) {
        hanning_window_float[i] = 0.5 * (1.0 - cos(2.0 * M_PI * i / (FFT_SIZE - 1)));
    }
    
    if(audio_data==nullptr){
        audio_data=(int16_t*)heap_caps_malloc(sizeof(int16_t)*1152, MALLOC_CAP_SPIRAM);
        memset(audio_data,0,sizeof(int16_t)*1152);
    }
    if(frame_audio_data==nullptr){
        frame_audio_data=(int16_t*)heap_caps_malloc(sizeof(int16_t)*1152, MALLOC_CAP_SPIRAM);
        memset(frame_audio_data,0,sizeof(int16_t)*1152);
    }

    ESP_LOGI(TAG,"Initialize fft_input, audio_data, frame_audio_data, spectrum_data");    

    wallpaper_start_auto_change(3600 * 1000);
}

void Xiaozhu2Display::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // 如果静音状态改变，则更新图标
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time
    if (update_all || (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now())) {
        time_t now = time(NULL);
        struct tm* tm = localtime(&now);
        if (tm->tm_year >= 2025 - 1900) {
            char time_str[16];
            strftime(time_str, sizeof(time_str), "%H:%M  ", tm);
            if (app.GetDeviceState() == kDeviceStateIdle) SetStatus(time_str);
            {
                DisplayLockGuard lock(this);
                lv_obj_set_flag(idle_screen_status_label_, LV_OBJ_FLAG_HIDDEN, false);
                lv_label_set_text(idle_screen_status_label_, time_str);
                ApplyWeatherLabelLayout();
            }
        } else {
            ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm->tm_year);
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // 更新电池图标
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,    // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        if (low_battery_popup_ != nullptr) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // 如果低电量提示框隐藏，则显示
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // 如果低电量提示框显示，则隐藏
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // 每 10 秒更新一次网络图标
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // 升级固件时，不读取 4G 网络状态，避免占用 UART 资源
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
                // lv_label_set_text(idle_screen_network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void Xiaozhu2Display::SetWallpaper(const char* path) {
    DisplayLockGuard lock(this);
    if (idle_screen_wallpaper_ == nullptr) {
        return;
    }
    
    ESP_LOGI(TAG, "SetWallpaper: %s", path);
    lv_image_set_src(idle_screen_wallpaper_, path);
}

void Xiaozhu2Display::SetWallpaper(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (idle_screen_wallpaper_ == nullptr) {
        return;
    }
    
    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();

    lv_image_set_src(idle_screen_wallpaper_, img_dsc);
}

void Xiaozhu2Display::AddWallpaperToCollection(const std::string& key) {
    wallpaper_keys.push_back(key);
}

void Xiaozhu2Display::SetMusicInfoVisible(bool visible) {
    DisplayLockGuard lock(this);
    lv_obj_set_flag(idle_screen_music_info_label_, LV_OBJ_FLAG_HIDDEN, !visible);
}

void Xiaozhu2Display::SetMusicInfo(const char* text) {
    DisplayLockGuard lock(this);
    if (idle_screen_music_info_label_ == nullptr) {
        return;
    }
    lv_label_set_text(idle_screen_music_info_label_, text);
}

void Xiaozhu2Display::SetMusicLyricsVisible(bool visible) {
    DisplayLockGuard lock(this);
    lv_obj_set_flag(idle_screen_music_lyrics_label_, LV_OBJ_FLAG_HIDDEN, !visible);
}

void Xiaozhu2Display::SetMusicLyrics(const char* lyrics) {
    DisplayLockGuard lock(this);
    if (idle_screen_music_lyrics_label_ == nullptr) {
        return;
    }
    lv_label_set_text(idle_screen_music_lyrics_label_, lyrics);
}

static std::string HmacSha256Hex(const std::string& key, const std::string& data) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        ESP_LOGE("HMAC", "mbedtls_md_info_from_type failed");
        return {};
    }

    unsigned char out[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int ret = mbedtls_md_setup(&ctx, info, 1);
    if (ret != 0) {
        ESP_LOGE("HMAC", "mbedtls_md_setup failed: %d", ret);
        mbedtls_md_free(&ctx);
        return {};
    }

    ret = mbedtls_md_hmac_starts(&ctx,
                                 reinterpret_cast<const unsigned char*>(key.data()),
                                 key.size());
    if (ret != 0) {
        ESP_LOGE("HMAC", "mbedtls_md_hmac_starts failed: %d", ret);
        mbedtls_md_free(&ctx);
        return {};
    }

    ret = mbedtls_md_hmac_update(&ctx,
                                 reinterpret_cast<const unsigned char*>(data.data()),
                                 data.size());
    if (ret != 0) {
        ESP_LOGE("HMAC", "mbedtls_md_hmac_update failed: %d", ret);
        mbedtls_md_free(&ctx);
        return {};
    }

    ret = mbedtls_md_hmac_finish(&ctx, out);
    if (ret != 0) {
        ESP_LOGE("HMAC", "mbedtls_md_hmac_finish failed: %d", ret);
        mbedtls_md_free(&ctx);
        return {};
    }

    mbedtls_md_free(&ctx);

    std::string hex;
    hex.reserve(64);
    char buf[3];
    for (int i = 0; i < 32; ++i) {
        snprintf(buf, sizeof(buf), "%02x", out[i]);
        hex.append(buf);
    }
    return hex;
}

bool Xiaozhu2Display::GetWallpapers() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    // 1. 准备 auth 相关字段
    std::string mac = SystemInfo::GetMacAddress();
    int64_t ts = static_cast<int64_t>(time(nullptr));
    std::string ts_str = std::to_string(ts);

    std::string sign_payload = mac + ":" + ts_str;
    std::string sign_hex = HmacSha256Hex(CONFIG_WALLPAPER_SHARED_SECRET, sign_payload);

    ESP_LOGI(TAG, "Wallpapers auth mac=%s ts=%s sign=%s",
             mac.c_str(), ts_str.c_str(), sign_hex.c_str());

    // 2. 塞 HTTP 头
    http->SetHeader("X-Device-Id", mac.c_str());
    http->SetHeader("X-Device-Ts", ts_str.c_str());
    http->SetHeader("X-Device-Sign", sign_hex.c_str());

    std::string url = "http://192.168.2.26:8002/xiaozhi/wallpaper/device-batch";
    std::string data;  // 空 body
    std::string method = "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to get wallpapers, status code: %d", status_code);
        http->Close();
        return false;
    }

    std::string resp = http->ReadAll();
    http->Close();

    // ESP_LOGI(TAG, "Wallpapers response: %s", resp.c_str());

    // 3. 解析 JSON
    // { "data": [ { "id": 1, "fileKey": "http://..." }, ... ] }
    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return false;
    }

    cJSON* dataNode = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(dataNode)) {
        ESP_LOGE(TAG, "No 'data' array in response");
        cJSON_Delete(root);
        return false;
    }

    cJSON* item = nullptr;
    auto display = board.GetDisplay();
    cJSON_ArrayForEach(item, dataNode) {
        cJSON* idNode = cJSON_GetObjectItem(item, "id");
        cJSON* fileKeyNode = cJSON_GetObjectItem(item, "fileKey");
        if (!cJSON_IsNumber(idNode) || !cJSON_IsString(fileKeyNode)) {
            continue;
        }

        int id = idNode->valueint;
        const char* wfileKey = fileKeyNode->valuestring;

        ESP_LOGI(TAG, "Wallpaper: id=%d fileKey=%s", id, wfileKey);
        display->AddWallpaperToCollection(std::string(wfileKey));
    }

    cJSON_Delete(root);
    return true;
}

void Xiaozhu2Display::SetIdleScreenVisible(bool visible) {
    DisplayLockGuard lock(this);
    lv_obj_set_flag(idle_screen_container_, LV_OBJ_FLAG_HIDDEN, !visible);
}

void Xiaozhu2Display::StartFft(){
    ESP_LOGI(TAG, "Starting Xiaozhu2Display with periodic data updates");
    
    vTaskDelay(pdMS_TO_TICKS(5000));

    // 创建周期性更新任务
    fft_task_should_stop = false;  // 重置停止标志
    xTaskCreate(
        periodicUpdateTaskWrapper,
        "display_fft",      // 任务名称
        4096*2,             // 堆栈大小
        this,               // 参数
        1,                  // 优先级
        &fft_task_handle    // 保存到成员变量
    );
    
  
}

void Xiaozhu2Display::drawSpectrumIfReady() {
    if (fft_data_ready) {
        draw_spectrum(avg_power_spectrum, FFT_SIZE/2);
        fft_data_ready = false;
    }
}

void Xiaozhu2Display::periodicUpdateTaskWrapper(void* arg) {
    auto self = static_cast<Xiaozhu2Display*>(arg);
    self->periodicUpdateTask();
}

void Xiaozhu2Display::periodicUpdateTask() {
    ESP_LOGI(TAG, "Periodic update task started");
    
    if(canvas_==nullptr){
        create_canvas();
    }
    else{
        ESP_LOGI(TAG, "canvas already created");
    }
  

    auto music = Board::GetInstance().GetMusic();
        
    const TickType_t displayInterval = pdMS_TO_TICKS(40);  
    const TickType_t audioProcessInterval = pdMS_TO_TICKS(15); 
    
    TickType_t lastDisplayTime = xTaskGetTickCount();
    TickType_t lastAudioTime = xTaskGetTickCount();
    
    while (!fft_task_should_stop) {
        
        TickType_t currentTime = xTaskGetTickCount();
        
        
        if (currentTime - lastAudioTime >= audioProcessInterval) {
            if(music->GetAudioData() != nullptr) {
                readAudioData();  // 快速处理，不阻塞
            } else {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            lastAudioTime = currentTime;
        }
        
        // 显示刷新（30Hz）
        if (currentTime - lastDisplayTime >= displayInterval) {
            if (fft_data_ready) {
                DisplayLockGuard lock(this);
                drawSpectrumIfReady();
                lv_area_t refresh_area;
                refresh_area.x1 = 0;
                refresh_area.y1 = height_-100;
                refresh_area.x2 = canvas_width_ -1;
                refresh_area.y2 = height_ -1; // 只刷新频谱区域
                lv_obj_invalidate_area(canvas_, &refresh_area);
                //lv_obj_invalidate(canvas_);
                fft_data_ready = false;
                lastDisplayTime = currentTime;
            }   // 绘制操作
           
            
            // 更新FPS计数
            //FPS();
        }
        
        
        
        vTaskDelay(pdMS_TO_TICKS(10));
        // 短暂延迟
        
    }
    
    ESP_LOGI(TAG, "FFT display task stopped");
    fft_task_handle = nullptr;  // 清空任务句柄
    vTaskDelete(NULL);  // 删除当前任务
}



void Xiaozhu2Display::readAudioData(){
   
    auto music = Board::GetInstance().GetMusic();
    
    if(music->GetAudioData()!=nullptr){
        
    
        if(audio_display_last_update<=2){
            memcpy(audio_data,music->GetAudioData(),sizeof(int16_t)*1152);
            for(int i=0;i<1152;i++){
                frame_audio_data[i]+=audio_data[i];
            }
            audio_display_last_update++;
            
        }else{
            const int HOP_SIZE = 512;
            const int NUM_SEGMENTS = 1 + (1152 - FFT_SIZE) / HOP_SIZE;
    
            for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
                int start = seg * HOP_SIZE;
                if (start + FFT_SIZE > 1152) break;

        // 准备当前段数据
                for (int i = 0; i < FFT_SIZE; i++) {
                    int idx = start + i;
                    //float sample =frame_audio_data[idx] / 32768.0f;
                    float sample =frame_audio_data[idx] / 32768.0f;
                    fft_real[i] = sample * hanning_window_float[i];
                    fft_imag[i] = 0.0f;
                    
                }

                compute(fft_real, fft_imag, FFT_SIZE, true);
        
        // 计算功率谱并累加（双边）
                for (int i = 0; i < FFT_SIZE/2; i++) {
                    avg_power_spectrum[i] += fft_real[i] * fft_real[i]+fft_imag[i] * fft_imag[i]; // 功率 = 幅度平方
                }
            }
        
    // 计算平均值
            for (int i = 0; i < FFT_SIZE/2; i++) {
                avg_power_spectrum[i] /= NUM_SEGMENTS;
            }

        audio_display_last_update=0;
        //memcpy(spectrum_data, avg_power_spectrum, sizeof(float) * FFT_SIZE/2);
        fft_data_ready=true;

        //draw_spectrum(avg_power_spectrum, FFT_SIZE/2);
        memset(frame_audio_data,0,sizeof(int16_t)*1152);

        }
    }else{
            ESP_LOGI(TAG, "audio_data is nullptr");
            vTaskDelay(pdMS_TO_TICKS(500));
        }   
}

uint16_t Xiaozhu2Display::get_bar_color(int x_pos){

    static uint16_t color_table[40];
    static bool initialized = false;
    
    if (!initialized) {
        // 生成黄绿->黄->黄红的渐变
        for (int i = 0; i < 40; i++) {
            if (i < 20) {
                // 黄绿到黄：增加红色分量
                uint8_t r = static_cast<uint8_t>((i / 19.0f) * 31);
                color_table[i] = (r << 11) | (0x3F << 5);
            } else {
                // 黄到黄红：减少绿色分量
                uint8_t g = static_cast<uint8_t>((1.0f - (i - 20) / 19.0f * 0.5f) * 63);
                color_table[i] = (0x1F << 11) | (g << 5);
            }
        }
        initialized = true;
    }
    
    return color_table[x_pos];
 }


void Xiaozhu2Display::draw_spectrum(float *power_spectrum,int fft_size){
   
    
    const int bartotal=40;
    int bar_height;
    const int bar_max_height=canvas_height_-100;
    const int bar_width=240/bartotal;
    int x_pos=0;
    int y_pos = (canvas_height_) - 1;

    float magnitude[bartotal]={0};
    float max_magnitude=0;

    const float MIN_DB = -25.0f;
    const float MAX_DB = 0.0f;
    
    for (int bin = 0; bin < bartotal; bin++) {
        int start = bin * (fft_size / bartotal);
        int end = (bin+1) * (fft_size / bartotal);
        magnitude[bin] = 0;
        int count=0;
        for (int k = start; k < end; k++) {
            magnitude[bin] += sqrt(power_spectrum[k]);
            count++;
        }
        if(count>0){
            magnitude[bin] /= count;
        }
      

        if (magnitude[bin] > max_magnitude) max_magnitude = magnitude[bin];
    }


    magnitude[1]=magnitude[1]*0.6;
    magnitude[2]=magnitude[2]*0.7;
    magnitude[3]=magnitude[3]*0.8;
    magnitude[4]=magnitude[4]*0.8;
    magnitude[5]=magnitude[5]*0.9;
    /*
    if (bartotal >= 6) {
        magnitude[0] *= 0.3f; // 最低频
        magnitude[1] *= 1.1f;
        magnitude[2] *= 1.0f;
        magnitude[3] *= 0.9f;
        magnitude[4] *= 0.8f;
        magnitude[5] *= 0.7f;
        // 更高频率保持或进一步衰减
        for (int i = 6; i < bartotal; i++) {
            magnitude[i] *= 0.6f;
        }
    }
    */    

    for (int bin = 1; bin < bartotal; bin++) {
        
        if (magnitude[bin] > 0.0f && max_magnitude > 0.0f) {
            // 相对dB值：20 * log10(magnitude/ref_level)
            magnitude[bin] = 20.0f * log10f(magnitude[bin] / max_magnitude+ 1e-10);
        } else {
            magnitude[bin] = MIN_DB;
        }
        if (magnitude[bin] > max_magnitude) max_magnitude = magnitude[bin];
    }

    ClearScreen();
    
    for (int k = 1; k < bartotal; k++) {  // 跳过直流分量（k=0）
        x_pos=canvas_width_/bartotal*(k-1);
        float mag=(magnitude[k] - MIN_DB) / (MAX_DB - MIN_DB);
        mag = std::max(0.0f, std::min(1.0f, mag));
        bar_height=int(mag*(bar_max_height));
        
        int color=get_bar_color(k);
        draw_bar(x_pos,y_pos,bar_width,bar_height, color,k-1);
        //printf("x: %d, y: %d,\n", x_pos, bar_height);
    }

    
}

void Xiaozhu2Display::draw_bar(int x,int y,int bar_width,int bar_height,uint16_t color,int bar_index){

    const int block_space=2;
    const int block_x_size=bar_width-block_space;
    const int block_y_size=4;
    
    int blocks_per_col=(bar_height/(block_y_size+block_space));
    int start_x=(block_x_size+block_space)/2+x;
    
    if(current_heights[bar_index]<bar_height) 
    {
        current_heights[bar_index]=bar_height;
    }
    else{
        int fall_speed=2;
        current_heights[bar_index]=current_heights[bar_index]-fall_speed;
        if(current_heights[bar_index]>(block_y_size+block_space)) 
        draw_block(start_x,canvas_height_-current_heights[bar_index],block_x_size,block_y_size,color,bar_index);

    }
   
    draw_block(start_x,canvas_height_-1,block_x_size,block_y_size,color,bar_index);

    for(int j=1;j<blocks_per_col;j++){
        
        int start_y=j*(block_y_size+block_space);
        draw_block(start_x,canvas_height_-start_y,block_x_size,block_y_size,color,bar_index); 
        
    }
    

}

void Xiaozhu2Display::draw_block(int x,int y,int block_x_size,int block_y_size,uint16_t color,int bar_index){
    for (int row = y; row > y-block_y_size;row--) {
        // 一次绘制一行
        uint16_t* line_start = &canvas_buffer_[row * canvas_width_ + x];
        std::fill_n(line_start, block_x_size, color);
    }
}   

void Xiaozhu2Display::ClearScreen() {
    std::fill_n(canvas_buffer_, canvas_width_ * canvas_height_, 0x0000);
}

void Xiaozhu2Display::StopFft() {
    ESP_LOGI(TAG, "Stopping FFT display");
    
    // 停止FFT显示任务
    if (fft_task_handle != nullptr) {
        ESP_LOGI(TAG, "Stopping FFT display task");
        fft_task_should_stop = true;  // 设置停止标志
        
        // 等待任务停止（最多等待1秒）
        int wait_count = 0;
        while (fft_task_handle != nullptr && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (fft_task_handle != nullptr) {
            ESP_LOGW(TAG, "FFT task did not stop gracefully, force deleting");
            vTaskDelete(fft_task_handle);
            fft_task_handle = nullptr;
        } else {
            ESP_LOGI(TAG, "FFT display task stopped successfully");
        }
    }
    
    // 使用显示锁保护所有操作
    DisplayLockGuard lock(this);
    
    // 重置FFT状态变量
    fft_data_ready = false;
    audio_display_last_update = 0;
    
    // 重置频谱条高度
    memset(current_heights, 0, sizeof(current_heights));
    
    // 重置平均功率谱数据
    for (int i = 0; i < FFT_SIZE/2; i++) {
        avg_power_spectrum[i] = -25.0f;
    }
    
    // 删除FFT画布对象，让原始UI重新显示
    if (canvas_ != nullptr) {
        lv_obj_del(canvas_);
        canvas_ = nullptr;
        ESP_LOGI(TAG, "FFT canvas deleted");
    }
    
    // 释放画布缓冲区内存
    if (canvas_buffer_ != nullptr) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
        ESP_LOGI(TAG, "FFT canvas buffer freed");
    }
    
    // 重置画布尺寸变量
    canvas_width_ = 0;
    canvas_height_ = 0;
    
    ESP_LOGI(TAG, "FFT display stopped, original UI restored");
}

bool Xiaozhu2Display::random_change_wallpaper() {
    if (!idle_screen_wallpaper_ || wallpaper_keys.empty()) return false;
    auto current_key = current_wallpaper_key_;

    ssize_t count = wallpaper_keys.size();
    if (count == 1 && wallpaper_keys[0] == current_key) {
        return false;
    }

    ssize_t idx = 0;
    if (wallpaper_switch_mode_ == "random") {
        do {
            idx = esp_random() % count;
        } while (count > 1 && wallpaper_keys[idx] == current_key);
    } else if (wallpaper_switch_mode_ == "sequential") {
        auto it = std::find(wallpaper_keys.begin(), wallpaper_keys.end(), current_key);
        if (it != wallpaper_keys.end()) {
            idx = (std::distance(wallpaper_keys.begin(), it) + 1) % count;
        }
    } else {
        // Fallback to random for unknown mode
        do {
            idx = esp_random() % count;
        } while (count > 1 && wallpaper_keys[idx] == current_key);
    }


    // 当场请求壁纸
    auto& key = wallpaper_keys[idx];
    current_wallpaper_key_ = key;
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(4);
    std::string wallpaper_url = std::string(CONFIG_WALLPAPER_SERVER_URL_PREFIX) + "/" + key;

    if (!http->Open("GET", wallpaper_url)) {
        ESP_LOGE(TAG, "Failed to open URL: %s", wallpaper_url.c_str());
        return false;
    }
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Unexpected status code: %s", std::to_string(status_code).c_str());
    }

    size_t content_length = http->GetBodyLength();
    char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
    if (data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for image: %s", wallpaper_url.c_str());
        http->Close();
        return false;
    }

    size_t total_read = 0;
    while (total_read < content_length) {
        int ret = http->Read(data + total_read, content_length - total_read);
        if (ret < 0) {
            heap_caps_free(data);
            ESP_LOGE(TAG, "Failed to download image: %s", wallpaper_url.c_str());
            http->Close();
            return false;
        }
        if (ret == 0) {
            break;
        }
        total_read += ret;
    }

    http->Close();
    std::unique_ptr<LvglAllocatedImage> image;
    image = std::make_unique<LvglAllocatedImage>(data, content_length);

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();

    lv_image_set_src(idle_screen_wallpaper_, img_dsc);

    // 等比放大：取 max(scale_x, scale_y) 以铺满屏幕（可能裁切）
    const int32_t img_w = img_dsc->header.w;
    const int32_t img_h = img_dsc->header.h;
    if (img_w > 0 && img_h > 0) {
        // LV_SCALE_NONE 通常是 256（1.0x），用整数避免 float
        const int32_t scale_x = (LV_HOR_RES * (int32_t)LV_SCALE_NONE + img_w / 2) / img_w;
        const int32_t scale_y = (LV_VER_RES * (int32_t)LV_SCALE_NONE + img_h / 2) / img_h;
        const int32_t scale   = (scale_x > scale_y) ? scale_x : scale_y; // fill：取更大的那个

        lv_image_set_scale(idle_screen_wallpaper_, scale);
        lv_obj_center(idle_screen_wallpaper_);
    }
    return true;
}

bool Xiaozhu2Display::RandomChangeWallpaper() {
    DisplayLockGuard lock(this);
    return random_change_wallpaper();
}

void Xiaozhu2Display::SetWallpaperSwitchConfig(uint32_t interval_ms, const std::string& mode) {
    wallpaper_switch_mode_ = mode.empty() ? "random" : mode;
    std::transform(wallpaper_switch_mode_.begin(), wallpaper_switch_mode_.end(),
                   wallpaper_switch_mode_.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (wallpaper_switch_mode_ != "random" && wallpaper_switch_mode_ != "sequential") {
        wallpaper_switch_mode_ = "random";
    }
    ESP_LOGI(TAG, "配置壁纸自动切换，模式: %s, 间隔: %u ms", wallpaper_switch_mode_.c_str(), interval_ms);
    wallpaper_start_auto_change(interval_ms > 0 ? interval_ms : wallpaper_switch_interval_ms_);
}

void Xiaozhu2Display::SetWeatherInfo(const char* info) {
    DisplayLockGuard lock(this);
    if (idle_screen_weather_label_ == nullptr) {
        return;
    }
    lv_label_set_text(idle_screen_weather_label_, info);
}

void Xiaozhu2Display::create_canvas(){
    DisplayLockGuard lock(this);
    if (canvas_ != nullptr) {
        lv_obj_del(canvas_);
    }
    if (canvas_buffer_ != nullptr) {
        heap_caps_free(canvas_buffer_);
        canvas_buffer_ = nullptr;
    }

    int content_height = lv_obj_get_height(idle_screen_content_);
    canvas_width_ = width_;
    canvas_height_ = content_height;

    canvas_buffer_=(uint16_t*)heap_caps_malloc(canvas_width_ * canvas_height_ * sizeof(uint16_t), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (canvas_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer");
        return;
    }
    ESP_LOGI(TAG, "canvas buffer allocated successfully");
    canvas_ = lv_canvas_create(idle_screen_content_);
    lv_canvas_set_buffer(canvas_, canvas_buffer_, canvas_width_, canvas_height_, LV_COLOR_FORMAT_RGB565);
    ESP_LOGI(TAG,"width: %d, height: %d", width_, height_);

    

    lv_obj_set_pos(canvas_, 0, 0);
    lv_obj_set_size(canvas_, canvas_width_, canvas_height_);
    lv_canvas_fill_bg(canvas_, lv_color_make(0, 0, 0), LV_OPA_TRANSP);

    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    // lv_obj_move_foreground(canvas_);
    lv_obj_move_background(canvas_);


    ESP_LOGI(TAG, "canvas created successfully");    
}


void Xiaozhu2Display::compute(float* real, float* imag, int n, bool forward) {
    // 位反转排序
    int j = 0;
    for (int i = 0; i < n; i++) {
        if (j > i) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
        
        int m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    // FFT计算
    for (int s = 1; s <= (int)log2(n); s++) {
        int m = 1 << s;
        int m2 = m >> 1;
        float w_real = 1.0f;
        float w_imag = 0.0f;
        float angle = (forward ? -2.0f : 2.0f) * M_PI / m;
        float wm_real = cosf(angle);
        float wm_imag = sinf(angle);
        
        for (int j = 0; j < m2; j++) {
            for (int k = j; k < n; k += m) {
                int k2 = k + m2;
                float t_real = w_real * real[k2] - w_imag * imag[k2];
                float t_imag = w_real * imag[k2] + w_imag * real[k2];
                
                real[k2] = real[k] - t_real;
                imag[k2] = imag[k] - t_imag;
                real[k] += t_real;
                imag[k] += t_imag;
            }
            
            float w_temp = w_real;
            w_real = w_real * wm_real - w_imag * wm_imag;
            w_imag = w_temp * wm_imag + w_imag * wm_real;
        }
    }
    
    // 正向变换需要缩放
    if (forward) {
        for (int i = 0; i < n; i++) {
            real[i] /= n;
            imag[i] /= n;
        }
    }
    
}

void Xiaozhu2Display::SetTheme(Theme* theme) {
    LcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);

    // auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // lv_obj_set_style_text_color(idle_screen_status_label_, lvgl_theme->text_color(), 0);
    // lv_obj_set_style_text_color(idle_screen_music_info_label_, lvgl_theme->text_color(), 0);
    // lv_obj_set_style_text_color(idle_screen_music_lyrics_label_, lvgl_theme->text_color(), 0);    
}

void Xiaozhu2Display::ApplyWeatherLabelLayout() {
    if (idle_screen_weather_label_ == nullptr || idle_screen_status_label_ == nullptr || !weather_layout_initialized_) {
        return;
    }

    if (weather_position_ == "hidden") {
        lv_obj_set_flag(idle_screen_weather_label_, LV_OBJ_FLAG_HIDDEN, true);
        return;
    }

    lv_obj_set_flag(idle_screen_weather_label_, LV_OBJ_FLAG_HIDDEN, false);
    if (weather_position_ == "above") {
        lv_obj_align_to(
            idle_screen_weather_label_, idle_screen_status_label_, LV_ALIGN_OUT_TOP_MID,
            weather_offset_x_, weather_offset_y_ - weather_spacing_);
    } else if (weather_position_ == "below") {
        lv_obj_align_to(
            idle_screen_weather_label_, idle_screen_status_label_, LV_ALIGN_OUT_BOTTOM_MID,
            weather_offset_x_, weather_offset_y_ + weather_spacing_);
    }
}

void Xiaozhu2Display::PreviewDeviceParams(const std::string& params_json) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_ || idle_screen_status_label_ == nullptr || idle_screen_weather_label_ == nullptr) {
        ESP_LOGW(TAG, "Skip PreviewDeviceParams before SetupUI is complete");
        return;
    }

    auto json = cJSON_Parse(params_json.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Invalid device params JSON");
        return;
    }
    ESP_LOGI(TAG, "Preview device params: %s", params_json.c_str());

    auto clockPosition = cJSON_GetObjectItem(json, "clockPosition");
    auto clockOffsetX = cJSON_GetObjectItem(json, "clockOffsetX");
    auto clockOffsetY = cJSON_GetObjectItem(json, "clockOffsetY");
    bool clock_updated = false;
    if (clockPosition != nullptr && cJSON_IsString(clockPosition) &&
        clockOffsetX != nullptr && cJSON_IsString(clockOffsetX) &&
        clockOffsetY != nullptr && cJSON_IsString(clockOffsetY)) {
        auto offsetX = atoi(clockOffsetX->valuestring);
        auto offsetY = atoi(clockOffsetY->valuestring);
        if (strcmp(clockPosition->valuestring, "顶部居左") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_TOP_LEFT, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "顶部居中") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_TOP_MID, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "顶部居右") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_TOP_RIGHT, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "底部居左") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_BOTTOM_LEFT, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "底部居中") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_BOTTOM_MID, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "底部居右") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_BOTTOM_RIGHT, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "中间居左") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_LEFT_MID, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "中间居中") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_CENTER, offsetX, offsetY);
        } else if (strcmp(clockPosition->valuestring, "中间居右") == 0) {
            lv_obj_align(idle_screen_status_label_, LV_ALIGN_RIGHT_MID, offsetX, offsetY);
        }
        clock_updated = true;
    }

    auto clockColor = cJSON_GetObjectItem(json, "clockColor");
    if (clockColor != nullptr && cJSON_IsString(clockColor) &&
        clockColor->valuestring != nullptr && clockColor->valuestring[0] == '#') {
        uint32_t color_ = strtoul(clockColor->valuestring + 1, nullptr, 16); // 跳过 '#' 字符
        lv_color_t color = lv_color_make((color_ >> 16) & 0xFF, (color_ >> 8) & 0xFF, color_ & 0xFF);
        lv_obj_set_style_text_color(idle_screen_status_label_, color, 0);
        lv_obj_set_style_text_color(idle_screen_weather_label_, color, 0);
    }
    
    auto weatherPosition = cJSON_GetObjectItem(json, "weatherPosition");
    auto weatherOffsetX = cJSON_GetObjectItem(json, "weatherOffsetX");
    auto weatherOffsetY = cJSON_GetObjectItem(json, "weatherOffsetY");
    auto weatherSpacing = cJSON_GetObjectItem(json, "weatherSpacing");
    bool weather_updated = false;
    if (weatherPosition != nullptr && cJSON_IsString(weatherPosition) &&
        weatherOffsetX != nullptr && cJSON_IsString(weatherOffsetX) &&
        weatherOffsetY != nullptr && cJSON_IsString(weatherOffsetY)) {
        weather_position_ = weatherPosition->valuestring != nullptr ? weatherPosition->valuestring : "hidden";
        weather_offset_x_ = atoi(weatherOffsetX->valuestring);
        weather_offset_y_ = atoi(weatherOffsetY->valuestring);
        weather_spacing_ = 0;
        if (weatherSpacing != nullptr && cJSON_IsString(weatherSpacing) && weatherSpacing->valuestring != nullptr) {
            weather_spacing_ = atoi(weatherSpacing->valuestring);
        }
        weather_layout_initialized_ = true;
        weather_updated = true;
    }

    // weatherPosition 是相对时钟的布局；时钟或天气任一参数变化都需要重排天气标签。
    if (weather_updated || clock_updated) {
        ApplyWeatherLabelLayout();
    }

    cJSON_Delete(json);
}
