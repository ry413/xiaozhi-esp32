#ifndef SIP_PHONE_H_
#define SIP_PHONE_H_

#include <functional>
#include <mutex>
#include <string>

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

    std::mutex mutex_;
    std::mutex audio_mutex_;
    esp_rtc_handle_t rtc_ = nullptr;
    esp_rtc_config_t config_ = {};
    esp_rtc_data_cb_t data_cb_ = {};
    std::string local_addr_;
    std::function<void()> on_audio_session_begin_;
    std::function<void()> on_audio_session_end_;
};

#endif  // SIP_PHONE_H_
