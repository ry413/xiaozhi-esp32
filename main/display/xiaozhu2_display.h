#ifndef XIAOZHU2_DISPLAY_H
#define XIAOZHU2_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"
#include "lcd_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>
#include <vector>

// 壁纸相关
#define WALLPAPER_MAX_COUNT 64
#define WALLPAPER_MAX_PATH  128
typedef struct {
    char  paths[WALLPAPER_MAX_COUNT][WALLPAPER_MAX_PATH];
    uint32_t count;
    int32_t last_index;   // 上一次用的
} wallpaper_list_t;

typedef struct {
    lv_obj_t *img;
    wallpaper_list_t *list;
} wallpaper_ctx_t;

class Xiaozhu2Display : public LcdDisplay {
protected:
    lv_obj_t* idle_screen_container_ = nullptr;
    lv_obj_t* idle_screen_wallpaper_ = nullptr;
    lv_obj_t* idle_screen_status_bar_ = nullptr;
    lv_obj_t* idle_screen_status_label_ = nullptr;
    lv_obj_t* idle_screen_weather_label_ = nullptr;
    lv_obj_t* idle_screen_mute_label_ = nullptr;
    lv_obj_t* idle_screen_network_label_ = nullptr;
    lv_obj_t* idle_screen_content_ = nullptr;
    lv_obj_t* idle_screen_music_info_label_ = nullptr;
    lv_obj_t* idle_screen_music_lyrics_label_ = nullptr;
    std::string weather_position_ = "hidden";
    int weather_offset_x_ = 0;
    int weather_offset_y_ = 0;
    int weather_spacing_ = 0;
    bool weather_layout_initialized_ = false;
    
    std::unique_ptr<LvglImage> preview_wallpaper_cached_ = nullptr;
    std::vector<std::string> wallpaper_keys;
    std::string current_wallpaper_key_;
    lv_timer_t* wallpaper_timer_ = nullptr;
    uint32_t wallpaper_switch_interval_ms_ = 3600 * 1000;
    std::string wallpaper_switch_mode_ = "random";
    
    void SetupXiaozhu2UI();

    // 壁纸
    static void wallpaper_timer_cb(lv_timer_t *timer);
    void wallpaper_start_auto_change(uint32_t interval_ms);
    bool random_change_wallpaper();
    
    // 一堆FFT的东西
    lv_obj_t* canvas_ = nullptr;
    uint16_t* canvas_buffer_ = nullptr;
    void create_canvas();
    uint16_t get_bar_color(int x_pos);
    void draw_spectrum(float *power_spectrum,int fft_size);
    void draw_bar(int x,int y,int bar_width,int bar_height,uint16_t color,int bar_index);
    void draw_block(int x,int y,int block_x_size,int block_y_size,uint16_t color,int bar_index);
    int canvas_width_;
    int canvas_height_;
    int audio_display_last_update = 0;
    std::atomic<bool> fft_task_should_stop = false;  // FFT任务停止标志
    TaskHandle_t fft_task_handle = nullptr;          // FFT任务句柄
    float* fft_real;
    float* fft_imag;
    float* hanning_window_float;
    int16_t* audio_data=nullptr;
    int16_t* frame_audio_data=nullptr;
    uint32_t last_fft_update = 0;
    bool fft_data_ready = false;
    float* spectrum_data=nullptr;
    void compute(float* real, float* imag, int n, bool forward);
    void periodicUpdateTask();
    static void periodicUpdateTaskWrapper(void* arg);
    void drawSpectrumIfReady();
    void readAudioData();
    void ApplyWeatherLabelLayout();

public:
    Xiaozhu2Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
    virtual void SetEmotion(const char* emotion) override {};
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetTheme(Theme* theme) override;
    virtual void SetupUI() override;

    virtual bool GetWallpapers() override;
    virtual void SetIdleScreenVisible(bool visible) override;
    virtual void SetWallpaper(const char* path) override;
    virtual void SetWallpaper(std::unique_ptr<LvglImage> image) override;
    virtual void AddWallpaperToCollection(const std::string& url) override;
    virtual bool RandomChangeWallpaper() override;
    virtual void SetWallpaperSwitchConfig(uint32_t interval_ms, const std::string& mode) override;
    virtual void SetWeatherInfo(const char* info) override;

    virtual void SetMusicInfoVisible(bool visible) override;
    virtual void SetMusicInfo(const char* text) override;
    virtual void SetMusicLyricsVisible(bool visible) override;
    virtual void SetMusicLyrics(const char* lyrics) override;
    virtual void StartFft() override;
    virtual void ClearScreen() override;
    virtual void StopFft() override;
    virtual void PreviewDeviceParams(const std::string& params_json) override;
};

#endif // XIAOZHU2_DISPLAY_H
