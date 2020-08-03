//
//  streamer.hpp
//
//  Created by Peter Barrett on 6/10/20.
//  Copyright © 2020 Peter Barrett. All rights reserved.
//

#ifndef streamer_hpp
#define streamer_hpp

enum {
  PDM_START = 1,
  PDM_END,
  VIDEO_PES,
  AUDIO_PES,
  PUSH_AUDIO,
  PUSH_VIDEO,
  VIDEO_READY_P,
  WAIT_BUFFER,
  REQUEST_BUFFER,
  RECEIVED_BUFFER,
};

#if 0
#define PLOG(_x) plog(_x)
#define PLOGV(_x,_v) plogv(_x,_v)
void plog(int x);
void plogv(int x,int v);
#else
#define PLOG(_x)
#define PLOGV(_x,_v)
#endif

enum {
    DECODER_RUN = 2,
    DECODER_PAUSED = 4,
    AUDIO_READY = 8,
    VIDEO_READY = 16,
    DNS_READY = 256
};

#include <string>
#include <vector>
#include <map>

void espflix_run(int standard);

// deal with access points
enum WiFiState {
    NONE,
    SCANNING,
    SCAN_COMPLETE,
    CONNECTING,
    CONNECTED
};

std::map<std::string,int>& wifi_list();
void wifi_join(const char* ssid, const char* pwd);
std::string wifi_ssid();
WiFiState wifi_state();
void wifi_scan();
void wifi_disconnect();
void beep();

void nv_write(const char* str, int64_t pts);
int64_t nv_read(const char* str);

uint32_t cpu_ticks();
uint64_t us();
uint64_t ms();
void quiet();

extern "C"
void* malloc32(int size, const char* name);

#ifdef ESP_PLATFORM

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

class Q
{
    QueueHandle_t _q;
public:
    Q();
    void push(const void* evt);
    const void* pop();
    bool empty();
    int waiting();
};

#else

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

class Q
{
    std::queue<const void*> queue;
    mutable std::mutex guard;
    std::condition_variable signal;
public:
    void push(const void* data);
    const void* pop();
    bool empty();
    int waiting();
};
typedef void(*TaskFunction_t)(void *);

#define IRAM_ATTR
#define taskYIELD()

#endif

void start_thread(TaskFunction_t tf, void* arg, int core = 0);
int  get_events();
void clear_events(int i);
void wait_events(int i);
void set_events(int i);
void set_events_isr(int i);
int  coreid();
int  stack();

std::string to_string(int i);
void mem(const char* t);

class AddTicks {
    uint32_t& _dst;
    uint64_t _start;
public:
    AddTicks(uint32_t& dst) : _dst(dst),_start(cpu_ticks()) {}
    ~AddTicks() { _dst += cpu_ticks()-_start; }
};

class Buffer {
public:
    uint32_t len;
    uint8_t data[8*188];
};

#define GENERIC_OTHER   0x8000

#define GENERIC_FIRE_X  0x4000  // RETCON
#define GENERIC_FIRE_Y  0x2000
#define GENERIC_FIRE_Z  0x1000
#define GENERIC_FIRE_A  0x0800
#define GENERIC_FIRE_B  0x0400
#define GENERIC_FIRE_C  0x0200

#define GENERIC_RESET   0x0100     // ATARI FLASHBACK
#define GENERIC_START   0x0080
#define GENERIC_SELECT  0x0040
#define GENERIC_FIRE    0x0020
#define GENERIC_RIGHT   0x0010
#define GENERIC_LEFT    0x0008
#define GENERIC_DOWN    0x0004
#define GENERIC_UP      0x0002
#define GENERIC_MENU    0x0001

int get_hid_ir(uint8_t* hid);       // get a fake ir hid event

class Streamer
{
    int _socket = 0;
    FILE* _file = 0;
    const uint8_t* _rom = 0;

    int _content_length = 0;
    uint32_t _mark = 0;
    uint32_t _offset;
    uint64_t _start_ms;
public:
    int     get(const char* url, uint32_t offset = 0, uint32_t len = 0);
    int     get_url(const char* url, std::vector<uint8_t>& v, uint32_t offset = 0, uint32_t len = 0);
    void    get_rom(const uint8_t* rom, int len);
    ssize_t read(uint8_t* dst, uint32_t len, uint32_t* offset = 0);
    void    close();
};

int printf_nano(const char *fmt, ...);
#define printf printf_nano

#endif /* streamer_hpp */
