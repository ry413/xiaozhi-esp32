#include "sip_phone.h"

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <utility>
#include <vector>

#include "audio_codec.h"
#include "board.h"
#include "esp_err.h"
#include "esp_log.h"
#include "media_lib_adapter.h"
#include "media_lib_netif.h"

#define TAG "SipPhone"

namespace {
constexpr const char* kSipUri = "udp://6001:unsecurepassword@192.168.1.83:5060";

uint8_t G711aEncode(int16_t pcm) {
    constexpr int kClip = 32635;
    int sign = (pcm < 0) ? 0x80 : 0x00;
    if (pcm < 0) {
        pcm = -pcm;
        if (pcm < 0) {
            pcm = kClip;
        }
    }
    if (pcm > kClip) {
        pcm = kClip;
    }

    int exponent = 7;
    for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; mask >>= 1) {
        --exponent;
    }
    int mantissa = (exponent == 0) ? ((pcm >> 4) & 0x0f) : ((pcm >> (exponent + 3)) & 0x0f);
    return static_cast<uint8_t>((sign | (exponent << 4) | mantissa) ^ 0x55);
}

int16_t G711aDecode(uint8_t alaw) {
    alaw ^= 0x55;
    int sign = alaw & 0x80;
    int exponent = (alaw >> 4) & 0x07;
    int mantissa = alaw & 0x0f;
    int sample = (mantissa << 4) + 8;
    if (exponent != 0) {
        sample += 0x100;
    }
    if (exponent > 1) {
        sample <<= (exponent - 1);
    }
    return static_cast<int16_t>(sign ? -sample : sample);
}
}

SipPhone::SipPhone() {
    data_cb_.send_audio = &SipPhone::SendAudio;
    data_cb_.receive_audio = &SipPhone::ReceiveAudio;
}

SipPhone::~SipPhone() {
    Stop();
}

bool SipPhone::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtc_ != nullptr) {
        return true;
    }

    media_lib_add_default_adapter();
    local_addr_ = GetLocalAddress();
    if (local_addr_.empty()) {
        ESP_LOGW(TAG, "No local IPv4 address, skip SIP register");
        return false;
    }

    config_ = {};
    config_.uri = kSipUri;
    config_.local_addr = local_addr_.c_str();
    config_.acodec_type = RTC_ACODEC_G711A;
    config_.data_cb = &data_cb_;
    config_.use_public_addr = false;
    config_.ctx = this;
    config_.event_handler = &SipPhone::OnRtcEvent;

    ESP_LOGI(TAG, "Start SIP register uri=%s local=%s", kSipUri, local_addr_.c_str());
    rtc_ = esp_rtc_service_init(&config_);
    if (rtc_ == nullptr) {
        ESP_LOGE(TAG, "esp_rtc_service_init failed");
        return false;
    }
    return true;
}

bool SipPhone::Call(const std::string& remote_user) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtc_ == nullptr) {
        ESP_LOGW(TAG, "SIP is not registered yet");
        return false;
    }

    int ret = esp_rtc_call(rtc_, remote_user.c_str());
    ESP_LOGI(TAG, "Call %s ret=%d", remote_user.c_str(), ret);
    return ret == ESP_OK;
}

bool SipPhone::Answer() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtc_ == nullptr) {
        ESP_LOGW(TAG, "SIP is not registered yet");
        return false;
    }

    int ret = esp_rtc_answer(rtc_);
    ESP_LOGI(TAG, "Answer ret=%d", ret);
    return ret == ESP_OK;
}

void SipPhone::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rtc_ != nullptr) {
        esp_rtc_service_deinit(rtc_);
        rtc_ = nullptr;
    }
}

void SipPhone::SetAudioSessionCallbacks(std::function<void()> on_begin, std::function<void()> on_end) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_audio_session_begin_ = std::move(on_begin);
    on_audio_session_end_ = std::move(on_end);
}

int SipPhone::OnRtcEvent(esp_rtc_event_t event, void* ctx) {
    return static_cast<SipPhone*>(ctx)->HandleRtcEvent(event);
}

int SipPhone::SendAudio(unsigned char* data, int len, void* ctx) {
    return static_cast<SipPhone*>(ctx)->SendAudioFrame(data, len);
}

int SipPhone::ReceiveAudio(unsigned char* data, int len, void* ctx) {
    return static_cast<SipPhone*>(ctx)->ReceiveAudioFrame(data, len);
}

int SipPhone::SendAudioFrame(unsigned char* data, int len) {
    if (len <= 0) {
        return 0;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!codec->input_enabled()) {
        codec->EnableInput(true);
    }

    const int source_rate = codec->input_sample_rate();
    const int channels = std::max(codec->input_channels(), 1);
    const int source_frames = len * source_rate / 8000;
    std::vector<int16_t> pcm(source_frames * channels);
    if (!codec->InputData(pcm)) {
        memset(data, 0xd5, len);
        return len;
    }

    for (int i = 0; i < len; ++i) {
        int source_frame = i * source_rate / 8000;
        data[i] = G711aEncode(pcm[source_frame * channels]);
    }
    return len;
}

int SipPhone::ReceiveAudioFrame(unsigned char* data, int len) {
    if (len <= 0) {
        return 0;
    }
    if ((len == 6) && !strncasecmp(reinterpret_cast<char*>(data), "DTMF-", 5)) {
        ESP_LOGI(TAG, "Receive DTMF Event ID: %d", data[5]);
        return 0;
    }

    auto codec = Board::GetInstance().GetAudioCodec();
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
    }

    const int target_rate = codec->output_sample_rate();
    const int target_frames = len * target_rate / 8000;
    std::vector<int16_t> pcm(target_frames);
    for (int i = 0; i < target_frames; ++i) {
        int source_index = i * 8000 / target_rate;
        pcm[i] = G711aDecode(data[source_index]);
    }
    codec->OutputData(pcm);
    return len;
}

int SipPhone::HandleRtcEvent(esp_rtc_event_t event) {
    switch (event) {
        case ESP_RTC_EVENT_REGISTERED:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_REGISTERED");
            break;
        case ESP_RTC_EVENT_UNREGISTERED:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_UNREGISTERED");
            break;
        case ESP_RTC_EVENT_INCOMING:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_INCOMING");
            break;
        case ESP_RTC_EVENT_CALLING:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_CALLING");
            break;
        case ESP_RTC_EVENT_CALL_ANSWERED:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_CALL_ANSWERED");
            break;
        case ESP_RTC_EVENT_HANGUP:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_HANGUP");
            break;
        case ESP_RTC_EVENT_AUDIO_SESSION_BEGIN:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_AUDIO_SESSION_BEGIN");
            if (on_audio_session_begin_) {
                on_audio_session_begin_();
            }
            break;
        case ESP_RTC_EVENT_AUDIO_SESSION_END:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_AUDIO_SESSION_END");
            if (on_audio_session_end_) {
                on_audio_session_end_();
            }
            break;
        case ESP_RTC_EVENT_ERROR:
            ESP_LOGW(TAG, "ESP_RTC_EVENT_ERROR");
            break;
        default:
            ESP_LOGD(TAG, "RTC event %d", static_cast<int>(event));
            break;
    }
    return ESP_OK;
}

std::string SipPhone::GetLocalAddress() {
    media_lib_ipv4_info_t ip_info = {};
    if (media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip_info) != ESP_OK) {
        return {};
    }

    char* addr = media_lib_ipv4_ntoa(&ip_info.ip);
    return addr == nullptr ? std::string() : std::string(addr);
}
