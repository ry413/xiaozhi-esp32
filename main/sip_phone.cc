#include "sip_phone.h"

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <utility>
#include <vector>

#include "audio_codec.h"
#include "board.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_lib_adapter.h"
#include "media_lib_netif.h"

#define TAG "SipPhone"

namespace {
constexpr const char* kSipUri = "udp://6001:unsecurepassword@192.168.1.83:5060";
constexpr size_t kAecBufferAlignment = 16;
constexpr int kSipAudioSampleRate = 16000;
constexpr int kSipRtpSampleRate = 8000;
constexpr int kSipAudioTaskStackSize = 8192;
constexpr int kSipAudioTaskPriority = 4;
constexpr size_t kMaxCaptureBufferSamples = kSipAudioSampleRate;
constexpr size_t kMaxPlaybackBufferSamples = kSipAudioSampleRate;

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
    StopAudioTask(true);
    std::lock_guard<std::mutex> audio_lock(audio_mutex_);
    DestroyAec();
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

    const int source_frames = len * kSipAudioSampleRate / kSipRtpSampleRate;
    std::vector<int16_t> pcm(source_frames, 0);
    PopCapturePcm(pcm);

    for (int i = 0; i < len; ++i) {
        int source_frame = i * kSipAudioSampleRate / kSipRtpSampleRate;
        data[i] = G711aEncode(pcm[source_frame]);
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

    const int target_frames = len * kSipAudioSampleRate / kSipRtpSampleRate;
    std::vector<int16_t> pcm(target_frames);
    for (int i = 0; i < target_frames; ++i) {
        int source_index = i * kSipRtpSampleRate / kSipAudioSampleRate;
        pcm[i] = G711aDecode(data[source_index]);
    }
    PushPlaybackPcm(std::move(pcm));
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
            if (sip_audio_active_) {
                sip_audio_active_ = false;
                StopAudioTask(false);
                if (on_audio_session_end_) {
                    on_audio_session_end_();
                }
            }
            break;
        case ESP_RTC_EVENT_AUDIO_SESSION_BEGIN:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_AUDIO_SESSION_BEGIN");
            sip_audio_active_ = true;
            if (on_audio_session_begin_) {
                on_audio_session_begin_();
            }
            StartAudioTask();
            break;
        case ESP_RTC_EVENT_AUDIO_SESSION_END:
            ESP_LOGI(TAG, "ESP_RTC_EVENT_AUDIO_SESSION_END");
            sip_audio_active_ = false;
            StopAudioTask(false);
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

void SipPhone::StartAudioTask() {
    std::lock_guard<std::mutex> lock(audio_mutex_);
    if (audio_task_handle_ != nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> capture_lock(capture_mutex_);
        aec_processed_.clear();
    }
    {
        std::lock_guard<std::mutex> playback_lock(playback_mutex_);
        playback_buffer_.clear();
    }

    audio_task_running_ = true;
    auto ret = xTaskCreate(&SipPhone::AudioTask, "sip_audio", kSipAudioTaskStackSize, this, kSipAudioTaskPriority,
                           &audio_task_handle_);
    if (ret != pdPASS) {
        audio_task_running_ = false;
        audio_task_handle_ = nullptr;
        ESP_LOGE(TAG, "Failed to create SIP audio task");
    }
}

void SipPhone::StopAudioTask(bool wait) {
    TaskHandle_t task = nullptr;
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        audio_task_running_ = false;
        task = audio_task_handle_;
    }

    if (wait && task != nullptr && task != xTaskGetCurrentTaskHandle()) {
        for (int i = 0; i < 100; ++i) {
            {
                std::lock_guard<std::mutex> lock(audio_mutex_);
                if (audio_task_handle_ == nullptr) {
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    {
        std::lock_guard<std::mutex> capture_lock(capture_mutex_);
        aec_processed_.clear();
    }
    {
        std::lock_guard<std::mutex> playback_lock(playback_mutex_);
        playback_buffer_.clear();
    }
}

void SipPhone::AudioTask(void* ctx) {
    auto self = static_cast<SipPhone*>(ctx);
    self->AudioTaskLoop();
    vTaskDelete(nullptr);
}

void SipPhone::AudioTaskLoop() {
    auto codec = Board::GetInstance().GetAudioCodec();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(audio_mutex_);
            if (!audio_task_running_) {
                break;
            }

            if (!codec->input_enabled()) {
                codec->EnableInput(true);
            }

            const int source_rate = codec->input_sample_rate();
            const int channels = std::max(codec->input_channels(), 1);
            int read_frames = source_rate / 50;
            if (EnsureAec(source_rate, channels)) {
                read_frames = aec_chunk_size_;
            }

            std::vector<int16_t> input(read_frames * channels);
            if (codec->InputData(input)) {
                if (aec_ != nullptr) {
                    FeedAec(input, read_frames, channels);
                } else {
                    std::vector<int16_t> mono(read_frames);
                    for (int i = 0; i < read_frames; ++i) {
                        mono[i] = input[i * channels];
                    }
                    if (source_rate == kSipAudioSampleRate) {
                        PushCapturePcm(mono.data(), mono.size());
                    } else {
                        const int target_frames = read_frames * kSipAudioSampleRate / source_rate;
                        std::vector<int16_t> resampled(target_frames);
                        for (int i = 0; i < target_frames; ++i) {
                            resampled[i] = mono[i * source_rate / kSipAudioSampleRate];
                        }
                        PushCapturePcm(resampled.data(), resampled.size());
                    }
                }
            }

            const int output_rate = codec->output_sample_rate();
            auto playback = PopPlaybackPcm(kSipAudioSampleRate / 50);
            if (!playback.empty()) {
                if (output_rate != kSipAudioSampleRate) {
                    const int output_frames = playback.size() * output_rate / kSipAudioSampleRate;
                    std::vector<int16_t> resampled(output_frames);
                    for (int i = 0; i < output_frames; ++i) {
                        resampled[i] = playback[i * kSipAudioSampleRate / output_rate];
                    }
                    playback = std::move(resampled);
                }
                if (!codec->output_enabled()) {
                    codec->EnableOutput(true);
                }
                codec->OutputData(playback);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        DestroyAec();
        audio_task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "SIP audio task stopped");
}

bool SipPhone::EnsureAec(int sample_rate, int channels) {
    if (sample_rate != 16000 || channels < 2) {
        return false;
    }
    if (aec_ != nullptr && aec_mic_chunk_ != nullptr && aec_ref_chunk_ != nullptr && aec_out_chunk_ != nullptr) {
        return true;
    }
    DestroyAec();

    aec_ = aec_create(sample_rate, 4, 1, AEC_MODE_VOIP_HIGH_PERF);
    if (aec_ == nullptr) {
        ESP_LOGW(TAG, "Failed to create SIP AEC");
        return false;
    }

    aec_chunk_size_ = aec_get_chunksize(aec_);
    const size_t chunk_bytes = aec_chunk_size_ * sizeof(int16_t);
    aec_mic_chunk_ = static_cast<int16_t*>(
        heap_caps_aligned_alloc(kAecBufferAlignment, chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    aec_ref_chunk_ = static_cast<int16_t*>(
        heap_caps_aligned_alloc(kAecBufferAlignment, chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    aec_out_chunk_ = static_cast<int16_t*>(
        heap_caps_aligned_alloc(kAecBufferAlignment, chunk_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (aec_mic_chunk_ == nullptr || aec_ref_chunk_ == nullptr || aec_out_chunk_ == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate SIP AEC buffers, chunk=%d", aec_chunk_size_);
        DestroyAec();
        return false;
    }

    aec_mic_buffer_.clear();
    aec_ref_buffer_.clear();
    aec_processed_.clear();
    ESP_LOGI(TAG, "SIP AEC started, chunk=%d", aec_chunk_size_);
    return true;
}

void SipPhone::DestroyAec() {
    if (aec_ != nullptr) {
        aec_destroy(aec_);
        aec_ = nullptr;
    }
    if (aec_mic_chunk_ != nullptr) {
        heap_caps_free(aec_mic_chunk_);
        aec_mic_chunk_ = nullptr;
    }
    if (aec_ref_chunk_ != nullptr) {
        heap_caps_free(aec_ref_chunk_);
        aec_ref_chunk_ = nullptr;
    }
    if (aec_out_chunk_ != nullptr) {
        heap_caps_free(aec_out_chunk_);
        aec_out_chunk_ = nullptr;
    }
    aec_chunk_size_ = 0;
    aec_mic_buffer_.clear();
    aec_ref_buffer_.clear();
    aec_processed_.clear();
}

void SipPhone::FeedAec(const std::vector<int16_t>& interleaved, int frames, int channels) {
    if (aec_ == nullptr || channels < 2 || aec_mic_chunk_ == nullptr || aec_ref_chunk_ == nullptr ||
        aec_out_chunk_ == nullptr) {
        return;
    }

    aec_mic_buffer_.reserve(aec_mic_buffer_.size() + frames);
    aec_ref_buffer_.reserve(aec_ref_buffer_.size() + frames);
    for (int i = 0; i < frames; ++i) {
        aec_mic_buffer_.push_back(interleaved[i * channels]);
        aec_ref_buffer_.push_back(interleaved[i * channels + 1]);
    }

    while (static_cast<int>(aec_mic_buffer_.size()) >= aec_chunk_size_ &&
           static_cast<int>(aec_ref_buffer_.size()) >= aec_chunk_size_) {
        const size_t chunk_bytes = aec_chunk_size_ * sizeof(int16_t);
        memcpy(aec_mic_chunk_, aec_mic_buffer_.data(), chunk_bytes);
        memcpy(aec_ref_chunk_, aec_ref_buffer_.data(), chunk_bytes);
        aec_process(aec_, aec_mic_chunk_, aec_ref_chunk_, aec_out_chunk_);
        PushCapturePcm(aec_out_chunk_, aec_chunk_size_);
        aec_mic_buffer_.erase(aec_mic_buffer_.begin(), aec_mic_buffer_.begin() + aec_chunk_size_);
        aec_ref_buffer_.erase(aec_ref_buffer_.begin(), aec_ref_buffer_.begin() + aec_chunk_size_);
    }
}

void SipPhone::PushCapturePcm(const int16_t* data, int samples) {
    if (data == nullptr || samples <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(capture_mutex_);
    aec_processed_.insert(aec_processed_.end(), data, data + samples);
    if (aec_processed_.size() > kMaxCaptureBufferSamples) {
        aec_processed_.erase(aec_processed_.begin(),
                             aec_processed_.begin() + (aec_processed_.size() - kMaxCaptureBufferSamples));
    }
}

void SipPhone::PopCapturePcm(std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    const size_t samples = std::min(pcm.size(), aec_processed_.size());
    if (samples > 0) {
        std::copy(aec_processed_.begin(), aec_processed_.begin() + samples, pcm.begin());
        aec_processed_.erase(aec_processed_.begin(), aec_processed_.begin() + samples);
    }
}

void SipPhone::PushPlaybackPcm(std::vector<int16_t>&& pcm) {
    if (pcm.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(playback_mutex_);
    playback_buffer_.insert(playback_buffer_.end(), pcm.begin(), pcm.end());
    if (playback_buffer_.size() > kMaxPlaybackBufferSamples) {
        playback_buffer_.erase(playback_buffer_.begin(),
                               playback_buffer_.begin() + (playback_buffer_.size() - kMaxPlaybackBufferSamples));
    }
}

std::vector<int16_t> SipPhone::PopPlaybackPcm(int max_samples) {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    const int samples = std::min(max_samples, static_cast<int>(playback_buffer_.size()));
    if (samples <= 0) {
        return {};
    }

    std::vector<int16_t> pcm(playback_buffer_.begin(), playback_buffer_.begin() + samples);
    playback_buffer_.erase(playback_buffer_.begin(), playback_buffer_.begin() + samples);
    return pcm;
}

std::string SipPhone::GetLocalAddress() {
    media_lib_ipv4_info_t ip_info = {};
    if (media_lib_netif_get_ipv4_info(MEDIA_LIB_NET_TYPE_STA, &ip_info) != ESP_OK) {
        return {};
    }

    char* addr = media_lib_ipv4_ntoa(&ip_info.ip);
    return addr == nullptr ? std::string() : std::string(addr);
}
