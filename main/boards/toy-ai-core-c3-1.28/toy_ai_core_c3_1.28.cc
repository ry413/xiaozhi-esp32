#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/oled_display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "mcp_server.h"
#include "press_to_talk_mcp_tool.h"


#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>

#include "display/lcd_display.h"
#include "assets/lang_config.h"
#include "system_reset.h"
#include <esp_lcd_panel_io.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <esp_io_expander_tca9554.h>
#include <driver/spi_common.h>
#include "i2c_device.h"
#include <esp_timer.h>
#include <esp_lcd_gc9a01.h>


#define TAG "Toy_AI_Core_C3_1_28"


class CustomLcdDisplay : public Display {
public:
    CustomLcdDisplay(): Display() {}

    virtual bool Lock(int timeout_ms = 0) override { return true; }
    virtual void Unlock() override {}
};


class Gc9D01LcdDisplay : public SpiLcdDisplay {
public:
    Gc9D01LcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                    esp_lcd_panel_handle_t panel_handle,
                    int width,
                    int height,
                    int offset_x,
                    int offset_y,
                    bool mirror_x,
                    bool mirror_y,
                    bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {

        DisplayLockGuard lock(this);
        // 由于屏幕是圆的，所以状态栏需要增加左右内边距
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES * 0.33, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES * 0.33, 0);
    }
};


class Toy_AI_Core_C3_1_28 : public WifiBoard
{
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display *display_ = nullptr;
    Button boot_button_;
    bool press_to_talk_enabled_ = false;
    PowerSaveTimer *power_save_timer_;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    void InitializePowerSaveTimer()
    {
        power_save_timer_ = new PowerSaveTimer(160, 60);
        power_save_timer_->OnEnterSleepMode([this]()
                                            {
            ESP_LOGI(TAG, "Enabling sleep mode");
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("sleepy");
            
            auto codec = GetAudioCodec();
            codec->EnableInput(false); });
        power_save_timer_->OnExitSleepMode([this]()
                                           {
            auto codec = GetAudioCodec();
            codec->EnableInput(true);
            
            auto display = GetDisplay();
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral"); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c()
    { // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    // SPI初始化
    void InitializeSpi()
    {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                                              DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // // GC9A01初始化
    // void InitializeGc9a01Display()
    // {
    //     ESP_LOGI(TAG, "Init GC9A01 display");

    //     ESP_LOGI(TAG, "Install panel IO");
    //     esp_lcd_panel_io_handle_t io_handle = NULL;
    //     esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
    //     io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
    //     ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &io_handle));

    //     ESP_LOGI(TAG, "Install GC9A01 panel driver");
    //     esp_lcd_panel_handle_t panel_handle = NULL;
    //     esp_lcd_panel_dev_config_t panel_config = {};
    //     panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN; // Set to -1 if not use
    //     panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;        // LCD_RGB_ENDIAN_RGB;
    //     panel_config.bits_per_pixel = 16;                    // Implemented by LCD command `3Ah` (16/18)

    //     ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
    //     ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    //     ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    //     ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    //     ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    //     ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    //     uint8_t data_0x62[] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 };
    //     esp_lcd_panel_io_tx_param(io_handle, 0x62, data_0x62, sizeof(data_0x62));

    //     uint8_t data_0x63[] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 };
    //     esp_lcd_panel_io_tx_param(io_handle, 0x63, data_0x63, sizeof(data_0x63));

    //    display_ = new CustomLcdDisplay(io_handle, panel_handle,
    //                                 DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    // }


    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
        
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.audio_speaker.set_gain", 
            "设置麦克风输入增益(灵敏度)，范围0-100",
            PropertyList({
                Property("gain", kPropertyTypeInteger, 0, 100)
            }), 
            [this](const PropertyList& properties) -> ReturnValue {
                auto codec = GetAudioCodec();
                codec->SetInputGain(properties["gain"].value<int>());
                return true;
        });

    }

public:
    Toy_AI_Core_C3_1_28() : boot_button_(BOOT_BUTTON_GPIO)
    {
        

        InitializeCodecI2c();
        InitializeSpi();
        // InitializeGc9a01Display();
        display_ = new CustomLcdDisplay(); // 使用默认的 Display 实现，显示功能会受限，但可以先保证其他功能正常
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeTools();
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);
    }

    virtual Led *GetLed() override
    {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display *GetDisplay() override
    {
        return display_;
    }

    virtual AudioCodec *GetAudioCodec() override
    {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }
};

DECLARE_BOARD(Toy_AI_Core_C3_1_28);


