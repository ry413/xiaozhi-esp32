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
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include "settings.h"
#include "xiaozhu2_display.h"
#include <esp32_music.h>
#include <sdmmc_cmd.h>
#include <esp_vfs_fat.h>
#include <driver/sdmmc_host.h>


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
    // i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Xiaozhu2Display* display_ = nullptr;
    // Pca9557* pca9557_;
    Esp32Camera* camera_;
    PowerManager* power_manager_ = new PowerManager(GPIO_NUM_9);
    Esp32Music* music_;

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

        // Initialize PCA9557
        // pca9557_ = new Pca9557(i2c_bus_, 0x19);
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
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnDoubleClick([this]() {
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
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            EnterWifiConfigMode();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
            codec->SetOutputVolume(volume);

        });

        volume_up_button_.OnLongPress([this]() {
            ESP_LOGW(TAG, "volume_up_button_.OnLongPress!");
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume/10));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        volume_down_button_.OnDoubleClick([this]() {
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
    
    void InitializeCamera() {
        // Open camera power
        // pca9557_->SetOutputState(2, 0);

        // camera_config_t config = {};
        // config.ledc_channel = LEDC_CHANNEL_2;  // LEDC通道选择  用于生成XCLK时钟 但是S3不用
        // config.ledc_timer = LEDC_TIMER_2; // LEDC timer选择  用于生成XCLK时钟 但是S3不用
        // config.pin_d0 = CAMERA_PIN_D0;
        // config.pin_d1 = CAMERA_PIN_D1;
        // config.pin_d2 = CAMERA_PIN_D2;
        // config.pin_d3 = CAMERA_PIN_D3;
        // config.pin_d4 = CAMERA_PIN_D4;
        // config.pin_d5 = CAMERA_PIN_D5;
        // config.pin_d6 = CAMERA_PIN_D6;
        // config.pin_d7 = CAMERA_PIN_D7;
        // config.pin_xclk = CAMERA_PIN_XCLK;
        // config.pin_pclk = CAMERA_PIN_PCLK;
        // config.pin_vsync = CAMERA_PIN_VSYNC;
        // config.pin_href = CAMERA_PIN_HREF;
        // config.pin_sccb_sda = -1;   // 这里�?-1 表示使用已经初始化的I2C接口
        // config.pin_sccb_scl = CAMERA_PIN_SIOC;
        // config.sccb_i2c_port = 1;
        // config.pin_pwdn = CAMERA_PIN_PWDN;
        // config.pin_reset = CAMERA_PIN_RESET;
        // config.xclk_freq_hz = XCLK_FREQ_HZ;
        // config.pixel_format = PIXFORMAT_RGB565;
        // config.frame_size = FRAMESIZE_VGA;
        // config.jpeg_quality = 9;
        // config.fb_count = 1;
        // config.fb_location = CAMERA_FB_IN_PSRAM;
        // config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        // camera_ = new Esp32Camera(config);
    }

	void InitializeController() { InitializeMCPController(); }

public:
    XiaoZhu2Board() 
    : boot_button_(BOOT_BUTTON_GPIO), volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        InitializeSdCard();
        InitializeCamera();
		InitializeController();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    	gpio_reset_pin(PA_ENABLE_GPIO);
	    gpio_set_direction(PA_ENABLE_GPIO,GPIO_MODE_OUTPUT);
	    gpio_set_level(PA_ENABLE_GPIO,1);
        gpio_set_direction(VOLUME_UP_BUTTON_GPIO,GPIO_MODE_OUTPUT);
	    gpio_set_level(VOLUME_UP_BUTTON_GPIO,1);
        music_ = new Esp32Music();
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
};

DECLARE_BOARD(XiaoZhu2Board);

