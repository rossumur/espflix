/* Copyright (c) 2020, Peter Barrett
**
** Permission to use, copy, modify, and/or distribute this software for
** any purpose with or without fee is hereby granted, provided that the
** above copyright notice and this permission notice appear in all copies.
**
** THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
** WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
** WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR
** BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
** OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
** WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OpR OTHER TORTIOUS ACTION,
** ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
** SOFTWARE.
*/

#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "esp_event_loop.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_int_wdt.h"
#include "esp_task_wdt.h"
#include "soc/rtc.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "nvs_flash.h"
#include "nvs.h"

#define PERF
#include "src/streamer.h"
#include "src/player.h"
#include "src/video.h"

#include <map>
#include <string>
using namespace std;

//================================================================================
//================================================================================
// Audio
// Single pin with second order software PDM modulation
// see https://www.sigma-delta.de/sdopt/index.html for the tool used to design the Delta Sigma modulator
// 48000hz sample rate, 32x oversampling for PDM
// Generous 192k bitrate, low complexity, low latency SBC codec

void audio_init_hw()
{
    mem("audio_init_hw");
    pinMode(IR_PIN,INPUT);   

    i2s_config_t i2s_config;
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = 48000;
    i2s_config.bits_per_sample = (i2s_bits_per_sample_t)16,
    i2s_config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    i2s_config.communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S);
    i2s_config.dma_buf_count = 2;
    i2s_config.dma_buf_len = 512; // 2k * 2 bytes for audio buffers, pretty tight
    i2s_config.use_apll = false;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_driver_install((i2s_port_t)1, &i2s_config, 0, NULL);
    mem("after audio_init_hw");
    
    i2s_pin_config_t pin_config;
    pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.ws_io_num = I2S_PIN_NO_CHANGE;
    pin_config.data_out_num = 18;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;
    i2s_set_pin((i2s_port_t)1, &pin_config);
}

void pdm_second_order(uint16_t* dst, const int16_t* src, int len, int32_t a1 = 0x7FFF*1.18940, int a2 = 0x7FFF*2.12340)
{
    static int32_t _i0;
    static int32_t _i1;
    static int32_t _i2;
    int32_t i0 = _i0;   // force compiler to use registers
    int32_t i1 = _i1;
    int32_t i2 = _i2;
    
    uint32_t b = 0;
    int32_t s = 0;
    len <<= 1;
    while (len--)
    {
        if (len & 1)
            s = *src++ * 2;
        i0 = (i0 + s) >> 1; // lopass
        int n = 16;
        while (n--) {
            b <<= 1;
            if (i2 >= 0) {
                i1 += i0 - a1 - (i2 >> 7);  // feedback
                i2 += i1 - a2;
                b |= 1;
            } else {
                i1 += i0 + a1 - (i2 >> 7);  // feedback
                i2 += i1 + a2;
            }
        }
        *dst++ = b;
    }
    _i0 = i0;
    _i1 = i1;
    _i2 = i2;
}

const int16_t _sin32[32] = {
  0x0000,0xE708,0xCF05,0xB8E4,0xA57F,0x9594,0x89C0,0x8277,
  0x8001,0x8277,0x89C0,0x9594,0xA57F,0xB8E4,0xCF05,0xE708,
  0x0000,0x18F8,0x30FB,0x471C,0x5A81,0x6A6C,0x7640,0x7D89,
  0x7FFF,0x7D89,0x7640,0x6A6C,0x5A81,0x471C,0x30FB,0x18F8,
};

uint8_t _beep;
void beep()
{
  _beep = 5;
}

// this routine turns PCM into PDM. Also handles generating slience and beeps
void write_pcm_16(const int16_t* s, int n, int channels)
{
  uint16_t samples_data[128*2];
  
  PLOG(PDM_START);
  if (_beep) {
     int16_t* b = (int16_t*)samples_data + 128; // both src and dst buffer to save stack
     for (int i = 0; i < 128; i++)
        b[i] = _sin32[i & 31] >> 2;    // sinewave beep plz
    _beep--;
    pdm_second_order(samples_data,b,128);
  } else {
    if (s)
      pdm_second_order(samples_data,s,n);
    else {
        for (int i = 0; i < 128*2; i++)
          samples_data[i] = 0xAAAA;     // PDM silence
    }
  }
  size_t i2s_bytes_write;
  i2s_write((i2s_port_t)1, samples_data, sizeof(samples_data), &i2s_bytes_write, portMAX_DELAY);  // audio thread will block here
  PLOG(PDM_END);
}

//================================================================================
//================================================================================
// Store the pts of the current movie in non-volatile storage

nvs_handle _nvs;
static int nv_open()
{
  if (_nvs || (nvs_open("espflix", NVS_READWRITE, &_nvs) == 0))
    return 0;
  return -1;
}

// force key to max 15 chars
static const char* limit_key(const char* key)
{
  int n = strlen(key);
  return (n < 15) ? key : key + (n-15);
}

int64_t nv_read(const char* key)
{
  int64_t pts = 0;
  if (nv_open() == 0)
    nvs_get_i64(_nvs,limit_key(key),&pts);
  return pts;
}

void nv_write(const char* key, int64_t pts)
{
  if (nv_open() == 0)
    nvs_set_i64(_nvs,limit_key(key),pts);
}

//================================================================================
//================================================================================
// WIFI

extern EventGroupHandle_t _event_group;
WiFiState _wifi_state = NONE;
WiFiState wifi_state()
{
    return _wifi_state;
}

std::map<string,int> _ssids;
std::map<string,int>& wifi_list()
{
  return _ssids;
}

// manually entered ssid/password
void wifi_join(const char* ssid, const char* pwd)
{
    wifi_config_t wifi_config = {0};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, pwd);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_connect();
    _wifi_state = CONNECTING;
}

// current ssid if any
std::string wifi_ssid()
{
    wifi_config_t wifi_config = {0};
    esp_wifi_get_config(ESP_IF_WIFI_STA,&wifi_config);
    return (char*)wifi_config.sta.ssid;
}

void wifi_scan()
{
    wifi_scan_config_t config = {0};
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&config,false);
    _wifi_state = SCANNING;
}

void wifi_disconnect()
{
    esp_wifi_disconnect();
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    printf("EVENT %d\n",event->event_id);
    switch(event->event_id)
    {
      case SYSTEM_EVENT_STA_START:
          printf("SYSTEM_EVENT_STA_START\n");
          esp_wifi_connect();
          break;
  
      case SYSTEM_EVENT_STA_GOT_IP:
          tcpip_adapter_ip_info_t ip_info;
          tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
          printf("Local IP Address:  %s\n", ip4addr_ntoa(&ip_info.ip));
          _ssids.clear();
          _wifi_state = CONNECTED;
          break;
  
      case SYSTEM_EVENT_STA_DISCONNECTED:
          printf("SYSTEM_EVENT_STA_DISCONNECTED %d\n",event->event_info.disconnected.reason);
          wifi_scan();
          break;

      case SYSTEM_EVENT_SCAN_DONE:
          {
            uint16_t apCount = 0;
            esp_wifi_scan_get_ap_num(&apCount);
            printf("SYSTEM_EVENT_SCAN_DONE %d %d\n",apCount,sizeof(wifi_ap_record_t)*apCount);
            if (apCount > 16)
              apCount = 16;
            wifi_ap_record_t list[16];  // careful of large stack 16*80 bytes. Limitation of the 16 highest power APs
            esp_wifi_scan_get_ap_records(&apCount, list);
            _ssids.clear();
            for (uint16_t i = 0; i < apCount; i++) {
                wifi_ap_record_t *ap = list + i;
                const char* ssid = (const char *)ap->ssid;
                if (_ssids.find(ssid) == _ssids.end()) {
                  _ssids[ssid] = (ap->rssi << 8) | ap->authmode;
                  //printf("\"%s\",%d,\n",ssid,(ap->rssi << 8) | ap->authmode);
                }
            }
          }
          _wifi_state = SCAN_COMPLETE;
          break;
    }
    return ESP_OK;
}

void init_wifi()
{
    esp_log_level_set("wifi", ESP_LOG_VERBOSE);  
    tcpip_adapter_init();
    esp_event_loop_init(event_handler, NULL);
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_tx_buf_num = 8;
    cfg.static_rx_buf_num = 16;
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    printf("connecting to %s\n",wifi_ssid().c_str());
    
    mem("after esp_wifi_start");
    //wifi_join("failure","test");
}

//================================================================================
//================================================================================
// Decide on a video standard

#define PAL 0
#define NTSC 1

void setup()
{
    rtc_clk_cpu_freq_set(RTC_CPU_FREQ_240M);  
    _event_group = xEventGroupCreate();
    mem("setup");
    init_wifi();
}

// this loop always runs on app_core (1).
void loop()
{
    espflix_run(1); // never returns
}
