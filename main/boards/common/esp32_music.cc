#include "esp32_music.h"
#include "board.h"
#include "system_info.h"
#include "audio/audio_codec.h"
#include "application.h"
#include "protocols/protocol.h"
#include "display/display.h"
#include <vector>
#include <netdb.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <thread>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/dirent.h>

#define TAG "Esp32Music"

// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

// 清理电台URL，去除末尾的问号
static std::string CleanRadioUrl(const std::string& url) {
    if (url.empty()) return url;

    // 去除末尾的 '?'
    if (url.back() == '?') {
        return url.substr(0, url.size() - 1);
    }

    return url;
}

Esp32Music::Esp32Music() : last_downloaded_data_(), current_music_url_(), current_song_name_(),
                         song_name_displayed_(false), current_lyric_str(), lyrics_(), 
                         current_lyric_index_(-1), lyric_thread_(), is_lyric_running_(false),
                         display_mode_(DISPLAY_MODE_LYRICS), is_playing_(false), is_downloading_(false), 
                         play_thread_(), download_thread_(), audio_buffer_(), buffer_mutex_(), 
                         buffer_cv_(), buffer_size_(0), mp3_decoder_(nullptr), mp3_frame_info_(), 
                         mp3_decoder_initialized_(false) {
    ESP_LOGI(TAG, "Music player initialized with default spectrum display mode");
    InitializeMp3Decoder();

    pcm_buffer = (int16_t*)heap_caps_malloc(
        2304 * sizeof(int16_t),
        MALLOC_CAP_SPIRAM
    );
    if (!pcm_buffer) {
        ESP_LOGE(TAG, "pcm_buffer malloc failed");
    }
}

Esp32Music::~Esp32Music() {
    ESP_LOGI(TAG, "Destroying music player - stopping all operations");
    
    // 停止所有操作
    is_downloading_ = false;
    is_playing_ = false;
    is_lyric_running_ = false;
    
    // 通知所有等待的线程
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    // 等待下载线程结束，设置5秒超时
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for download thread to finish (timeout: 5s)");
        auto start_time = std::chrono::steady_clock::now();
        
        // 等待线程结束
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 5) {
                ESP_LOGW(TAG, "Download thread join timeout after 5 seconds");
                break;
            }
            
            // 再次设置停止标志，确保线程能够检测到
            is_downloading_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!download_thread_.joinable()) {
                thread_finished = true;
            }
            
            // 定期打印等待信息
            if (elapsed > 0 && elapsed % 1 == 0) {
                ESP_LOGI(TAG, "Still waiting for download thread to finish... (%ds)", (int)elapsed);
            }
        }
        
        if (download_thread_.joinable()) {
            download_thread_.join();
        }
        ESP_LOGI(TAG, "Download thread finished");
    }
    
    // 等待播放线程结束，设置3秒超时
    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for playback thread to finish (timeout: 3s)");
        auto start_time = std::chrono::steady_clock::now();
        
        bool thread_finished = false;
        while (!thread_finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count();
            
            if (elapsed >= 3) {
                ESP_LOGW(TAG, "Playback thread join timeout after 3 seconds");
                break;
            }
            
            // 再次设置停止标志
            is_playing_ = false;
            
            // 通知条件变量
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            
            // 检查线程是否已经结束
            if (!play_thread_.joinable()) {
                thread_finished = true;
            }
        }
        
        if (play_thread_.joinable()) {
            play_thread_.join();
        }
        ESP_LOGI(TAG, "Playback thread finished");
    }
    
    // 等待歌词线程结束
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Waiting for lyric thread to finish");
        lyric_thread_.join();
        ESP_LOGI(TAG, "Lyric thread finished");
    }
    
    // 清理缓冲区和MP3解码器
    ClearAudioBuffer();
    CleanupMp3Decoder();
    
    ESP_LOGI(TAG, "Music player destroyed successfully");
}

bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name) {
    auto display = Board::GetInstance().GetDisplay();
    // display->SetMusicInfoVisible(true);
    display->SetChatMessage("assistant", "正在搜索...");
    ESP_LOGI(TAG, "Starting to get music details for: %s", song_name.c_str());

    last_downloaded_data_.clear();

    // 1. 使用公开 API：api.yaohud.cn
    std::string api_url = "https://api.yaohud.cn/api/music/wyvip";
    std::string api_key = "ZHr87ZPsXSBxFTGCF9O";

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);

    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Connection", "close");
    
    const int kMaxCandidateIndex = 3;
    bool any_success = false;
    for (int n = 1; n <= kMaxCandidateIndex; n++) {
        std::string full_url = api_url +
        "?key=" + api_key +
        "&msg=" + url_encode(song_name + (artist_name.empty() ? "" : " - " + artist_name)) +
        "&n=" + std::to_string(n);
        
        ESP_LOGI(TAG, "Request URL: %s", full_url.c_str());
        
        
        if (!http->Open("GET", full_url)) {
            ESP_LOGE(TAG, "Failed to connect to music API");
            continue;
        }
        
        int status_code = http->GetStatusCode();
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP GET failed with status code: %d", status_code);
            http->Close();
            continue;
        }
        
        //--------------------------------------------------------------
        // 2. 读取 JSON 响应
        //--------------------------------------------------------------
        last_downloaded_data_ = http->ReadAll();
        http->Close();

        ESP_LOGI(TAG, "Response length = %d", last_downloaded_data_.length());

        //--------------------------------------------------------------
        // 3. 解析 JSON
        //--------------------------------------------------------------
        cJSON* root = cJSON_Parse(last_downloaded_data_.c_str());
        if (!root) {
            ESP_LOGE(TAG, "JSON parse failed");
            return false;
        }

        cJSON* code = cJSON_GetObjectItem(root, "code");
        if (!cJSON_IsNumber(code) || code->valueint != 200) {
            ESP_LOGE(TAG, "API error: code=%d", code ? code->valueint : -1);
            cJSON_Delete(root);
            return false;
        }

        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsObject(data)) {
            ESP_LOGE(TAG, "Missing 'data' object");
            cJSON_Delete(root);
            return false;
        }

        cJSON* title     = cJSON_GetObjectItem(data, "name");
        cJSON* artist    = cJSON_GetObjectItem(data, "songname");
        cJSON* url    = cJSON_GetObjectItem(data, "url");
        if (!cJSON_IsString(url) || strlen(url->valuestring) == 0) {
            ESP_LOGE(TAG, "No valid streaming URL found in response");
            cJSON_Delete(root);
            continue;
        }

        std::string candidate_title = cJSON_IsString(title) ? title->valuestring : "";
        std::string candidate_url   = url->valuestring;
        std::string candidate_lrc;

        cJSON* music_obj = cJSON_GetObjectItem(data, "music");
        if (cJSON_IsObject(music_obj)) {
            cJSON* lrc = cJSON_GetObjectItem(music_obj, "lrc");
            if (cJSON_IsString(lrc) && lrc->valuestring && lrc->valuestring[0] != '\0') {
                candidate_lrc = lrc->valuestring;
            }
        }

        cJSON_Delete(root);

        StopStreaming();
        ResetStreamStartResult();
        current_song_name_ = candidate_title;
        current_music_url_ = candidate_url;
        song_name_displayed_ = false;
        last_play_is_radio_ = false;

        StartStreaming(current_music_url_);

        StreamStartResult start_result = StreamStartResult::None;
        bool started = WaitForStreamStart(8000 /* ms */, &start_result);

        if (started) {
            ESP_LOGI(TAG, "Song started successfully with candidate n=%d", n);

            // 只有起播成功才真正挂载歌词线程
            if (!candidate_lrc.empty() && display_mode_ == DISPLAY_MODE_LYRICS) {
                current_lyric_str = candidate_lrc;
                // 先收掉旧歌词线程
                if (lyric_thread_.joinable()) {
                    is_lyric_running_ = false;
                    lyric_thread_.join();
                }
                is_lyric_running_ = true;
                current_lyric_index_ = -1;
                {
                    std::lock_guard<std::mutex> lock(lyrics_mutex_);
                    lyrics_.clear();
                }
                lyric_thread_ = std::thread(&Esp32Music::LyricDisplayThread, this);
            } else {
                // 没歌词就清掉歌词状态
                is_lyric_running_ = false;
                current_lyric_str.clear();
                std::lock_guard<std::mutex> lock(lyrics_mutex_);
                lyrics_.clear();
            }

            any_success = true;
            break;    // 不再试后面的 n
        } else {
            ESP_LOGW(TAG, "Song candidate n=%d failed to start (result=%d), trying next",
                     n, (int)start_result);
            // 确保线程收掉，缓冲清理
            StopStreaming();
            continue;
        }
    }
    if (!any_success) {
        ESP_LOGE(TAG, "All candidate songs failed to start");
        return false;
    }

    return true;
}

std::string Esp32Music::GetDownloadResult() { return last_downloaded_data_; }

// 开始流式播放
bool Esp32Music::StartStreaming(const std::string& music_url) {
    if (music_url.empty()) {
        ESP_LOGE(TAG, "Music URL is empty");
        return false;
    }
    
    ESP_LOGI(TAG, "Starting streaming for URL: %s", music_url.c_str());
    
    StopStreaming();
    current_music_url_ = music_url;
    
    // 配置线程栈大小以避免栈溢出
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "download_audio_stream";
    cfg.stack_size = 4096;
    cfg.prio = 7;
    cfg.pin_to_core = tskNO_AFFINITY;
    esp_pthread_set_cfg(&cfg);
    
    // 开始下载线程
    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadAudioStream, this, music_url);
    
    // 开始播放线程（会等待缓冲区有足够数据）
    cfg.thread_name = "play_audio_stream";
    cfg.stack_size = 3072;
    cfg.prio = 6;
    cfg.pin_to_core = tskNO_AFFINITY;
    esp_pthread_set_cfg(&cfg);
    is_playing_ = true;
    is_paused_ = false;
    play_thread_ = std::thread(&Esp32Music::PlayAudioStream, this);

    cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&cfg);
    
    ESP_LOGI(TAG, "Streaming threads started successfully");
    
    return true;
}

void Esp32Music::Pause() {
    // 只有正在播放时才有意义
    if (!is_playing_) {
        ESP_LOGI(TAG, "Pause ignored: not playing");
        return;
    }

        // 因为系统顶不住, 所以暂时不存在暂停
        StopStreaming();
        return;

    // 电台直接停止
    if (last_play_is_radio_) {
        StopStreaming();
        return;
    }

    is_paused_ = true;
    ESP_LOGI(TAG, "Music playback paused");
}

void Esp32Music::Resume() {
    // 电台直接开始播
    if (last_play_is_radio_) {
        PlayRadioStream(last_play_radio_name_, last_play_radio_url_);
        return;
    }

    // 必须本来在播，而且确实处于暂停
    if (!is_playing_ || !is_paused_) {
        ESP_LOGI(TAG, "Resume ignored: not paused or not playing");
        return;
    }

    is_paused_ = false;
    ESP_LOGI(TAG, "Music playback resumed");
}

// 停止流式播放
bool Esp32Music::StopStreaming() {
    ESP_LOGI(TAG,
             "Stopping music streaming - downloading=%d, playing=%d, lyric=%d, paused=%d",
             is_downloading_.load(),
             is_playing_.load(),
             is_lyric_running_.load(),
             is_paused_.load());

    is_downloading_    = false;
    is_playing_        = false;
    is_lyric_running_  = false;
    is_paused_         = false;

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();  // 把所有 wait 唤醒
    }

    // 逐个 join，确保不会留下孤儿线程
    if (download_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining download thread in StopStreaming");
        download_thread_.join();
    }

    if (play_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining play thread in StopStreaming");
        play_thread_.join();
    }

    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining lyric thread in StopStreaming");
        lyric_thread_.join();
    }

    // 清空音频缓冲区，释放所有 chunk 的 heap_caps_malloc 内存
    ClearAudioBuffer();

    // 重置采样率到原始值
    ResetSampleRate();
    
    // 清空歌名显示
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        // 清空歌名和歌词显示
        display->SetMusicInfo("");
        display->SetMusicInfoVisible(false);
        display->SetMusicLyrics("");
        display->SetMusicLyricsVisible(false);

        if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
            display->StopFft();
            ESP_LOGI(TAG, "Stopped FFT display in StopStreaming (spectrum mode)");
        } else {
            ESP_LOGI(TAG, "Not in spectrum mode, skip FFT stop");
        }
    }
    
    ESP_LOGI(TAG, "Music streaming stop signal sent");
    return true;
}

// 流式下载音频数据
void Esp32Music::DownloadAudioStream(const std::string& music_url) {
    ESP_LOGD(TAG, "Starting audio stream download from: %s", music_url.c_str());
    
    // 验证URL有效性
    if (music_url.empty() || music_url.find("http") != 0) {
        ESP_LOGE(TAG, "Invalid URL format: %s", music_url.c_str());
        is_downloading_ = false;
        return;
    }
    
    auto network = Board::GetInstance().GetNetwork();

    // 处理重定向
    std::string current_url = music_url;
    const int max_redirects = 5;
    int redirect_count = 0;

    using HttpPtr = decltype(network->CreateHttp(0));
    HttpPtr http = nullptr;

    while (true) {
        http = network->CreateHttp(0);

        http->SetHeader(
            "User-Agent",
            "Mozilla/5.0 (ESP32) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
        );
        http->SetHeader("Accept", "*/*");

        ESP_LOGD(TAG, "Requesting music URL: %s", current_url.c_str());

        if (!http->Open("GET", current_url)) {
            ESP_LOGE(TAG, "Failed to connect to music stream URL");
            is_downloading_ = false;
            return;
        }

        int status_code = http->GetStatusCode();

        if (status_code == 301 || status_code == 302 ||
            status_code == 303 || status_code == 307 ||
            status_code == 308) {

            std::string location = http->GetResponseHeader("Location");
            if (location.empty()) {
                ESP_LOGE(TAG, "Redirect (%d) but no Location header", status_code);
                http->Close();
                is_downloading_ = false;
                return;
            }

            ESP_LOGI(TAG, "Redirect %d to: %s", status_code, location.c_str());
            http->Close();

            current_url = location;

            if (++redirect_count > max_redirects) {
                ESP_LOGE(TAG, "Too many redirects, abort");
                is_downloading_ = false;
                return;
            }

            continue;
        }

        if (status_code != 200 && status_code != 206) {
            char buf[256];
            int n = http->Read(buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                ESP_LOGW(TAG, "Error code: %d, body preview: %s", status_code, buf);
            }
            http->Close();
            is_downloading_ = false;
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_cv_.notify_all();
            }
            return;
        }

        ESP_LOGI(TAG, "Started downloading audio stream, status: %d", status_code);
        break;
    }

    // 分块读取音频数据
    const size_t chunk_size = 4096;  // 4KB每块
    char* buffer = (char*)heap_caps_malloc(chunk_size, MALLOC_CAP_SPIRAM);
    size_t total_downloaded = 0;
    
    while (is_downloading_ && is_playing_) {
        vTaskDelay(1);  // 让出CPU时间片

        int bytes_read = http->Read(buffer, chunk_size);
        if (bytes_read < 0) {
            ESP_LOGE(TAG, "Failed to read audio data: error code %d", bytes_read);
            break;
        }
        if (bytes_read == 0) {
            ESP_LOGI(TAG, "Audio stream download completed, total: %d bytes", total_downloaded);
            break;
        }

        if (bytes_read >= 16) {
        } else {
            ESP_LOGI(TAG, "Data chunk too small: %d bytes", bytes_read);
        }
        
        // 尝试检测文件格式（检查文件头）
        if (total_downloaded == 0 && bytes_read >= 4 && !last_play_is_radio_) {
            if (memcmp(buffer, "ID3", 3) == 0) {
                ESP_LOGI(TAG, "Detected MP3 file with ID3 tag");
            } else if (buffer[0] == 0xFF && (buffer[1] & 0xE0) == 0xE0) {
                ESP_LOGI(TAG, "Detected MP3 file header");
            } else if (memcmp(buffer, "RIFF", 4) == 0) {
                ESP_LOGI(TAG, "Detected WAV file");
                break;
            } else if (memcmp(buffer, "fLaC", 4) == 0) {
                ESP_LOGI(TAG, "Detected FLAC file");
                break;
            } else if (memcmp(buffer, "OggS", 4) == 0) {
                ESP_LOGI(TAG, "Detected OGG file");
                break;
            } else {
                ESP_LOGI(TAG, "Unknown audio format, first 4 bytes: %02X %02X %02X %02X", 
                        (unsigned char)buffer[0], (unsigned char)buffer[1], 
                        (unsigned char)buffer[2], (unsigned char)buffer[3]);
                break;
            }
        }
        
        // 创建音频数据块
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);
        
        // 等待缓冲区有空间
        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this] { return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_; });
            
            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_ += bytes_read;
                total_downloaded += bytes_read;
                
                // 通知播放线程有新数据
                buffer_cv_.notify_one();
                
                if (total_downloaded % (256 * 1024) == 0) {  // 每256KB打印一次进度
                    ESP_LOGI(TAG, "Downloaded %d bytes, buffer size: %d", total_downloaded, buffer_size_);
                }
            } else {
                heap_caps_free(chunk_data);
                heap_caps_free(buffer);
                break;
            }
        }
    }
    
    http->Close();
    is_downloading_ = false;
    
    // 通知播放线程下载完成
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }
    
    ESP_LOGI(TAG, "Audio stream download thread finished");
}

// 流式播放音频数据
void Esp32Music::PlayAudioStream() {
    ESP_LOGI(TAG, "Starting audio stream playback");
    
    // 重置状态机
    ResetStreamStartResult();

    // 初始化时间跟踪变量
    current_play_time_ms_ = 0;
    last_frame_time_ms_ = 0;
    total_frames_decoded_ = 0;
    
    auto codec = Board::GetInstance().GetAudioCodec();
    if (!codec) {
        is_playing_ = false;
        return;
    }
    
    if (!mp3_decoder_initialized_) {
        ESP_LOGE(TAG, "MP3 decoder not initialized");
        is_playing_ = false;
        return;
    }
    
    // 等待缓冲区有足够数据开始播放
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);

        auto timeout = std::chrono::seconds(5);
        bool ok = buffer_cv_.wait_for(lock, timeout, [this] {
            // 如果上层已经让我们停了，就直接退出等待
            if (!is_playing_) return true;

            // 有足够的数据可以开始播
            if (buffer_size_ >= MIN_BUFFER_SIZE) return true;

            // 下载已经结束 + 缓冲区里有一点点数据（哪怕不够 MIN，也可以试着播完就退出）
            if (!is_downloading_ && !audio_buffer_.empty()) return true;

            return false;
        });

        if (!ok) {
            ESP_LOGW(TAG, "Timeout waiting for initial audio data, aborting playback");
            is_playing_ = false;
            SetStreamStartResult(StreamStartResult::Timeout);
            return;
        }

        if (!is_playing_) {
            ESP_LOGI(TAG, "Playback stopped before it could start");
            SetStreamStartResult(StreamStartResult::Error);
            return;
        }

        if (buffer_size_ == 0 && !is_downloading_ && audio_buffer_.empty()) {
            ESP_LOGW(TAG, "No audio data received, download stopped, aborting playback");
            is_playing_ = false;
            SetStreamStartResult(StreamStartResult::NoData);
            return;
        }

        SetStreamStartResult(StreamStartResult::Ok);
    }
    
    ESP_LOGI(TAG, "Starting playback with buffer size: %d", buffer_size_);
    
    size_t total_played = 0;
    uint8_t* mp3_input_buffer = nullptr;
    int bytes_left = 0;
    uint8_t* read_ptr = nullptr;
    
    // 分配MP3输入缓冲区
    mp3_input_buffer = (uint8_t*)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!mp3_input_buffer) {
        ESP_LOGE(TAG, "Failed to allocate MP3 input buffer");
        is_playing_ = false;
        return;
    }
    
    // 标记是否已经处理过ID3标签
    bool id3_processed = false;

    // 播放主循环
    while (is_playing_) {
        if (!last_play_is_radio_ && is_paused_) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        
        // 检查设备状态，只有在空闲状态才播放音乐
        auto& app = Application::GetInstance();
        DeviceState current_state = app.GetDeviceState();
        
        // 状态转换：说话中-》聆听中-》待机状态-》播放音乐
        if (current_state == kDeviceStateListening || current_state == kDeviceStateSpeaking) {
            if (current_state == kDeviceStateSpeaking) {
                ESP_LOGI(TAG, "Device is in speaking state, switching to listening state for music playback");
            }
            if (current_state == kDeviceStateListening) {
                ESP_LOGI(TAG, "Device is in listening state, switching to idle state for music playback");
            }
            // 切换状态
            app.ToggleChatState(); // 试图变成待机状态
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        } else if (current_state != kDeviceStateIdle) { // 不是待机状态，就一直卡在这里，不让播放音乐
            ESP_LOGD(TAG, "Device state is %d, pausing music playback", current_state);
            // 如果不是空闲状态，暂停播放
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        
        // 设备状态检查通过，显示当前播放的歌名
        if (!song_name_displayed_ && !current_song_name_.empty()) {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            if (display) {
                std::string formatted_song_name = "《" + current_song_name_ + "》";
                display->SetMusicInfo(formatted_song_name.c_str());
                display->SetMusicInfoVisible(true);
                ESP_LOGI(TAG, "Displaying song name: %s", formatted_song_name.c_str());
                song_name_displayed_ = true;
            }

            // 根据显示模式启动相应的显示功能
            if (display) {
                if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
                    display->StartFft();
                    display->SetMusicLyricsVisible(false);
                    ESP_LOGI(TAG, "Display start() called for spectrum visualization");
                } else {
                    display->SetMusicLyricsVisible(true);
                    ESP_LOGI(TAG, "Lyrics display mode active, FFT visualization disabled");
                }
            }
        }
        
        // 如果需要更多MP3数据，从缓冲区读取
        if (bytes_left < 4096) {  // 保持至少4KB数据用于解码
            AudioChunk chunk;
            
            // 从缓冲区获取音频数据
            {
                std::unique_lock<std::mutex> lock(buffer_mutex_);
                if (audio_buffer_.empty()) {
                    if (!is_downloading_) {
                        // 下载完成且缓冲区为空，播放结束
                        ESP_LOGI(TAG, "Playback finished, total played: %d bytes", total_played);
                        break;
                    }
                    // 等待新数据
                    buffer_cv_.wait(lock, [this] { return !audio_buffer_.empty() || !is_downloading_; });
                    if (audio_buffer_.empty()) {
                        continue;
                    }
                }
                
                chunk = audio_buffer_.front();
                audio_buffer_.pop();
                buffer_size_ -= chunk.size;
                
                // 通知下载线程缓冲区有空间
                buffer_cv_.notify_one();
            }
            
            // 将新数据添加到MP3输入缓冲区
            if (chunk.data && chunk.size > 0) {
                // 移动剩余数据到缓冲区开头
                if (bytes_left > 0 && read_ptr != mp3_input_buffer) {
                    memmove(mp3_input_buffer, read_ptr, bytes_left);
                }
                
                // 检查缓冲区空间
                size_t space_available = 8192 - bytes_left;
                size_t copy_size = std::min(chunk.size, space_available);
                
                // 复制新数据
                memcpy(mp3_input_buffer + bytes_left, chunk.data, copy_size);
                bytes_left += copy_size;
                read_ptr = mp3_input_buffer;
                
                // 检查并跳过ID3标签（仅在开始时处理一次）
                if (!id3_processed && bytes_left >= 10) {
                    size_t id3_skip = SkipId3Tag(read_ptr, bytes_left);
                    if (id3_skip > 0) {
                        read_ptr += id3_skip;
                        bytes_left -= id3_skip;
                        ESP_LOGI(TAG, "Skipped ID3 tag: %u bytes", (unsigned int)id3_skip);
                    }
                    id3_processed = true;
                }
                
                // 释放chunk内存
                heap_caps_free(chunk.data);
            }
        }
        
        // 尝试找到MP3帧同步
        int sync_offset = MP3FindSyncWord(read_ptr, bytes_left);
        if (sync_offset < 0) {
            ESP_LOGW(TAG, "No MP3 sync word found, skipping %d bytes", bytes_left);
            bytes_left = 0;
            // continue;
        }
        
        // 跳过到同步位置
        if (sync_offset > 0) {
            read_ptr += sync_offset;
            bytes_left -= sync_offset;
        }
        
        // 解码MP3帧
        int decode_result = MP3Decode(mp3_decoder_, &read_ptr, &bytes_left, pcm_buffer, 0);
        
        if (decode_result == 0) {
            // 解码成功，获取帧信息
            MP3GetLastFrameInfo(mp3_decoder_, &mp3_frame_info_);
            total_frames_decoded_++;
            
            // 基本的帧信息有效性检查，防止除零错误
            if (mp3_frame_info_.samprate == 0 || mp3_frame_info_.nChans == 0) {
                ESP_LOGW(TAG, "Invalid frame info: rate=%d, channels=%d, skipping", 
                        mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                continue;
            }
            
            // 计算当前帧的持续时间(毫秒)
            int frame_duration_ms = (mp3_frame_info_.outputSamps * 1000) / 
                                  (mp3_frame_info_.samprate * mp3_frame_info_.nChans);
            
            // 更新当前播放时间
            current_play_time_ms_ += frame_duration_ms;
            
            ESP_LOGD(TAG, "Frame %d: time=%lldms, duration=%dms, rate=%d, ch=%d", 
                    total_frames_decoded_, current_play_time_ms_, frame_duration_ms,
                    mp3_frame_info_.samprate, mp3_frame_info_.nChans);
            
            // 更新歌词显示
            int buffer_latency_ms = 600; // 实测调整值
            UpdateLyricDisplay(current_play_time_ms_ + buffer_latency_ms);
            
            // 将PCM数据发送到Application的音频解码队列
            if (mp3_frame_info_.outputSamps > 0) {
                int16_t* final_pcm_data = pcm_buffer;
                int final_sample_count = mp3_frame_info_.outputSamps;
                std::vector<int16_t> mono_buffer;
                
                // 如果是双通道，转换为单通道混合
                if (mp3_frame_info_.nChans == 2) {
                    // 双通道转单通道：将左右声道混合
                    int stereo_samples = mp3_frame_info_.outputSamps;  // 包含左右声道的总样本数
                    int mono_samples = stereo_samples / 2;  // 实际的单声道样本数
                    
                    mono_buffer.resize(mono_samples);
                    
                    for (int i = 0; i < mono_samples; ++i) {
                        // 混合左右声道 (L + R) / 2
                        int left = pcm_buffer[i * 2];      // 左声道
                        int right = pcm_buffer[i * 2 + 1]; // 右声道
                        mono_buffer[i] = (int16_t)((left + right) / 2);
                    }
                    
                    final_pcm_data = mono_buffer.data();
                    final_sample_count = mono_samples;

                    ESP_LOGD(TAG, "Converted stereo to mono: %d -> %d samples", 
                            stereo_samples, mono_samples);
                } else if (mp3_frame_info_.nChans == 1) {
                    // 已经是单声道，无需转换
                    ESP_LOGD(TAG, "Already mono audio: %d samples", final_sample_count);
                } else {
                    ESP_LOGW(TAG, "Unsupported channel count: %d, treating as mono", 
                            mp3_frame_info_.nChans);
                }
                
                // 创建AudioStreamPacket
                AudioStreamPacket packet;
                packet.sample_rate = mp3_frame_info_.samprate;
                packet.frame_duration = 60;  // 使用Application默认的帧时长
                packet.timestamp = 0;
                
                // 将int16_t PCM数据转换为uint8_t字节数组
                size_t pcm_size_bytes = final_sample_count * sizeof(int16_t);
                packet.payload.resize(pcm_size_bytes);
                memcpy(packet.payload.data(), final_pcm_data, pcm_size_bytes);

                if (final_pcm_data_fft == nullptr) {
                    final_pcm_data_fft = (int16_t*)heap_caps_malloc(
                        final_sample_count * sizeof(int16_t),
                        MALLOC_CAP_SPIRAM
                    );
                }
                
                memcpy(
                    final_pcm_data_fft,
                    final_pcm_data,
                    final_sample_count * sizeof(int16_t)
                );
                
                ESP_LOGD(TAG, "Sending %d PCM samples (%d bytes, rate=%d, channels=%d->1) to Application", 
                        final_sample_count, pcm_size_bytes, mp3_frame_info_.samprate, mp3_frame_info_.nChans);
                
                // 发送到Application的音频解码队列
                app.AddAudioData(std::move(packet));
                total_played += pcm_size_bytes;
                
                // 打印播放进度
                if (total_played % (128 * 1024) == 0) {
                    ESP_LOGI(TAG, "Played %d bytes, buffer size: %d", total_played, buffer_size_);
                }
            }
            
        } else {
            // 解码失败
            ESP_LOGW(TAG, "MP3 decode failed with error: %d", decode_result);
            if (last_play_is_radio_) {
                ESP_LOGW(TAG, "In radio mode, stopping playback on decode error");
                SetStreamStartResult(StreamStartResult::Error);
                // break;
            }
            
            // 跳过一些字节继续尝试
            if (bytes_left > 1) {
                read_ptr++;
                bytes_left--;
            } else {
                bytes_left = 0;
            }
        }

        // 避免长期占用 CPU，定期让出时间片
        static int frame_counter = 0;
        frame_counter++;
        if (frame_counter >= 14) {
            frame_counter = 0;
            vTaskDelay(1);
        }
    }

    // 播放循环结束

    // 清理
    if (mp3_input_buffer) {
        heap_caps_free(mp3_input_buffer);
        mp3_input_buffer = nullptr;
    }
    
    is_playing_ = false;

    ResetSampleRate();

    // 播放结束时进行基本清理，但不调用StopStreaming避免线程自我等待
    ESP_LOGI(TAG, "Audio stream playback finished, total played: %d bytes", total_played);
    ESP_LOGI(TAG, "Performing basic cleanup from play thread");
    
    // 只在频谱显示模式下才停止FFT显示
    if (display_mode_ == DISPLAY_MODE_SPECTRUM) {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            display->StopFft();
            ESP_LOGI(TAG, "Stopped FFT display from play thread (spectrum mode)");
        }
    } else {
        ESP_LOGI(TAG, "Not in spectrum mode, skipping FFT stop");
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    if (display) {
        display->SetMusicInfo("");  // 清空歌名显示
        display->SetMusicLyrics(""); // 清空歌词显示
        display->SetMusicInfoVisible(false);
        display->SetMusicLyricsVisible(false);
        ESP_LOGI(TAG, "Cleared song name display");
    }
}

// 清空音频缓冲区
void Esp32Music::ClearAudioBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    
    while (!audio_buffer_.empty()) {
        AudioChunk chunk = audio_buffer_.front();
        audio_buffer_.pop();
        if (chunk.data) {
            heap_caps_free(chunk.data);
        }
    }
    
    buffer_size_ = 0;
    ESP_LOGI(TAG, "Audio buffer cleared");
}

// 初始化MP3解码器
bool Esp32Music::InitializeMp3Decoder() {
    mp3_decoder_ = MP3InitDecoder();
    if (mp3_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        mp3_decoder_initialized_ = false;
        return false;
    }
    
    mp3_decoder_initialized_ = true;
    ESP_LOGI(TAG, "MP3 decoder initialized successfully");
    return true;
}

// 清理MP3解码器
void Esp32Music::CleanupMp3Decoder() {
    if (mp3_decoder_ != nullptr) {
        MP3FreeDecoder(mp3_decoder_);
        mp3_decoder_ = nullptr;
    }
    mp3_decoder_initialized_ = false;
    ESP_LOGI(TAG, "MP3 decoder cleaned up");
}

// 重置采样率到原始值
void Esp32Music::ResetSampleRate() {
    // auto& board = Board::GetInstance();
    // auto codec = board.GetAudioCodec();
    // if (codec && codec->original_output_sample_rate() > 0 && 
    //     codec->output_sample_rate() != codec->original_output_sample_rate()) {
    //     ESP_LOGI(TAG, "重置采样率：从 %d Hz 重置到原始值 %d Hz", 
    //             codec->output_sample_rate(), codec->original_output_sample_rate());
    //     if (codec->SetOutputSampleRate(-1)) {  // -1 表示重置到原始值
    //         ESP_LOGI(TAG, "成功重置采样率到原始值: %d Hz", codec->output_sample_rate());
    //     } else {
    //         ESP_LOGW(TAG, "无法重置采样率到原始值");
    //     }
    // }
}

// 跳过MP3文件开头的ID3标签
size_t Esp32Music::SkipId3Tag(uint8_t* data, size_t size) {
    if (!data || size < 10) {
        return 0;
    }
    
    // 检查ID3v2标签头 "ID3"
    if (memcmp(data, "ID3", 3) != 0) {
        return 0;
    }
    
    // 计算标签大小（synchsafe integer格式）
    uint32_t tag_size = ((uint32_t)(data[6] & 0x7F) << 21) |
                        ((uint32_t)(data[7] & 0x7F) << 14) |
                        ((uint32_t)(data[8] & 0x7F) << 7)  |
                        ((uint32_t)(data[9] & 0x7F));
    
    // ID3v2头部(10字节) + 标签内容
    size_t total_skip = 10 + tag_size;
    
    // 确保不超过可用数据大小
    if (total_skip > size) {
        total_skip = size;
    }
    
    ESP_LOGI(TAG, "Found ID3v2 tag, skipping %u bytes", (unsigned int)total_skip);
    return total_skip;
}

// 解析歌词
bool Esp32Music::ParseLyrics(const std::string& lyric_content) {
    ESP_LOGI(TAG, "Parsing lyrics content");
    
    // 使用锁保护lyrics_数组访问
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    lyrics_.clear();
    
    // 按行分割歌词内容
    std::istringstream stream(lyric_content);
    std::string line;
    
    while (std::getline(stream, line)) {
        // 去除行尾的回车符
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        // 跳过空行
        if (line.empty()) {
            continue;
        }
        
        // 解析LRC格式: [mm:ss.xx]歌词文本
        if (line.length() > 10 && line[0] == '[') {
            size_t close_bracket = line.find(']');
            if (close_bracket != std::string::npos) {
                std::string tag_or_time = line.substr(1, close_bracket - 1);
                std::string content = line.substr(close_bracket + 1);
                
                // 检查是否是元数据标签而不是时间戳
                // 元数据标签通常是 [ti:标题], [ar:艺术家], [al:专辑] 等
                size_t colon_pos = tag_or_time.find(':');
                if (colon_pos != std::string::npos) {
                    std::string left_part = tag_or_time.substr(0, colon_pos);
                    
                    // 检查冒号左边是否是时间（数字）
                    bool is_time_format = true;
                    for (char c : left_part) {
                        if (!isdigit(c)) {
                            is_time_format = false;
                            break;
                        }
                    }
                    
                    // 如果不是时间格式，跳过这一行（元数据标签）
                    if (!is_time_format) {
                        // 可以在这里处理元数据，例如提取标题、艺术家等信息
                        ESP_LOGD(TAG, "Skipping metadata tag: [%s]", tag_or_time.c_str());
                        continue;
                    }
                    
                    // 是时间格式，解析时间戳
                    try {
                        int minutes = std::stoi(tag_or_time.substr(0, colon_pos));
                        float seconds = std::stof(tag_or_time.substr(colon_pos + 1));
                        int timestamp_ms = minutes * 60 * 1000 + (int)(seconds * 1000);
                        
                        // 安全处理歌词文本，确保UTF-8编码正确
                        std::string safe_lyric_text;
                        if (!content.empty()) {
                            // 创建安全副本并验证字符串
                            safe_lyric_text = content;
                            // 确保字符串以null结尾
                            safe_lyric_text.shrink_to_fit();
                        }
                        
                        lyrics_.push_back(std::make_pair(timestamp_ms, safe_lyric_text));
                        
                        if (!safe_lyric_text.empty()) {
                            // 限制日志输出长度，避免中文字符截断问题
                            size_t log_len = std::min(safe_lyric_text.length(), size_t(50));
                            std::string log_text = safe_lyric_text.substr(0, log_len);
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] %s", timestamp_ms, log_text.c_str());
                        } else {
                            ESP_LOGD(TAG, "Parsed lyric: [%d ms] (empty)", timestamp_ms);
                        }
                    } catch (const std::exception& e) {
                        ESP_LOGW(TAG, "Failed to parse time: %s", tag_or_time.c_str());
                    }
                }
            }
        }
    }
    
    // 按时间戳排序
    std::sort(lyrics_.begin(), lyrics_.end());
    
    ESP_LOGI(TAG, "Parsed %d lyric lines", lyrics_.size());
    return !lyrics_.empty();
}

// 歌词显示线程
void Esp32Music::LyricDisplayThread() {
    ESP_LOGI(TAG, "Lyric display thread started");

    if (!ParseLyrics(current_lyric_str)) {
        ESP_LOGE(TAG, "Failed to  parse lyrics");
        is_lyric_running_ = false;
        return;
    }
    
    // 定期检查是否需要更新显示(频率可以降低)
    while (is_lyric_running_ && is_playing_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ESP_LOGI(TAG, "Lyric display thread finished");
    is_lyric_running_ = false;
}

void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms) {
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    
    if (lyrics_.empty()) {
        return;
    }
    
    // 查找当前应该显示的歌词
    int new_lyric_index = -1;
    
    // 从当前歌词索引开始查找，提高效率
    int start_index = (current_lyric_index_.load() >= 0) ? current_lyric_index_.load() : 0;
    
    // 正向查找：找到最后一个时间戳小于等于当前时间的歌词
    for (int i = start_index; i < (int)lyrics_.size(); i++) {
        if (lyrics_[i].first <= current_time_ms) {
            new_lyric_index = i;
        } else {
            break;  // 时间戳已超过当前时间
        }
    }
    
    // 如果没有找到(可能当前时间比第一句歌词还早)，显示空
    if (new_lyric_index == -1) {
        new_lyric_index = -1;
    }
    
    // 如果歌词索引发生变化，更新显示
    if (new_lyric_index != current_lyric_index_) {
        current_lyric_index_ = new_lyric_index;
        
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        if (display) {
            std::string lyric_text;
            
            if (current_lyric_index_ >= 0 && current_lyric_index_ < (int)lyrics_.size()) {
                lyric_text = lyrics_[current_lyric_index_].second;
            }
            
            // 显示歌词
            display->SetMusicLyrics(lyric_text.c_str());
            
            ESP_LOGD(TAG, "Lyric update at %lldms: %s", 
                    current_time_ms, 
                    lyric_text.empty() ? "(no lyric)" : lyric_text.c_str());
        }
    }
}

// 显示模式控制方法实现
void Esp32Music::SetDisplayMode(DisplayMode mode) {
    DisplayMode old_mode = display_mode_.load();
    display_mode_ = mode;
    
    ESP_LOGI(TAG, "Display mode changed from %s to %s", 
            (old_mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS",
            (mode == DISPLAY_MODE_SPECTRUM) ? "SPECTRUM" : "LYRICS");
}

static std::string ProvinceToState(const std::string& province) {
    if (province == "浙江") return "Chekiang";
    if (province == "江苏") return "Kiangsu";
    if (province == "山东") return "Shantung";
    if (province == "河南") return "Honan";
    if (province == "河北") return "Hopei";
    if (province == "广东") return "Kwangtung";
    if (province == "吉林") return "Jilin";
    if (province == "四川") return "Szechuan";
    if (province == "新疆") return "Sinkiang";
    if (province == "山西") return "Shansi";
    if (province == "辽宁") return "Liaoning";
    if (province == "北京") return "Beijing";
    if (province == "云南") return "Yunnan";
    if (province == "湖南") return "Hunan";
    if (province == "湖北") return "Hupei";
    if (province == "内蒙古") return "Inner Mongolia";
    if (province == "大连") return "Dalian";
    if (province == "上海") return "Shanghai";
    if (province == "黑龙江") return "Amur River";
    if (province == "福建") return "Fukien";
    if (province == "江西") return "Kiangsi";
    if (province == "贵州") return "Kweichow";
    if (province == "安徽") return "Anhwei";
    if (province == "广西") return "Kwangsi";
    if (province == "陕西") return "Shensi";
    if (province == "海南") return "Hainan";
    if (province == "甘肃") return "Kansu";
    if (province == "香港") return "Hong Kong";
    if (province == "重庆") return "Chungking";
    if (province == "青海") return "Tsinghai";
    if (province == "天津") return "Tientsin";
    if (province == "澳门") return "澳門";
    if (province == "西藏") return "xizang";

    return province;
}

bool Esp32Music::SearchRadioStations(const std::string& tag,
                                     const std::string& language,
                                     const std::string& province,
                                     bool order_by_votes,
                                     const std::string& name_keyword,
                                     int limit,
                                     int offset,
                                     std::string* out_json) {
    if (!out_json) return false;
    *out_json = "{}";

    std::string base_url = GetRadioBrowserBaseUrl();
    std::string path     = "/json/stations/search";

    std::string query = "?codec=mp3&hidebroken=true";
    query += "&limit=" + std::to_string(limit > 0 ? limit : 5);
    query += "&offset=" + std::to_string(offset > 0 ? offset : 0);

    if (!name_keyword.empty())   query += "&name=" + url_encode(name_keyword);
    if (!tag.empty())           query += "&tag=" + url_encode(tag);
    query += "&language=" + (language.empty() ? url_encode("chinese") : url_encode(language));
    if (!province.empty())      query += "&state=" + url_encode(ProvinceToState(province));
    if (order_by_votes)         query += "&order=votes&reverse=true";


    std::string full_url = base_url + path + query;
    ESP_LOGI(TAG, "Radio search URL: %s", full_url.c_str());

    last_downloaded_data_.clear();

    auto network = Board::GetInstance().GetNetwork();
    auto http    = network->CreateHttp(0);

    http->SetHeader("User-Agent",
                    "Mozilla/5.0 (ESP32) AppleWebKit/537.36 "
                    "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Connection", "close");

    if (!http->Open("GET", full_url)) {
        ESP_LOGE(TAG, "Failed to connect to radio-browser API");
        return false;
    }

    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Radio-browser HTTP GET failed, status=%d", status_code);
        http->Close();
        return false;
    }

    last_downloaded_data_ = http->ReadAll();
    http->Close();

    if (last_downloaded_data_.empty()) {
        ESP_LOGE(TAG, "Empty response from radio-browser");
        return false;
    }

    cJSON* root = cJSON_Parse(last_downloaded_data_.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse radio-browser JSON");
        return false;
    }

    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Radio-browser response is not an array");
        cJSON_Delete(root);
        return false;
    }

    int station_count = cJSON_GetArraySize(root);
    if (station_count <= 0) {
        // ESP_LOGW(TAG, "No station found for query: %s", station_name.c_str());
        cJSON_Delete(root);
        return false;
    }

    // 组一个“干净的” JSON 数组返回给 LLM 用
    cJSON* out_arr = cJSON_CreateArray();

    int max_count = std::min(station_count, limit > 0 ? limit : 5);
    for (int i = 0; i < max_count; ++i) {
        cJSON* station = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(station)) continue;

        cJSON* name_field    = cJSON_GetObjectItem(station, "name");
        // cJSON* country_field = cJSON_GetObjectItem(station, "country");
        // cJSON* state_field   = cJSON_GetObjectItem(station, "state");
        // cJSON* lang_field    = cJSON_GetObjectItem(station, "language");
        // cJSON* codec_field   = cJSON_GetObjectItem(station, "codec");
        // cJSON* bitrate_field = cJSON_GetObjectItem(station, "bitrate");
        cJSON* url_resolved  = cJSON_GetObjectItem(station, "url_resolved");
        // cJSON* url_field     = cJSON_GetObjectItem(station, "url");

        std::string stream_url;
        if (cJSON_IsString(url_resolved) &&
            url_resolved->valuestring && url_resolved->valuestring[0] != '\0') {
            stream_url = url_resolved->valuestring;
        }
        // else if (cJSON_IsString(url_field) &&
        //            url_field->valuestring && url_field->valuestring[0] != '\0') {
        //     stream_url = url_field->valuestring;
        // }
        else {
            continue;
        }

        std::string final_url = CleanRadioUrl(stream_url);

        cJSON* out_item = cJSON_CreateObject();
        cJSON_AddStringToObject(out_item, "name",
            (cJSON_IsString(name_field) && name_field->valuestring) ? name_field->valuestring : "");
        // cJSON_AddStringToObject(out_item, "country",
        //     (cJSON_IsString(country_field) && country_field->valuestring) ? country_field->valuestring : "");
        // cJSON_AddStringToObject(out_item, "state",
        //     (cJSON_IsString(state_field) && state_field->valuestring) ? state_field->valuestring : "");
        // cJSON_AddStringToObject(out_item, "language",
        //     (cJSON_IsString(lang_field) && lang_field->valuestring) ? lang_field->valuestring : "");
        // cJSON_AddStringToObject(out_item, "codec",
        //     (cJSON_IsString(codec_field) && codec_field->valuestring) ? codec_field->valuestring : "");
        // cJSON_AddNumberToObject(out_item, "bitrate",
        //     cJSON_IsNumber(bitrate_field) ? bitrate_field->valueint : 0);
        cJSON_AddStringToObject(out_item, "stream_url", final_url.c_str());

        cJSON_AddItemToArray(out_arr, out_item);
    }

    char* out_str = cJSON_PrintUnformatted(out_arr);
    if (out_str) {
        *out_json = out_str;
        cJSON_free(out_str);
    }

    cJSON_Delete(out_arr);
    cJSON_Delete(root);

    return true;
}

bool Esp32Music::PlayRadioStream(const std::string& display_name,
                                 const std::string& stream_url) {
    std::string final_url = CleanRadioUrl(stream_url);

    ESP_LOGI(TAG, "PlayRadioStream: name=\"%s\", url=\"%s\"",
             display_name.c_str(), final_url.c_str());

    // 先停掉任何已存在的流
    StopStreaming();

    // 起播结果重置并启动流
    ResetStreamStartResult();
    last_play_is_radio_ = true;
    StartStreaming(final_url);

    StreamStartResult start_result = StreamStartResult::None;
    bool started = WaitForStreamStart(8000 /* ms */, &start_result);

    if (!started) {
        ESP_LOGW(TAG, "Radio stream \"%s\" failed to start, result=%d",
                 display_name.c_str(), (int)start_result);
        return false;
    }

    ESP_LOGI(TAG, "Radio stream \"%s\" started successfully", display_name.c_str());
    current_song_name_ = display_name;
    song_name_displayed_ = false;
    last_play_radio_name_ = display_name;
    last_play_radio_url_ = stream_url;

    // 电台没有歌词，确保歌词线程不会跑
    is_lyric_running_ = false;
    current_lyric_str.clear();
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        lyrics_.clear();
    }
    if (lyric_thread_.joinable()) {
        ESP_LOGI(TAG, "Joining previous lyric thread before starting radio");
        lyric_thread_.join();
    }

    return true;
}

std::string Esp32Music::GetRadioBrowserBaseUrl() {
    // 静态镜像列表
    static const char* kRadioBrowserBases[] = {
        "http://fi1.api.radio-browser.info",
        "http://de2.api.radio-browser.info",
    };
    static size_t kRadioBrowserBaseCount =
        sizeof(kRadioBrowserBases) / sizeof(kRadioBrowserBases[0]);

    static size_t index = 0;
    const char* base = kRadioBrowserBases[index];
    index = (index + 1) % kRadioBrowserBaseCount;

    ESP_LOGI(TAG, "Using static RadioBrowser base URL: %s", base);
    return std::string(base);
}

void Esp32Music::ResetStreamStartResult() {
    std::lock_guard<std::mutex> lock(stream_start_mutex_);
    stream_start_result_ = StreamStartResult::None;
}

void Esp32Music::SetStreamStartResult(StreamStartResult r) {
    {
        std::lock_guard<std::mutex> lock(stream_start_mutex_);
        // 只从 None 转成具体状态，避免被后来状态覆盖
        if (stream_start_result_ == StreamStartResult::None) {
            stream_start_result_ = r;
        }
    }
    stream_start_cv_.notify_all();
}

bool Esp32Music::WaitForStreamStart(uint32_t timeout_ms, StreamStartResult* out_result) {
    std::unique_lock<std::mutex> lock(stream_start_mutex_);
    bool ok = stream_start_cv_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [this] {
            return stream_start_result_ != StreamStartResult::None;
        });

    if (!ok) {
        if (out_result) *out_result = StreamStartResult::Timeout;
        return false;
    }

    if (out_result) *out_result = stream_start_result_;
    return stream_start_result_ == StreamStartResult::Ok;
}

bool Esp32Music::PlayLocalFile(const std::string& name) {
    ESP_LOGI(TAG, "PlayLocalFile: %s", name.c_str());

    if (name.empty()) {
        if (local_music_files_.empty()) {
            ESP_LOGE(TAG, "No local music files available to play");
            return false;
        }
    
        // 随机选择一个本地文件
        size_t index = esp_random() % local_music_files_.size();
        std::string selected_file = local_music_files_[index];
        ESP_LOGI(TAG, "No file specified, randomly selected: %s", selected_file.c_str());
        return PlayLocalFile(selected_file);
    }

    StopStreaming();
    ResetStreamStartResult();

    auto pos = name.find_last_of("/\\");
    if (pos != std::string::npos && pos + 1 < name.size()) {
        current_song_name_ = name.substr(pos + 1);
    } else {
        current_song_name_ = name;
    }

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.thread_name = "local_music_player";
    cfg.stack_size = 6144;
    cfg.prio = 7;
    cfg.pin_to_core = tskNO_AFFINITY;
    esp_pthread_set_cfg(&cfg);

    is_downloading_ = true;
    download_thread_ = std::thread(&Esp32Music::DownloadLocalFile, this, local_music_dir_ + name);

    cfg.thread_name = "play_audio_stream";
    cfg.stack_size = 3072;
    cfg.prio = 6;
    cfg.pin_to_core = tskNO_AFFINITY;
    esp_pthread_set_cfg(&cfg);

    is_playing_     = true;
    song_name_displayed_ = false;
    last_play_is_radio_ = false;
    is_paused_ = false;
    play_thread_     = std::thread(&Esp32Music::PlayAudioStream, this);

    cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&cfg);

    return true;
}

void Esp32Music::DownloadLocalFile(const std::string& path) {
    ESP_LOGI(TAG, "DownloadLocalFile from SD: %s", path.c_str());

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open local file: %s", path.c_str());
        is_downloading_ = false;

        // 唤醒播放线程，让它早点发现没数据
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_cv_.notify_all();
        }
        return;
    }

    ESP_LOGI(TAG, "HELL WORD");

    const size_t chunk_size = 4096;
    char buffer[chunk_size];
    size_t total_read = 0;

    while (is_downloading_ && is_playing_) {
        size_t bytes_read = fread(buffer, 1, chunk_size, fp);
        if (bytes_read == 0) {
            if (feof(fp)) {
                ESP_LOGI(TAG, "Local file read completed, total: %d bytes", (int)total_read);
            } else {
                ESP_LOGE(TAG, "Error reading local file: %s", path.c_str());
            }
            break;
        }

        // 申请一块和网络下载一样的 chunk 内存
        uint8_t* chunk_data = (uint8_t*)heap_caps_malloc(bytes_read, MALLOC_CAP_SPIRAM);
        if (!chunk_data) {
            ESP_LOGE(TAG, "Failed to allocate memory for local audio chunk");
            break;
        }
        memcpy(chunk_data, buffer, bytes_read);

        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            // 等待缓冲区有空间，或者被停止
            buffer_cv_.wait(lock, [this] {
                return buffer_size_ < MAX_BUFFER_SIZE || !is_downloading_;
            });

            if (is_downloading_) {
                audio_buffer_.push(AudioChunk(chunk_data, bytes_read));
                buffer_size_   += bytes_read;
                total_read     += bytes_read;

                buffer_cv_.notify_one();
            } else {
                // 已经被停止了，丢掉这块数据
                heap_caps_free(chunk_data);
                break;
            }
        }
    }

    fclose(fp);
    is_downloading_ = false;

    // 通知播放线程“下载”完了
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_cv_.notify_all();
    }

    ESP_LOGI(TAG, "DownloadLocalFile finished");
}

static bool has_mp3_extension(const std::string& name) {
    auto pos = name.find_last_of('.');
    if (pos == std::string::npos) return false;

    std::string ext = name.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == "mp3";
}

// 返回 /sdcard/Music/ 下所有 mp3 文件的「文件名」
void Esp32Music::InitializeLocalMusicFiles() {
    DIR* dp = opendir(local_music_dir_.c_str());
    if (!dp) {
        ESP_LOGE(TAG, "Failed to open dir: %s", local_music_dir_.c_str());
    }

    struct dirent* entry;
    while ((entry = readdir(dp)) != nullptr) {
        const char* name = entry->d_name;

        // 跳过 . 和 ..
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        // 组合成完整路径，用 stat 判断是不是普通文件
        std::string full_path = local_music_dir_;
        if (!full_path.empty() && full_path.back() != '/') {
            full_path.push_back('/');
        }
        full_path += name;

        struct stat st {};
        if (stat(full_path.c_str(), &st) != 0) {
            continue;
        }

        // 只要普通文件
        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        std::string filename{name};
        if (has_mp3_extension(filename)) {
            local_music_files_.push_back(filename);  // 只存名字，不带路径
        }
    }

    closedir(dp);
}
