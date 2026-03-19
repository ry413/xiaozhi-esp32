#include "wifi_board.h"
#include "audio/codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "assets/lang_config.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <wifi_station.h>
#include "power_manager.h"
#include "power_save_timer.h"
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "settings.h"
#include "xiaozhu2_display.h"
#include <esp32_music.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>
#include <esp_sleep.h>

#include <ssid_manager.h>
#include <cctype>

#define FIRST_BOOT_NS "boot_config"
#define FIRST_BOOT_KEY "is_first"


#define TAG "Xiaozhu2Board"

//控制器初始化函数声明
void InitializeMCPController();

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x01, 0x03);
        WriteReg(0x03, 0xf8);
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        uint8_t data = ReadReg(0x01);
        data = (data & ~(1 << bit)) | (level << bit);
        WriteReg(0x01, data);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    // Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus) 
        : BoxAudioCodec(i2c_bus, 
                       AUDIO_INPUT_SAMPLE_RATE, 
                       AUDIO_OUTPUT_SAMPLE_RATE,
                       AUDIO_I2S_GPIO_MCLK, 
                       AUDIO_I2S_GPIO_BCLK, 
                       AUDIO_I2S_GPIO_WS, 
                       AUDIO_I2S_GPIO_DOUT, 
                       AUDIO_I2S_GPIO_DIN,
                       GPIO_NUM_NC, 
                       AUDIO_CODEC_ES8311_ADDR, 
                       AUDIO_CODEC_ES7210_ADDR, 
                       AUDIO_INPUT_REFERENCE)
    {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        // if (enable) {
        //     pca9557_->SetOutputState(1, 1);
        // } else {
        //     pca9557_->SetOutputState(1, 0);
        // }
    }
};

class XiaoZhu2Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Xiaozhu2Display* display_ = nullptr;
    Esp32Camera* camera_;
    PowerManager* power_manager_ = new PowerManager(GPIO_NUM_9);
    PowerSaveTimer* power_save_timer_ = nullptr;
    Esp32Music* music_;
    bool block_sleep_when_music_playing_ = false;
    int wallpaper_switch_interval_ms_ = 60000;
    std::string wallpaper_switch_mode_ = "random";
    std::string local_music_play_end_action_ = "stop";
    std::string weather_region_ = "auto";

    void RefreshPowerSaveTimerState() {
        if (power_save_timer_ == nullptr) {
            return;
        }
        auto music = GetMusic();
        bool is_music_playing = (music != nullptr && music->IsPlaying());
        bool should_block_sleep = block_sleep_when_music_playing_ && is_music_playing;
        power_save_timer_->SetEnabled(!should_block_sleep);
    }

    int ParseDurationSeconds(const cJSON* item, int default_value) {
        if (item == nullptr || !cJSON_IsString(item) || item->valuestring == nullptr) {
            return default_value;
        }

        std::string value = item->valuestring;
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
            start++;
        }
        value = value.substr(start);
        if (value.empty()) {
            return default_value;
        }

        std::string lower = value;
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lower == "off" || lower == "never" || lower == "none" || lower == "no_sleep") {
            return -1;
        }

        int multiplier = 1;
        if (!std::isdigit(static_cast<unsigned char>(value.back()))) {
            char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(value.back())));
            value.pop_back();
            if (unit == 's') {
                multiplier = 1;
            } else if (unit == 'm') {
                multiplier = 60;
            } else if (unit == 'h') {
                multiplier = 3600;
            } else {
                return default_value;
            }
        }

        if (value.empty()) {
            return default_value;
        }

        char* end_ptr = nullptr;
        long number = strtol(value.c_str(), &end_ptr, 10);
        if (end_ptr == value.c_str() || *end_ptr != '\0' || number <= 0) {
            return default_value;
        }

        return static_cast<int>(number) * multiplier;
    }

    void ApplyPowerTimersFromParams(const std::string& params_json) {
        int screen_off_seconds = -1;
        int auto_power_off_seconds = -1;
        block_sleep_when_music_playing_ = false;

        auto* json = cJSON_Parse(params_json.c_str());
        if (json != nullptr) {
            screen_off_seconds = ParseDurationSeconds(cJSON_GetObjectItem(json, "screenOffTime"), -1);
            auto_power_off_seconds = ParseDurationSeconds(cJSON_GetObjectItem(json, "autoPowerOff"), -1);
            int switch_interval_seconds =
                ParseDurationSeconds(cJSON_GetObjectItem(json, "switchInterval"), 60);
            if (switch_interval_seconds > 0) {
                wallpaper_switch_interval_ms_ = switch_interval_seconds * 1000;
            }

            auto* switch_mode = cJSON_GetObjectItem(json, "switchMode");
            if (switch_mode != nullptr && cJSON_IsString(switch_mode) && switch_mode->valuestring != nullptr) {
                wallpaper_switch_mode_ = switch_mode->valuestring;
            }

            auto* local_music_end = cJSON_GetObjectItem(json, "localMusicEnd");
            if (local_music_end != nullptr && cJSON_IsString(local_music_end) && local_music_end->valuestring != nullptr) {
                local_music_play_end_action_ = local_music_end->valuestring;
            }

            auto* music_during = cJSON_GetObjectItem(json, "musicDuring");
            if (music_during != nullptr && cJSON_IsString(music_during) && music_during->valuestring != nullptr) {
                std::string value = music_during->valuestring;
                for (char& ch : value) {
                    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                }
                // If musicDuringValue == "no_sleep", block sleep while music is playing.
                block_sleep_when_music_playing_ = (value == "no_sleep");
            }

            auto* weather_region_item = cJSON_GetObjectItem(json, "weatherRegion");
            if (weather_region_item != nullptr && cJSON_IsString(weather_region_item) && weather_region_item->valuestring != nullptr) {
                weather_region_ = weather_region_item->valuestring;
            }
            cJSON_Delete(json);
        } else {
            ESP_LOGW(TAG, "ApplyPowerTimersFromParams ignored invalid JSON, keep defaults");
        }

        if (display_ != nullptr) {
            display_->SetWallpaperSwitchConfig(static_cast<uint32_t>(wallpaper_switch_interval_ms_),
                                               wallpaper_switch_mode_);
        }

        // 如果永不熄屏, 那么自动关机也永不触发
        if (screen_off_seconds == -1) {
            auto_power_off_seconds = -1;
        }

        if (auto_power_off_seconds != -1 && screen_off_seconds != -1) {
            auto_power_off_seconds += screen_off_seconds;
        }

        if (power_save_timer_ != nullptr) {
            delete power_save_timer_;
            power_save_timer_ = nullptr;
        }

        power_save_timer_ = new PowerSaveTimer(-1, screen_off_seconds, auto_power_off_seconds);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "进入睡眠模式，屏幕将关闭");
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "退出睡眠模式，屏幕将打开");
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "进入深度睡眠模式");
            GetDisplay()->SetPowerSaveMode(true);
            esp_deep_sleep_start();
        });

        RefreshPowerSaveTimerState();

        ESP_LOGI(TAG, "配置熄屏时间: %d秒, 自动关机时间: %d秒, 音乐播放时%s睡眠", 
                 screen_off_seconds, auto_power_off_seconds, block_sleep_when_music_playing_ ? "阻止" : "允许");
    }

    void InitializePowerSaveTimer() {
        ApplyPowerTimersFromParams(GetDeviceParams());
        ESP_LOGI(TAG, "Power save timer initialized");
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnDoubleClick([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            Settings settings(FIRST_BOOT_NS, true);
            bool is_first_boot = settings.GetInt(FIRST_BOOT_KEY, 1) != 0;
            if (is_first_boot) {
                ESP_LOGI(TAG, "首次启动，启用双击拍照功�?");
                auto camera = GetCamera();
                if (!camera->Capture()) {
                    ESP_LOGE(TAG, "Camera capture failed");
                }
                settings.SetInt(FIRST_BOOT_KEY, 0);


            } else {
                ESP_LOGI(TAG, "非首次启动，禁用双击拍照功能");
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
                    GetAudioCodec()->SetOutputVolume(60);
                }
            }
        });

        boot_button_.OnLongPress([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            EnterWifiConfigMode();
        });

        volume_up_button_.OnClick([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
            codec->SetOutputVolume(volume);

        });

        volume_up_button_.OnLongPress([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            // GetAudioCodec()->SetOutputVolume(100);
            // GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_down_button_.OnLongPress([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        volume_down_button_.OnDoubleClick([this]() {
            if (power_save_timer_ != nullptr) {
                power_save_timer_->WakeUp();
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateAudioTesting) {
                app.GetAudioService().EnableAudioTesting(true);
                app.SetDeviceState(kDeviceStateAudioTesting);
            } else {
                app.GetAudioService().EnableAudioTesting(false);
                app.SetDeviceState(kDeviceStateIdle);
            }
        });
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始�?
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        // pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);

        Settings settings("lcd_display", true);
        bool is_landscape = settings.GetInt("lcd_mode", 1) != 0;

        if(is_landscape) {
            // 横屏模式
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
            display_ = new Xiaozhu2Display(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        } else {
            // 竖屏模式
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY_1);
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1);
            display_ = new Xiaozhu2Display(panel_io, panel,
                                        DISPLAY_WIDTH_1, DISPLAY_HEIGHT_1, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1, DISPLAY_SWAP_XY_1);
        }

    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddUserOnlyTool("self.audio_speaker.set_gain", 
            "设置麦克风输入增益，范围0-100",
            PropertyList({
                Property("gain", kPropertyTypeInteger, 0, 100)
            }), 
            [this](const PropertyList& properties) -> ReturnValue {
                auto codec = GetAudioCodec();
                codec->SetInputGain(properties["gain"].value<int>());
                return true;
        });

        mcp_server.AddUserOnlyTool("self.clear_wifi_credentials",
            "清除所有已保存的WiFi网络凭据，下次启动时需要重新配置WiFi。",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                SsidManager::GetInstance().Clear();
                ESP_LOGI(TAG, "Cleared all WiFi credentials");
                return true;
        });
        
        auto music = GetMusic();
        if (music) {
            mcp_server.AddTool("self.music.play_song",
                "播放指定的歌曲。当用户要求播放音乐时通常使用此工具，会在线搜索歌曲并开始流式播放。\n"
                "参数:\n"
                "  `song_name`: 要播放的歌曲名称（可选，默认为空字符串）。\n"
                "  `artist_name`: 要播放的歌曲艺术家名称（可选，默认为空字符串）。两个参数至少要有一个，会一起用于模糊搜索。\n"
                "返回:\n"
                "  播放状态信息，不需确认，立刻播放歌曲。",
                PropertyList({
                    Property("song_name", kPropertyTypeString, ""),
                    Property("artist_name", kPropertyTypeString, "")
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto song_name = properties["song_name"].value<std::string>();
                    auto artist_name = properties["artist_name"].value<std::string>();
                    
                    if (!music_->Download(song_name, artist_name)) {
                        return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
                    }
                    auto download_result = music_->GetDownloadResult();
                    ESP_LOGI(TAG, "Music details result: %s", download_result.c_str());
                    return "{\"success\": true, \"message\": \"音乐开始播放\"}";
                });

            mcp_server.AddTool("self.radio.search_radio_station",
                "搜索网络电台并返回电台列表。所有参数都是子串匹配，不是模糊搜索，要很注意筛选项过多可能导致无结果。\n"
                "参数:\n"
                "  `tag`: 电台的tag（可选），类似 'news' 或 'music'等。\n"
                "  `province`: 电台所在省份，比如‘北京’‘广东’等，只可使用省份名。（可选，留空则不按省份筛选）。\n"
                "  `language`: 电台的语言（可选，默认为‘chinese’）。\n"
                "  `order_by_votes`: 是否按点赞数排序（可选，默认为true）。\n"
                "  `name_keyword`: 电台名称关键词，此参数最容易导致过于严格而搜不到结果（可选，留空则不按名称关键词筛选）。\n"
                "  `offset`: 返回的列表的起始偏移，用于在同样的筛选条件下想查看排序更后的结果（可选，留空则0偏移）。\n"
                "返回:\n"
                "  搜索结果的电台列表JSON字符串\n",
                PropertyList({
                    Property("tag",          kPropertyTypeString, ""),
                    Property("province",     kPropertyTypeString, ""),
                    Property("language",     kPropertyTypeString, "chinese"),
                    Property("order_by_votes", kPropertyTypeBoolean, true),
                    Property("name_keyword", kPropertyTypeString, ""),
                    Property("offset", kPropertyTypeInteger, 0),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto tag            = properties["tag"].value<std::string>();
                    auto province       = properties["province"].value<std::string>();
                    auto language       = properties["language"].value<std::string>();
                    auto order_by_votes = properties["order_by_votes"].value<bool>();
                    auto name_keyword   = properties["name_keyword"].value<std::string>();
                    auto offset         = properties["offset"].value<int>();
                    std::string stations_json;
                    if (!music_->SearchRadioStations(tag,
                                                    language,
                                                    province,
                                                    order_by_votes,
                                                    name_keyword,
                                                    /*limit=*/5,
                                                    offset,
                                                    &stations_json)) {
                        display_->SetChatMessage("system", "电台搜索失败");
                        return "{\"success\": false, \"message\": \"搜索失败\"}";
                    }

                    // 包一层 success/stations
                    std::string result = std::string("{\"success\": true, \"message\": ")
                                    + stations_json + "}";

                    return result;
                });

            mcp_server.AddTool("self.radio.play_radio_station",
                "根据给定的电台名称和流地址开始播放电台。\n"
                "当用户在看到搜索结果之后说“播放某一个电台”时使用此工具。\n"
                "参数应该使用 `self.radio.search_radio_station` 返回的字段。",
                PropertyList({
                    Property("station_name", kPropertyTypeString),  // 用于显示给用户看的名字
                    Property("stream_url",   kPropertyTypeString),  // 直接可播放的 URL
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto station_name = properties["station_name"].value<std::string>();
                    auto stream_url   = properties["stream_url"].value<std::string>();

                    if (!music_->PlayRadioStream(station_name, stream_url)) {
                        display_->SetChatMessage("system", "无法播放指定电台");
                        return "{\"success\": false, \"message\": \"播放失败\"}";
                    }
                    return "{\"success\": true, \"message\": \"电台开始播放\"}";
                });

            mcp_server.AddTool(
                "self.radio.stop_radio",
                "彻底停止播放电台。\n",
                PropertyList(),
                [this](const PropertyList& properties) {
                if (!music_->StopStreaming()) {
                    return "{\"success\": false, \"message\": \"停止失败\"}";
                }
                music_->last_play_radio_name_ = "";
                music_->last_play_radio_url_ = "";
                return "{\"success\": true, \"message\": \"停止成功\"}";
                });


            mcp_server.AddTool(
                "self.radio.resume_radio",
                "恢复上次播放的网络电台。只有当用户有类似’继续播放刚刚的电台‘的意图时才使用此工具。\n",
                PropertyList(),
                [this](const PropertyList& properties) {
                if (music_->last_play_radio_name_.empty()) {
                    return "{\"success\": false, \"message\": \"不存在已暂停的电台\"}";
                }
                if (!music_->PlayRadioStream(music_->last_play_radio_name_, music_->last_play_radio_url_)) {
                    return "{\"success\": false, \"message\": \"播放失败\"}";
                }
                return "{\"success\": true, \"message\": \"电台恢复播放\"}";
                });
            
            mcp_server.AddTool("self.music.get_local_songs",
                "获取本地SD卡中存储的音乐文件列表。\n"
                "返回:\n"
                "  播放状态信息，不需确认，立刻播放歌曲。",
                PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto songs = music_->GetLocalMp3Songs();
                    cJSON* root = cJSON_CreateObject();
                    cJSON* arr  = cJSON_CreateArray();

                    for (auto& s : songs) {
                        cJSON_AddItemToArray(arr, cJSON_CreateString(s.c_str()));
                    }

                    cJSON_AddBoolToObject(root, "success", true);
                    cJSON_AddItemToObject(root, "message", arr);

                    // 转成字符串
                    char* json_str = cJSON_PrintUnformatted(root);
                    std::string result = json_str ? json_str : "{\"success\": false, \"message\": \"本地未检测到音乐文件\"}";

                    // 清理
                    if (json_str) free(json_str);
                    cJSON_Delete(root);

                    return result;
                });

            mcp_server.AddTool("self.music.play_local_music",
                "播放本地音乐。当只有当用户明确要求播放本地音乐时使用此工具，可以用get_local_songs来获取歌曲名列表。\n"
                "参数:\n"
                "  `song_name`: 想播放的歌曲的完整文件名，包括后缀名。（可选，留空则为随机播放）。\n"
                "返回:\n"
                "  播放状态信息，不需确认，立刻播放歌曲。",
                PropertyList({
                    Property("song_name", kPropertyTypeString),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto song_name = properties["song_name"].value<std::string>();
                    
                    if (!music_->PlayLocalFile(song_name)) {
                        display_->SetChatMessage("system", "无法播放指定歌曲");
                        return "{\"success\": false, \"message\": \"播放失败\"}";
                    }
                    return "{\"success\": true, \"message\": \"本地音乐开始播放\"}";
                });
            mcp_server.AddTool("self.system.change_wallpaper", "随机更换壁纸",
                PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    if (display_->RandomChangeWallpaper()) {
                        auto& app = Application::GetInstance();
                        if (app.GetDeviceState() == kDeviceStateListening || app.GetDeviceState() == kDeviceStateSpeaking) {
                            display_->SetIdleScreenVisible(true);
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            display_->SetIdleScreenVisible(false);
                        }
                        return "{\"success\": false, \"message\": \"更换完成\"}";
                    } else {
                        return "{\"success\": false, \"message\": \"更换失败\"}";
                    }
                });
    
        }
    }

    void InitializeSdCard() {
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 5,
            .allocation_unit_size = 16 * 1024
        };
        sdmmc_card_t* card;
        const char mount_point[] = SD_CARD_MOUNT_POINT;

        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

        slot_config.width = 1;
        slot_config.clk = SD_CARD_MMC_PIN_CLK;
        slot_config.cmd = SD_CARD_MMC_PIN_CMD;
        slot_config.d0 = SD_CARD_MMC_PIN_D0;

        slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);
        if (ret != ESP_OK) {
            if (ret == ESP_FAIL) {
                ESP_LOGE(TAG, "Failed to mount filesystem. "
                        "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
            } else {
                ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                        "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
            }
            return;
        }
        ESP_LOGI(TAG, "SD card mounted successfully.");

        sdmmc_card_print_info(stdout, card);

        music_->SetLocalMusicDir(SD_CARD_MOUNT_POINT"/Music/");
        music_->InitializeLocalMusicFiles();
    }

	void InitializeController() { InitializeMCPController(); }

public:
    XiaoZhu2Board() 
    : boot_button_(BOOT_BUTTON_GPIO), volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        music_ = new Esp32Music();
        InitializePowerSaveTimer();
        InitializeSdCard();
		InitializeController();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    	gpio_reset_pin(PA_ENABLE_GPIO);
	    gpio_set_direction(PA_ENABLE_GPIO,GPIO_MODE_OUTPUT);
	    gpio_set_level(PA_ENABLE_GPIO,1);
        gpio_set_direction(VOLUME_UP_BUTTON_GPIO,GPIO_MODE_OUTPUT);
	    gpio_set_level(VOLUME_UP_BUTTON_GPIO,1);
	}

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging)  override {
        static bool last_discharging = false;
        RefreshPowerSaveTimerState();
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            last_discharging = discharging;
        }
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        return true;
    }

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        // return camera_;
        return nullptr;
    }

    virtual Esp32Music* GetMusic() override {
        return music_;
    }

    virtual void SetDeviceParams(const std::string& params_json) override {
        Settings settings("device_params", true);
        cJSON* root = cJSON_Parse(params_json.c_str());
        if (!root) {
            ESP_LOGE(TAG, "Failed to parse device params JSON");
            return;
        }
        ESP_LOGI(TAG, "Save device params JSON: %s", params_json.c_str());
        settings.EraseKey("device_params");
        settings.SetString("device_params", params_json);
        cJSON_Delete(root);

        // Apply immediately if UI is already initialized.
        if (display_ != nullptr && display_->IsSetupUICalled()) {
            display_->PreviewDeviceParams(params_json);
        }
        ApplyPowerTimersFromParams(params_json);
    }

    virtual std::string GetDeviceParams() override {
        Settings settings("device_params");
        return settings.GetString("device_params", "{}");
    }

    virtual std::string GetWeatherRegion() override {
        size_t last_pipe = weather_region_.rfind('|');
        if (last_pipe != std::string::npos) {
            return weather_region_.substr(last_pipe + 1);
        }
        return weather_region_;
    }
};

DECLARE_BOARD(XiaoZhu2Board);

