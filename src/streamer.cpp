//
//  streamer.cpp
//
//  Created by Peter Barrett on 6/10/20.
//  Copyright © 2020 Peter Barrett. All rights reserved.
//

#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"
#include "string.h"
#include "stdarg.h"

#include <vector>
#include <string>
#include <mutex>
using namespace std;

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include <lwip/sockets.h>
#include <lwip/dns.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#endif

#include "streamer.h"

// Teeny printf that behaves itself in a FreeRTOS multithread enviroment.

#define PUT(_c) putchar(_c)
const char* _chr = "0123456789ABCDEF";
mutex _printf_guard;

IRAM_ATTR
int printf_nano(const char *fmt, ...)
{
    unique_lock<mutex> lock(_printf_guard);
    //PUT('!');

    int r,i,width,pad;
    uint32_t uv;
    const char* s;
    char buf[16];

    va_list ap;
    va_start (ap, fmt);

    while (*fmt) {
        if (*fmt != '%')
            PUT(*fmt++);
        else {
            ++fmt;
            char c = *fmt++;
            r = i = 16;

            // get widths
            width = 0;
            pad = ' ';
            while (c >= '0' && c <= '9') {
                if (c == '0') {
                    pad = c;
                    c = *fmt++;
                }
                if (c >= '1' && c <= '9') {
                    width = c - '0';
                    c = *fmt++;
                }
            }

            // do format
            switch (c) {
                case 's':
                    s = va_arg(ap,const char *);
                    while (*s) {
                        if (width) width--;
                        PUT(*s++);
                    }
                    while (width--)
                        PUT(' ');
                    break;
                case 'd':
                    r = 10;
                case 'X':
                case 'x':
                case 'c':
                    uv = va_arg(ap, int);
                    if (c == 'c')
                        PUT(uv);
                    else {
                        if (c == 'd' && ((int)uv < 0)) {
                            PUT('-');
                            uv = -(int)uv;
                        }
                    }
                    do {
                        buf[--i] = _chr[uv % r];
                        uv /= r;
                    } while (uv);
                    while (16-i < width)
                        buf[--i] = pad;
                    while (i < 16)
                        PUT(buf[i++]);
                    break;
                default:
                    PUT(c);
                    break;
            }
        }
    }
    return 0;
}

#ifdef ESP_PLATFORM

extern "C" void* malloc32(int x, const char* label)
{
    printf("malloc32 %d free, %d biggest, allocating %s:%d\n",
      heap_caps_get_free_size(MALLOC_CAP_32BIT),heap_caps_get_largest_free_block(MALLOC_CAP_32BIT),label,x);
    void * r = heap_caps_malloc(x,MALLOC_CAP_32BIT);
    if (!r) {
        printf("malloc32 FAILED allocation of %s:%d!!!!####################\n",label,x);
        esp_restart();
    }
    else
        printf("malloc32 allocation of %s:%d %08X\n",label,x,r);
    return r;
}

void dns_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    *((ip_addr_t *)callback_arg) = *ipaddr;
    set_events(DNS_READY);
}

err_t gethostbyname (const char *hostname, ip_addr_t *addr)
{
    printf("gethostbyname %s\n", hostname);
    clear_events(DNS_READY);
    if (dns_gethostbyname(hostname,addr,dns_cb,addr) == 0)
        return 0;
    wait_events(DNS_READY);
    printf("gethostbyname %s:%s\n", hostname, ip4addr_ntoa(&addr->u_addr.ip4));
    return 0;
}

string to_string(int n)
{
    char buf[16];
    return itoa(n,buf,10);
}

int stack()
{
    return uxTaskGetStackHighWaterMark(NULL);
}

int coreid()
{
    return xPortGetCoreID();
}

Q::Q()
{
    _q = xQueueCreate(32,sizeof(void*));
}

void Q::push(const void* data)
{
    xQueueSend(_q,&data,portMAX_DELAY);
}

const void* Q::pop()
{
    void* data;
    xQueueReceive(_q,&data,portMAX_DELAY);
    return data;
}

bool Q::empty()
{
    return waiting() == 0;
}

int Q::waiting()
{
    return uxQueueMessagesWaiting(_q);
}

EventGroupHandle_t _event_group;
int get_events()
{
    return xEventGroupGetBits(_event_group);
}

void clear_events(int i)
{
    xEventGroupClearBits(_event_group, i);
}

void wait_events(int i)
{
    xEventGroupWaitBits(_event_group, i, false, true, portMAX_DELAY);
}

void set_events(int i)
{
    xEventGroupSetBits(_event_group,i);
}

IRAM_ATTR
void set_events_isr(int i)
{
    BaseType_t xHigherPriorityTaskWoken, xResult;
    xHigherPriorityTaskWoken = pdFALSE;
    xResult = xEventGroupSetBitsFromISR(
                                _event_group,   /* The event group being updated. */
                                i, /* The bits being set. */
                                &xHigherPriorityTaskWoken );
    if(xHigherPriorityTaskWoken)
        portYIELD_FROM_ISR();
}

void start_thread(TaskFunction_t tf, void* arg, int core)
{
    xTaskCreatePinnedToCore(tf, "demux", 3*1024, arg, 5, NULL, core);
}

IRAM_ATTR
uint32_t cpu_ticks()
{
    return xthal_get_ccount();
}

IRAM_ATTR
uint64_t us()
{
    return esp_timer_get_time();
}

IRAM_ATTR
uint64_t ms()
{
    return esp_timer_get_time()/1000;
}

#else

#include <sys/time.h>
#include <time.h>

uint32_t cpu_ticks()
{
    unsigned int lo,hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
   return ((uint64_t)hi << 32) | lo;
}

uint64_t us()
{
    return clock();
}

uint64_t ms()
{
    return clock()/1000;
}

typedef struct {
    uint32_t addr;
} ip4_addr_t;

typedef struct {
    union {
      //ip6_addr_t ip6;
      ip4_addr_t ip4;
    } u_addr;
    uint8_t type;
} ip_addr_t;

const char* ipaddr_ntoa(ip_addr_t* a)
{
    static char buf[16];
    uint32_t n = a->u_addr.ip4.addr;
    sprintf(buf,"%d.%d.%d.%d",(n >> 0)&0xFF,(n>>8)&0xFF,(n>>16)&0xFF,(n>>24)&0xFF);
    return buf;
}

int gethostbyname(const char *hostname, ip_addr_t *addr)
{
    struct hostent *hp = gethostbyname(hostname);
    if (!hp)
        return -1;
    auto* ha = (struct in_addr *)hp->h_addr_list[0];
    addr->u_addr.ip4.addr = (uint32_t)ha->s_addr;
    return 0;
}

std::mutex _event_guard;
std::condition_variable _event_signal;

int _event_group = 0;

int get_events()
{
    return _event_group;
}

void clear_events(int i)
{
    //std::thread::id this_id = std::this_thread::get_id();
    //printf("clear_events t:%d m:%04X\n",this_id,i);

    _event_group &= ~i;
    _event_signal.notify_all();
}

void wait_events(int i)
{
    //std::thread::id this_id = std::this_thread::get_id();
    //printf("wait_events t:%d m:%04X\n",this_id,i);
    
    std::unique_lock<std::mutex> lock(_event_guard);
    while (!(_event_group & i))
        _event_signal.wait(lock);
}

void set_events(int i)
{
    //std::thread::id this_id = std::this_thread::get_id();
    //printf("set_events t:%d m:%04X\n",this_id,i);

    _event_group |= i;
    _event_signal.notify_all();
    _event_signal.notify_all();
}

void set_events_isr(int i)
{
    set_events(i);
}

void Q::push(const void* data)
{
    {
        std::lock_guard<std::mutex> lock(guard);
        queue.push(data);
    }
    signal.notify_one();
}

const void* Q::pop()
{
    std::unique_lock<std::mutex> lock(guard);
    while (queue.empty())
        signal.wait(lock);
    const void* value = queue.front();
    queue.pop();
    return value;
}

bool Q::empty()
{
    std::unique_lock<std::mutex> lock(guard);
    return queue.empty();
}

int Q::waiting()
{
    return queue.size();
}

int stack()
{
    return -1;
}

int coreid()
{
    return -1;
}

void start_thread(TaskFunction_t tf, void* arg, int core)
{
    new std::thread(tf,arg);
}

extern "C" void* malloc32(int x, const char* label)
{
    printf("malloc32 %s %d\n",label,x);
    return malloc(x);
}

void mem(const char* t)
{
    printf("mem:%s\n",t);
}
#endif

//========================================================================================
//========================================================================================
// Streamer.
// Read from files, memory or http urls

int Streamer::get(const char* url, uint32_t offset, uint32_t len)
{
    printf("get %s %d %d\n",url,offset,len);
    _start_ms = ms()-1;
    _content_length = -1;
    _mark = 0;
    _offset = offset;
    close();

    ip_addr_t host_ip;

    if (strncmp(url,"file",4) == 0)
    {
        _file = fopen(url + 7,"rb");
        if (_file) {
            fseek(_file, 0, SEEK_END);
            _content_length = (int)ftell(_file) - offset;
            if (len)
                _content_length = min((int)len,_content_length);
            fseek(_file, offset, SEEK_SET);
        }
        return _file ? 0 : -1;
    }

    char host[100] = {0};
    char path[100] = {0};
    int host_port = 80;
    int n = sscanf(url, "http://%99[^:]:%99d/%99[^\n]",host, &host_port, path);
    if (n != 3)
        n = sscanf(url, "http://%99[^/]/%99[^\n]",host, path);

    if (gethostbyname(host, &host_ip) == -1)   // blocking
        return -1;

    _socket = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    printf("connecting to %s:%d %s : %08X\n",host,host_port,ipaddr_ntoa(&host_ip),host_ip.u_addr.ip4);
    addr.sin_addr.s_addr = host_ip.u_addr.ip4.addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(host_port);
    int err = ::connect(_socket, (struct sockaddr*)&addr, sizeof(addr));
    printf("connect: %d\n",err);
    if (err)
        return -1;

    // send an http request
    string req = "GET /" + string(path) + " HTTP/1.1\r\n";
    req += "Host: " + string(host) + ":" + ::to_string(host_port) + "\r\n";
    if (offset != 0 || len != 0) {
        req += "Range: bytes=" + to_string(offset) + "-";
        if (len)
            req += to_string(offset+len-1);
        req += "\r\n";
    }
    req += "User-Agent: espflix esp32\r\n\r\n";
    send(_socket,req.c_str(),req.length(),0);
    printf("%s",req.c_str());

    // Content-Range: bytes 0-1023/146515

    // read response/headers
    int status = 0;
    for (int h = 0; ; h++) {
        char line[256];
        char c = 0;
        for (int i = 0; (i < sizeof(line)-1); i++) {
            if (::recv(_socket,&c,1,0) != 1)
                break;
            if (c == '\n') {
                line[i-1] = 0;
                break;
            }
            line[i] = c;
        }
        if (c != '\n')
            return -1;

        if (h == 0)
            status = atoi(line+9);
        else {
            if (strncmp(line,"Content-Length:",15) == 0)
                _content_length = atoi(line+15);
        }
        printf("%s\n",line);
        if (strlen(line) == 0)
            return 0;               // ready for the body
    }
    return -1;
}

int Streamer::get_url(const char* url, vector<uint8_t>& v, uint32_t offset, uint32_t len)
{
    if (get(url,offset,len) || (_content_length == -1)) {
        close();
        return -1;
    }
    v.resize(_content_length);
    ssize_t n = read(&v[0],_content_length);
    close();
    return n >= 0 ? 0 : -1;
}

void Streamer::get_rom(const uint8_t* rom, int len)
{
    _rom = rom;
    _content_length = len;
    _mark = _offset = 0;
    _start_ms = ms();
}

ssize_t Streamer::read(uint8_t* dst, uint32_t len, uint32_t* offset)
{
    //printf("%dkbits/s\n",_mark*8/(int)(ms()-_start_ms));
    if (offset)
        *offset = _offset + _mark;
    len = min((uint32_t)(_content_length - _mark),len);
    if (_rom) {
        memcpy(dst,_rom,len);
        _rom += len;
        _mark += len;
        return len;
    }
    if (_file) {
        _mark += len;
        return fread(dst,1,len,_file);
    }
    if (_socket <= 0)
        return -1;
    int i = 0;
    while (i < len) {
        ssize_t n = min((uint32_t)(_content_length - _mark),len-i);
        if (n == 0)
            break;
        n = ::recv(_socket,dst,n,0);
        //printf("recv:%d\n",n);
        if (n <= 0)
            break;
        _mark += n;
        dst += n;
        i += n;
    }
    return i;
}

void Streamer::close()
{
    if (_socket)
        ::close(_socket);
    if (_file)
        fclose(_file);
    _rom = 0;
    _socket = 0;
    _file = 0;
    _mark = 0;
}

