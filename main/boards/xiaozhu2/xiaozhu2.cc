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
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    SpiLcdDisplay* display_ = nullptr;
    Esp32Camera* camera_;
    PowerManager* power_manager_ = new PowerManager(GPIO_NUM_9);

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
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            app.ToggleChatState();
        });

        boot_button_.OnDoubleClick([this]() {
            // Settings settings(FIRST_BOOT_NS, true);
            // bool is_first_boot = settings.GetInt(FIRST_BOOT_KEY, 1) != 0;
            // if (is_first_boot) {
            //     ESP_LOGI(TAG, "首次启动，启用双击拍照功�?");
            //     auto camera = GetCamera();
            //     if (!camera->Capture()) {
            //         ESP_LOGE(TAG, "Camera capture failed");
            //     }
            //     settings.SetInt(FIRST_BOOT_KEY, 0);


            // } else {
            //     ESP_LOGI(TAG, "非首次启动，禁用双击拍照功能");
                auto& app = Application::GetInstance();
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
                    // GetAudioCodec()->SetOutputVolume(60);
                }
            // }
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

        volume_up_button_.OnDoubleClick([this]() {
            EnterWifiConfigMode();
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
            // GetAudioCodec()->SetOutputVolume(0);
            // GetDisplay()->ShowNotification(Lang::Strings::MUTED);

            Application::GetInstance().SipAnswer();
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
            display_ = new SpiLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);

        } else {
            // 竖屏模式
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY_1);
            esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1);
            display_ = new SpiLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH_1, DISPLAY_HEIGHT_1, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X_1, DISPLAY_MIRROR_Y_1, DISPLAY_SWAP_XY_1);
        }

    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.audio_speaker.set_gain", 
            "设置麦克风输入增益，范围0-100",
            PropertyList({
                Property("gain", kPropertyTypeInteger, 0, 100)
            }), 
            [this](const PropertyList& properties) -> ReturnValue {
                auto codec = GetAudioCodec();
                codec->SetInputGain(properties["gain"].value<int>());
                return true;
        });
        
        mcp_server.AddTool("self.call_9000", "呼叫9000", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            Application::GetInstance().SipCall("9000");
            return true;
        });
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

    }

	void InitializeController() { InitializeMCPController(); }

public:
    XiaoZhu2Board() 
    : boot_button_(BOOT_BUTTON_GPIO), volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeButtons();
        // InitializeSdCard();
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
        return nullptr;
    }
};

DECLARE_BOARD(XiaoZhu2Board);