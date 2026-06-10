#ifndef SIP_PHONE_H_
#define SIP_PHONE_H_

#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_aec.h"
#include "esp_rtc.h"

class SipPhone {
public:
    SipPhone();
    ~SipPhone();

    bool Start();
    bool Call(const std::string& remote_user);
    bool Answer();
    void Stop();
    void SetAudioSessionCallbacks(std::function<void()> on_begin, std::function<void()> on_end);

private:
    static int OnRtcEvent(esp_rtc_event_t event, void* ctx);
    static int SendAudio(unsigned char* data, int len, void* ctx);
    static int ReceiveAudio(unsigned char* data, int len, void* ctx);

    int SendAudioFrame(unsigned char* data, int len);
    int ReceiveAudioFrame(unsigned char* data, int len);
    int HandleRtcEvent(esp_rtc_event_t event);
    std::string GetLocalAddress();
    void StartAudioTask();
    void StopAudioTask(bool wait);
    static void AudioTask(void* ctx);
    void AudioTaskLoop();
    bool EnsureAec(int sample_rate, int channels);
    void DestroyAec();
    void FeedAec(const std::vector<int16_t>& interleaved, int frames, int channels);
    void PushCapturePcm(const int16_t* data, int samples);
    void PopCapturePcm(std::vector<int16_t>& pcm);
    void PushPlaybackPcm(std::vector<int16_t>&& pcm);
    std::vector<int16_t> PopPlaybackPcm(int max_samples);

    std::mutex mutex_;
    std::mutex audio_mutex_;
    std::mutex capture_mutex_;
    std::mutex playback_mutex_;
    esp_rtc_handle_t rtc_ = nullptr;
    esp_rtc_config_t config_ = {};
    esp_rtc_data_cb_t data_cb_ = {};
    std::string local_addr_;
    std::function<void()> on_audio_session_begin_;
    std::function<void()> on_audio_session_end_;
    bool sip_audio_active_ = false;
    bool audio_task_running_ = false;
    TaskHandle_t audio_task_handle_ = nullptr;
    aec_handle_t* aec_ = nullptr;
    int aec_chunk_size_ = 0;
    int16_t* aec_mic_chunk_ = nullptr;
    int16_t* aec_ref_chunk_ = nullptr;
    int16_t* aec_out_chunk_ = nullptr;
    std::vector<int16_t> aec_mic_buffer_;
    std::vector<int16_t> aec_ref_buffer_;
    std::vector<int16_t> aec_processed_;
    std::vector<int16_t> playback_buffer_;
};

#endif  // SIP_PHONE_H_
