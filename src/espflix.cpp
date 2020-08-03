
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
** WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
** ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
** SOFTWARE.
*/

#include "math.h"
#include "player.h"
#include "streamer.h"
#include "vmedia.h"

#include "video.h"
#include <unistd.h>
#include <string.h>

#include "font.h"
#include "splash.h"

#include <map>
using namespace std;

//========================================================================================
//========================================================================================

vector<string> split(const string &text, const string& sep)
{
    vector<string> tokens;
    ssize_t start = 0, end = 0;
    while ((end = text.find(sep, start)) != string::npos) {
        tokens.push_back(text.substr(start, end - start));
        start = end + sep.length();
    }
    if (start < text.length())
        tokens.push_back(text.substr(start));
    return tokens;
}

//========================================================================================
//========================================================================================

// draw text into mem32
enum Icon {
    STOP = 0x20-8,
    PAUSE,
    PLAY,
    FFWD,
    RWND
};

// Draw into
class Render {
    Frame* _f = 0;
public:
    int _color = 240;
    void set(Frame* f)
    {
        _f = f;
    }

    void erase()
    {
        if (_f)
            _f->erase();
        else
            memset(_video_composite,0,VIDEO_COMPOSITE_WIDTH*VIDEO_COMPOSITE_HEIGHT);
    }

    uint8_t* get_y(int y)
    {
        if (_f)
            return _f->get_y(y);
        return _video_composite + y*VIDEO_COMPOSITE_WIDTH;
    }

    // TODO. Alpha etc
    inline uint32_t p32(uint32_t src, int x, int p)
    {
        int s = (uint8_t)(src >> x*8);
        if (!p)
            p = s;
        else
            p = ((_color*p) + (255-p)*s) >> 8;  // alpha blend
        src &= ~(0xFF << x*8);
        return src | (p << x*8);
    }

    inline void unaligned(uint32_t* dst, int x, const uint8_t* src, int n)
    {
        while (n--) {
            int p = *src++;
            dst[x >> 2] = p32(dst[x >> 2],x&3,p);
            x++;
        }
    }

    inline void unaligned(uint32_t* dst, int x, int n, int p)
    {
        while (n--) {
            dst[x >> 2] = p32(dst[x >> 2],x&3,p);
            x++;
        }
    }

    void blit(const uint8_t* src, int src_rowbytes, int x, int y, int width, int height)
    {
        while (height--) {
            unaligned((uint32_t*)get_y(y++),x,src,width);
            src += src_rowbytes;
        }
    }

    int draw_char(int x, int y, char c)
    {
        c -= 0x20-8;
        int left = c ? _font8_rights[c-1] : 0;
        int right = _font8_rights[c];
        blit(_font8+left,1024,x,y,right-left,16);
        return right-left;
    }

    int measure_char(int c)
    {
        c -= 0x20-8;
        int left = c ? _font8_rights[c-1] : 0;
        return _font8_rights[c] - left;
    }

    int measure_text(const char* s)
    {
        int w = 0;
        while (*s)
            w += measure_char(*s++);
        return w;
    }

    void fill(int x, int y, int width, int height, int color)
    {
        while (height--)
            unaligned((uint32_t*)get_y(y++),x,width,color);
    }

    int draw_text(int x, int y, const char* str)
    {
        while (*str)
            x += draw_char(x,y,*str++);
        return x;
    }

};
Render _text;

void show_time(int s, int icon = ' ')
{
    int m = s / 60;
    int h = m / 60;
    char buf[16];
    if (h)
        sprintf(buf,"%c %d:%02d:%02d",icon,h,m%60,s%60);
    else
        sprintf(buf,"%c   %02d:%02d",icon,m%60,s%60);
    _text.erase();
    _text.draw_text(0,0,buf);
}

//========================================================================================
//========================================================================================
// GUI for WiFi/Password

const char* pwds[] = {
    "0123456789",
    "ABCDEFGHIJKLM",
    "NOPQRSTUVWXZY",
    "abcdefghijklm",
    "nopqrstuvwxyz",
    "!\"#$%&'()*+,-",
    "./:;<=>?@[\\]^",
    "_`{|}~",
};

#define CELL_WIDTH 20
#define CELL_HEIGHT 17
#define SSID_LINES 9

class gui {
    Frame* _f;
    int _findex = 0;
    Render _render;

    WiFiState _wifi_state = NONE;
    int _state = 0;
    int _dirty = 1;
    int _selected = 0;
    int _row = 0;
    int _col = 0;

    string _ssid;
    int _mode = 0;
    char _pwd[64] = {0};

public:
    gui(Frame* f) : _f(f),_state(0)
    {
    }

    void begin()
    {
        _render.set(&_f[_findex&1]);
        _render.erase();
    }

    void end()
    {
        push_video(_f,_findex++&1,0,1);
    }

    void draw_text(int x, int y, const string& txt)
    {
        if (x == -1)
            x = (352-_render.measure_text(txt.c_str()))/2;   // center
        _render.draw_text(x,y,txt.c_str());
    }

    void show_bars(int x, int y, int ss, int mode)
    {
        x += 13*CELL_WIDTH-2-_render.measure_text("lllll");
        int bars = (ss + 85)/10 + 1;
        if (bars > 5) bars = 5;
        for (int i = 0; i < 5; i++)
        {
            _render._color = i < bars ? 0xA0:0x40;
            x = _render.draw_text(x,y+3,"l");
            ss -= 10;
        }
        _render._color = 240;
    }

    void draw_ssid()
    {
        int y = CELL_HEIGHT;
        int x = 2*CELL_WIDTH;
        int i = 0;
        static int scroll = 0;
        if ((_selected - scroll) >= SSID_LINES)
            scroll = (_selected-SSID_LINES)+1;
        else if ((_selected - scroll) < 0)
            scroll += _selected - scroll;

        auto& ssids = wifi_list();
        for (auto& kv : ssids) {
            if (i >= scroll && ((i-scroll) < SSID_LINES)) {
                _render.fill(x,y+1,13*CELL_WIDTH,CELL_HEIGHT-2,i == _selected ? 0x40 : 0x10);
                if (i == _selected) {
                    _ssid = kv.first;
                    _mode = kv.second & 0xFF;
                }
                draw_text(x+8,y+3,kv.first);
                int ss = kv.second >> 8;
                int mode = kv.second & 0xFF;
                show_bars(x,y,ss,mode);
                y += CELL_HEIGHT;
            }
            i++;
        }
        draw_text(2*CELL_WIDTH,10*CELL_HEIGHT+3,"Select Access Point");
    }

    void key_ssid(int key, int down)
    {
        if (!down)
            return;

        switch (key) {
            case 0:
                usleep(10*1000);
                break;

            case 16:    // 'M' or menu
                break;

            case 19:    // Pause/Play or 'P'
                break;

            case 40:    // Center select button
                _state = 1;
                if (_mode == 0) {   // WIFI_AUTH_NONE
                    _pwd[0] = 0;
                    join();
                }
                dirty();
                break;

            case 81:     // down key
                if (_selected < (int)wifi_list().size()-1)
                    _selected++;
                dirty();
                break;

            case 82:     // up key
                if (_selected)
                    _selected--;
                dirty();
                break;
        }
    }

    void draw_cell(int x, int y, int c, int hilite)
    {
        char str[2] = {0};
        str[0] = c;
        draw_button(x,y,1,str,hilite);
    }

    void draw_button(int x, int y, int width, const char* label, int hilite)
    {
        x = (x+2)*CELL_WIDTH;
        y = (y+2)*CELL_HEIGHT;
        _render.fill(x+1,y,width*CELL_WIDTH-2,CELL_HEIGHT-2,hilite ? 0x40 : 0x10);
        x += (CELL_WIDTH*width - _render.measure_text(label))/2;
        _render.draw_text(x,y+2,label);
    }

    void draw_p()
    {
        int x = 2*CELL_WIDTH;
        int y = 1*CELL_HEIGHT;
        _render.fill(x,y,13*CELL_WIDTH,CELL_HEIGHT,0x60);
        const char* p = _pwd;
        while (_render.measure_text(p) > 12*CELL_WIDTH)
            p++;
        _render.draw_text(x+8,y+2,(string(p) + '_').c_str());
    }

    void dirty()
    {
        _dirty = 1;
    }

    void join()
    {
        if ((strlen(_pwd) >= 8) || (_mode == 0)) {
            _state = 2;
            dirty();
            update();
            wifi_join(_ssid.c_str(),_pwd);
        }
    }

    void key_pwd(int key, int down)
    {
        if (!down)
            return;

        switch (key) {
            case 0:
                usleep(10*1000);
                break;

            case 16:    // 'M' or menu
                break;

            case 19:    // Pause/Play or 'P'
                join();
                break;

            case 40:    // Center select button
            {
                const char* s = pwds[_row];
                int i = (int)strlen(_pwd);
                if (_col >= strlen(s)) {
                    if (i && _row == 0)
                        _pwd[i-1] = 0;
                    else if (_row == 7)
                    {
                        if (_col >= 9) {
                            join();
                        } else {
                            printf("back\n");
                            _state = 0;
                        }
                    }
                } else {
                    if (i < 63)
                        _pwd[i] = s[_col];
                }
            }
                dirty();
                break;

            case 79:    // right
                if (_row == 7) {
                    if (_col >= 6 && _col < 9)
                        _col = 9;
                }
                if (_col < 12)
                    _col++;
                dirty();
                break;

            case 80:    // left
                if (_row == 0 && _col > 10)
                    _col = 10;
                if (_row == 7) {
                    if (_col >= 9)
                        _col = 9;
                    else if (_col > 6)
                        _col = 6;
                }
                if (_col > 0)
                    _col--;
                dirty();
                break;

            case 81:     // down key
                if (_row < 7) _row++;
                dirty();
                break;

            case 82:     // up key
                if (_row) _row--;
                dirty();
                break;
        }
    }

    void draw_pwd()
    {
        draw_p();
        for (int y = 0; y < 8; y++)
        {
            const char* c = pwds[y];
            for (int x = 0; *c; x++) {
                bool h = (y == _row) && (x == _col);
                draw_cell(x,y,*c++,h);
            }
        }
        draw_button(10,0,3,"del",(_row == 0) && (_col >= 10));
        draw_button(6,7,3,"back",(_row == 7) && (_col >= 6 && _col < 9));
        draw_button(9,7,4,"join",(_row == 7) && (_col >= 9));

        draw_text(2*CELL_WIDTH,10*CELL_HEIGHT+3,"Enter Password");
    }

    void draw_info()
    {
        draw_text(-1,4*CELL_HEIGHT+3,"Connecting To");
        draw_text(-1,5*CELL_HEIGHT+3,wifi_ssid().c_str());
    }

    void update()
    {
        if (_dirty) {
            begin();
            switch (_state) {
                case 0: draw_ssid();    break;
                case 1: draw_pwd();     break;
                case 2: draw_info();    break;
            }
            end();
            _dirty = 0;
        }
    }

    void service_error()
    {
        draw_text(-1,4*CELL_HEIGHT+3,"Can't connect to service");
    }

    void close()
    {
        if (_findex & 1) {
            begin();
            end();
        }
    }

    int key(int k, bool keydown)
    {
        // IP, SCANNING, SCAN_RESULT etc

        WiFiState s = wifi_state();
        if (s != _wifi_state)
        {
            _wifi_state = s;
            dirty();
            switch (s) {
                case CONNECTED:
                    close();
                    return 1;       // got an IP, connect is complete

                case SCANNING:
                case SCAN_COMPLETE:
                    _state = 0;     // back to scanning
                    break;

                case CONNECTING:    //
                    _state = 2;
                    break;
            }
        } else {
            if (s == CONNECTED)
                return -1;
        }

        switch (_state) {
            case 0: key_ssid(k,keydown);    break;
            case 1: key_pwd(k,keydown);     break;
           // case 2: key_info(k,keydown);    break;
        }
        update();
        return 0;
    }
};

//========================================================================================
//========================================================================================

#define BOOT "http://rossumur.s3.amazonaws.com/espflix/service.txt"

extern "C" void demux_thread(void* arg);
extern "C" void audio_thread(void* arg);
void up_key();
void down_key();

int get_nec();

static
const char* pts_str(int64_t pts)
{
    if (pts < 0)
        return "invalid pts";
    int h = (int)(pts/(90000*60*60));
    int m = (int)(pts/(90000*60) % 60);
    int s = (int)(pts/(90000) % 60);
    int f = (int)(pts/(90000/30) % 30);
    static char buf[32];
    sprintf(buf,"%02D:%02D:%02D:%02D",h,m,s,f);
    return buf;
}

class ESPFlix {
    int _standard;
    int _pictures;

    enum State {
        NONE = 0,
        GUI,
        PLAYING,
        PAUSED,
        STOPPED,
        FAST_FORWARD,
        REWIND,
        SKIP_FORWARD,
        SKIP_BACK,
        NAV,
        DONE
    };
    State _pending = NONE;
    State _state = NONE;
    int _nav = -1;
    int _speed = 0;

    // indexes allow us to move from normal/fwd/rwd timestamps and random access points in video.
    typedef struct {
        int64_t first_pts;
        int64_t last_pts;
        uint32_t bin_size;
        uint32_t trick_speed;
        uint32_t sample_count;
    } idx_rec;

    typedef struct idx_hdr {
        uint32_t sig;
        uint32_t len;   // 3
        idx_rec video;
        idx_rec fwd;
        idx_rec rwd;

        int64_t map_pts(int64_t pts, const idx_rec& r)
        {
            pts -= r.first_pts;
            pts *= video.last_pts-video.first_pts;
            return pts/(r.last_pts - r.first_pts);
        }

        // translate to main pts at specified speed
        int64_t pts2pts(int64_t pts, int speed)
        {
            switch (speed) {
                case 1: return video.first_pts + map_pts(pts,fwd);
                case -1: return video.last_pts - map_pts(pts,rwd);
            }
            return pts;
        }

        // translate main pts to random access point offset at specified speed
        uint32_t pts2offset(int64_t pts, int speed)
        {
            uint32_t offset;
            pts = max(min(pts,video.last_pts),video.first_pts);
            switch (speed) {
                case 1:
                    offset = (uint32_t)(pts-video.first_pts)/fwd.trick_speed/fwd.bin_size;
                    offset = min(fwd.sample_count-1,offset);
                    offset += video.sample_count;
                    break;
                case -1:
                    offset = (uint32_t)((video.last_pts - pts)-video.first_pts)/rwd.trick_speed/rwd.bin_size;
                    offset = min(rwd.sample_count-1,offset);
                    offset += video.sample_count + fwd.sample_count;
                    break;
                default:
                    offset = (uint32_t)((pts-video.first_pts)/video.bin_size);
                    offset = min(video.sample_count-1,offset);
                    break;
            }
            return offset*4 + sizeof(idx_hdr);
        }
    } idx_hdr;

    typedef struct {
        int64_t pos;
        idx_hdr idx;
    } info;
    vector<info> _info;
    int64_t _last_seconds = 0;

    string _service_root;
    vector<string> _manifest;

    Frame _frame_buffers[2];

    MpegDecoder _decoder;
    Streamer _streamer;

    Q _events;
    const char* _vid_names[3] = {"/video_rwd.ts","/video.ts","/video_fwd.ts"};

    gui _gui;
public:
    ESPFlix(int ntsc) : _standard(ntsc),_decoder(&_frame_buffers[0],&_frame_buffers[1]),_gui(_frame_buffers)
    {
        //gen_palettes();
        _frame_buffers[0].init();
        _frame_buffers[1].init();
        _pictures = 0;
        start_thread(demux_thread,this);
        start_thread(audio_thread,0,1);
    }

    // Demux/Decode Thread on core 0
    void demux()
    {
        _decoder.run();
    }

    vector<string> get_list(const string& url)
    {
        vector<uint8_t> v;
        vector<string> s;
        if (_streamer.get_url(url.c_str(),v) == 0 && !v.empty())
            s = split(string((const char*)&v[0],v.size()),"\n");
        return s;
    }

    int init_service()
    {
        auto s = get_list(BOOT);
        if (s.size() == 0) {
            printf("Can't load %s\n",BOOT);
            return -1;
        }
        _service_root = s[0];
        //_service_root = "http://rossumur.s3.amazonaws.com/espflix/service11/";

        if (!_service_root.empty()) {
            _manifest = get_list(_service_root + "manifest.txt");
            if (!_manifest.empty()) {
                _info.resize(_manifest.size());
                _state = NAV;
                return 0;
            }
        }
        return -1;
    }

    void run()
    {
        play_rom(splash_ts,sizeof(splash_ts));
        _state = GUI;
        loop();
    }

    void nav(int i)
    {
        if (i < 0 || i >= _manifest.size())
            return;
        hide_progress();
        printf("nav to %d\n",i);
        load_poster(i,_nav == -1 ? 0 : _nav - i);
        _nav = i;

        // get duration / trick mode info
        if (!_info[i].idx.sig) {
            vector<uint8_t> hdr;
            _streamer.get_url((folder(i) + "/video.idx").c_str(),hdr,0,sizeof(idx_hdr));
            _info[i].idx = *((idx_hdr*)&hdr[0]);
        }
        _info[i].pos = nv_read(_manifest[i].c_str());   // last point saved?
    }

    // fill buffers and forward them to the decoder
    int decode_next()
    {
        PLOG(WAIT_BUFFER);
        Buffer* b = (Buffer*)_decoder.pop_empty();
        if (!b)
            return -1;  // paused

        PLOG(REQUEST_BUFFER);
        int n = (int)_streamer.read(b->data,(int)sizeof(b->data));
        PLOG(RECEIVED_BUFFER);

        b->len = n;     // may be n, 0, or -1
        _decoder.push_full(b);
        return n;
    }

    string folder(int i)
    {
        return _service_root + "media/" + _manifest[i];
    }

    void stream(const string& url, uint32_t offset)
    {
        _streamer.get(url.c_str(),offset);
    }

    const char* state_name(int s)
    {
        switch (s) {
            case NONE:      return "NONE";
            case PLAYING:   return "PLAYING";
            case FAST_FORWARD:   return "FAST_FORWARD";
            case REWIND:    return "REWIND";
            case PAUSED:    return "PAUSED";
            case STOPPED:   return "STOPPED";
            case NAV:       return "NAV";
        }
        return "weird";
    }

    void set_state(State s)
    {
        printf("State->%s\n",state_name(s));
        if (_state == s)
            return;
        switch (s) {
            case NAV:
                nav(_nav == -1 ? 0:_nav);
                break;
            default:
                break;
        }
        _state = s;
    }

    void menu()
    {
        if (_state == PLAYING) {
            _pending = NAV;
            pause();
        } else
            set_state(NAV);
    }

    void play(int i, int speed = 0, uint32_t offset = 0)
    {
        show_progress(speed == 0 ? 180 : -1);
        printf("playing at offset %d\n",offset);
        const char* s = _vid_names[speed+1];
        _speed = speed;
        stream(folder(i) + s,offset);
        _decoder.reset();
        video_reset();
        set_state(PLAYING);
        set_events(DECODER_RUN);
    }

    void pause()
    {
        clear_events(DECODER_RUN);  // pause
    }

    void play_pause()
    {
        if (_state == PLAYING) {
            pause();
        } else {
            if (_state == NAV) {
                play(_nav,0,get_index(0,_info[_nav].pos)*188);
            } else if (_state == PAUSED) {
                video_pause(0);
                set_events(DECODER_RUN);
                set_state(PLAYING);
                show_progress();
            }
        }
        _last_seconds = 0;
    }

    // map from a pts to a random access point
    uint32_t get_index(int speed, int64_t pts)
    {
        uint32_t offset = _info[_nav].idx.pts2offset(pts,speed);
        vector<uint8_t> buf;
        _streamer.get_url((folder(_nav) + "/video.idx").c_str(),buf,offset,4);
        return *((uint32_t*)&buf[0]);
    }

    void fast_forward()
    {
        auto& n = _info[_nav];
        play(_nav,1,get_index(1,n.pos)*188);
    }

    void rewind()
    {
        auto& n = _info[_nav];
        play(_nav,-1,get_index(-1,n.pos)*188);
    }

    void skip(int s)
    {
        auto& n = _info[_nav];
        n.pos += s*90000;
        play(_nav,0,get_index(0,n.pos)*188);
    }

    // just paused, save main pts
    void save_pos(int64_t pts, bool write2nv)
    {
        printf("save_pos from %s->",pts_str(pts));
        pts = _info[_nav].idx.pts2pts(pts,_speed);
        printf("%s %d\n",pts_str(pts),_speed);
        _info[_nav].pos = pts;
        if (write2nv)
            nv_write(_manifest[_nav].c_str(),pts);
    }

    // update time if required
    void update_progress()
    {
        if (_nav == -1)
            return; // in splash screen
        int64_t pts = _info[_nav].idx.pts2pts(_decoder.get_pts(),_speed);   // main pts
        int seconds = (int)(pts/90000);
        if (seconds != _last_seconds) {
            int i = _speed == 0 ? (_state == PAUSED ? PAUSE : PLAY) : (_speed == 1 ? FFWD : RWND);
            show_time((int)seconds,i);
            _last_seconds = seconds;
        }
        _video_composite_progress = (int)(pts*VIDEO_COMPOSITE_PROGRESS_WIDTH/_info[_nav].idx.video.last_pts);
    }

    void hide_progress()
    {
        _video_composite_blend = 0;
    }

    void show_progress(int t = 180)
    {
        _video_composite_blend = t;
    }

    void loop()
    {
        for (;;)
        {
            int key = 0;
            bool keydown = key_event(&key);

            // give wifi gui first dibs
            switch (_gui.key(key,keydown)) {
                case 0:
                    continue;   // handled it
                case 1:
                    if (init_service() == 0)
                        nav(0);     // wifi just connected, show posters for the first time
                    else
                        _gui.service_error();
                    break;
            }

            // Paused triggers state changes
            if ((_state == PLAYING) && (get_events() & DECODER_PAUSED) && (!(get_events() & DECODER_RUN))) {
                int64_t pts = _decoder.get_pts();
                printf("paused at %s speed %d\n",pts_str(pts),_speed);
                save_pos(_pending == DONE ? 0 : pts,_pending == NAV);
                show_progress();
                if (_speed && _pending != DONE) {
                    _state = NAV;   // Play/Pause while in trick mode -> PLAYING
                    play_pause();
                } else {
                    switch (_pending) {
                        case NAV:
                        case DONE:
                            set_state(NAV);
                            break;
                        case REWIND:
                            rewind();
                            break;
                        case FAST_FORWARD:
                            fast_forward();
                            break;
                        case SKIP_FORWARD:
                            skip(30);
                            break;
                        case SKIP_BACK:
                            skip(-30);
                            break;
                        default:
                            video_pause(1);
                            set_state(PAUSED);
                            _last_seconds = 0;
                    }
                }
                _pending = NONE;
            }

            switch (key) {
                case 0:
                    if (_state == PLAYING) {
                        int n = decode_next();
                        if (n == -1)
                            printf("decode_next %d, player is waiting\n",n);
                        else {
                            if (n == 0)
                                _pending = DONE;
                        }
                    } else {
                        usleep(1000);
                    }
                    update_progress();
                    break;

                case 16:    // 'M' or menu
                    if (keydown)
                        menu();
                    break;

                case 19:    // Pause/Play or 'P'
                case 40:    // Center select button
                    if (keydown)
                        play_pause();
                    break;

                case 79:    // right
                case 80:    // left
                    if (keydown) {
                        bool left = key == 80;
                        switch (_state) {
                            case NAV:
                                nav(_nav + (left ? -1 : 1));
                                break;
                            case PLAYING:
                                _pending = left ? REWIND : FAST_FORWARD;
                                pause();
                                break;
                            case PAUSED:
                                if (left)
                                    rewind();
                                else
                                    fast_forward();
                                break;
                        }
                    }
                    break;

                case 82:
                    if (keydown && (_state == PLAYING)) {
                        _pending = SKIP_FORWARD;
                        pause();
                    }
                    up_key();
                    break;

                case 81:
                    if (keydown && (_state == PLAYING)) {
                        _pending = SKIP_BACK;
                        pause();
                    }
                    down_key();
                    break;

                default:
                    ;
            }
        }
    }

    #define APPLE_MENU      0x40
    #define APPLE_PLAY      0x7A
    #define APPLE_CENTER    0x3A
    #define APPLE_RIGHT     0x60
    #define APPLE_LEFT      0x10
    #define APPLE_UP        0x50
    #define APPLE_DOWN      0x30

    bool key_event(int* k)
    {
        *k = 0;
        int nec = get_nec();
        if (nec) {
            printf("nec:%04X\n",nec);
            switch ((nec >> 8) & 0x7F) {
                case APPLE_MENU:    *k = 16;  break;    // M
                case APPLE_PLAY:    *k = 19;  break;    // P
                case APPLE_CENTER:  *k = 40;  break;    // select/enter
                case APPLE_RIGHT:   *k = 79;  break;    // RIGHT
                case APPLE_LEFT:    *k = 80;  break;    // LEFT
                case APPLE_DOWN:    *k = 81;  break;    // DOWN
                case APPLE_UP:      *k = 82;  break;    // UP
            }
            if (*k)
                beep(); // make a little feedback noise
            return 1;
        }
        return 0;
    }

    // modal
    void play_rom(const uint8_t* data, int len)
    {
        _streamer.get_rom(data,len);
        _decoder.reset();
        set_state(PLAYING);
        set_events(DECODER_RUN);
        int menuk = 0;
        while (decode_next()) {
            int key = 0;
            if (menuk == 0 && key_event(&key) && key == 16) {      // menu on boot
                wifi_disconnect();                  // force disconnect to enter gui
                menuk++;
            }
        }
        wait_events(DECODER_PAUSED);
    }

    void load_poster(int i, int dir)
    {
        stream(folder(i) + "/poster.ts",0);
        _decoder.reset();
        set_events(DECODER_RUN);
        while (decode_next())
            ;
        wait_events(DECODER_PAUSED);
        _decoder.flush_picture(dir == 0 ? 1 : (dir < 0 ? 2 : 3));
    }

    int RUP(float v)
    {
        if (v < 0)
            return -(RUP(-v));
        int i = (v+0.5);
        return i;
    }

    // swizzle chroma to match blitter arragement
    uint32_t swaz(uint32_t uv)
    {
        // 0123 -> 0213
        return (uv & 0xFF0000FF) | ((uv >> 8) & 0xFF00) | ((uv << 8) & 0xFF0000);
    }

    static int pin(int p)
    {
        return p < 0 ? 0 : (p < 127 ? p : 127);
    }
    
    virtual void gen_palettes()
    {
        for (int i = 0; i < 256; i++) {
            int c = (i << 16) | (i << 8) | i;
            printf("0x%08X,",c);
            if ((i & 7) == 7)
                printf("\n");
        }

        const uint8_t ordered8x8[64] = {
             0, 32,  8, 40,  2, 34, 10, 42,
            48, 16, 56, 24, 50, 18, 58, 26,
            12, 44,  4, 36, 14, 46,  6, 38,
            60, 28, 52, 20, 62, 30, 54, 22,
             3, 35, 11, 43,  1, 33,  9, 41,
            51, 19, 59, 27, 49, 17, 57, 25,
            15, 47,  7, 39, 13, 45,  5, 37,
            63, 31, 55, 23, 61, 29, 53, 21
        };

        #define IRE(_x)          ((uint32_t)(((_x)+40)*255/3.3/147.5) << 8)   // 3.3V DAC
        #define BLACK_LEVEL      IRE(7.5)

        float phase = 0*2*M_PI/360;    // TODO
        int black = BLACK_LEVEL >> 8;
        float uscale = (float)black/33;   // saturation
        float vscale = (float)black/33;

        printf("const uint32_t uv_tab[512] = {\n");
        printf("// u\n");
        for (int c = 0; c < 256; c++) {
            int u = 128-c;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int p = RUP(sin(2*M_PI*i/4 + phase)*u*uscale) + 2*black;
                v = (v << 8) | pin(p);
            }
            printf("0x%08X,",swaz(v));
            if ((c & 7) == 7)
                printf("\n");
        }
        printf("// v\n");

        for (int c = 0; c < 256; c++) {
            int vv = 128-c;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int p = RUP(cos(2*M_PI*i/4 + phase)*vv*vscale) + 2*black;
                v = (v << 8) | pin(p);
            }
            printf("0x%08X,",swaz(v));
            if ((c & 7) == 7)
                printf("\n");
        }
        printf("};\n\n");

        printf("const uint32_t sin_u[256] = {\n");  // pal
        for (int c = 0; c < 256; c++) {
            int u = 128-c;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int p = RUP(sin(2*M_PI*i/4)*u*uscale) + 2*black;
                v = (v << 8) | pin(p);
            }
            printf("0x%08X,",swaz(v));
            if ((c & 7) == 7)
                printf("\n");
        }
        printf("};\n\n");

        printf("const uint32_t cos_v[256] = {\n");
        for (int c = 0; c < 256; c++) {
            int vv = 128-c;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int p = RUP(cos(2*M_PI*i/4)*vv*vscale) + 2*black;
                v = (v << 8) | pin(p);
            }
            printf("0x%08X,",swaz(v));
            if ((c & 7) == 7)
                printf("\n");
        }
        printf("};\n\n");

        printf("const uint32_t cos_v_neg[256] = {\n");
        for (int c = 0; c < 256; c++) {
            int vv = 128-c;
            uint32_t v = 0;
            for (int i = 0; i < 4; i++) {
                int p = RUP(-cos(2*M_PI*i/4)*vv*vscale) + 2*black;
                v = (v << 8) | pin(p);
            }
            printf("0x%08X,",swaz(v));
            if ((c & 7) == 7)
                printf("\n");
        }
        printf("};\n\n");

        printf("const uint32_t dither8x8[16] = {\n");
        for (int i = 0; i < 8; i++) {
            uint32_t d = 0;
            for (int j = 0; j < 4; j++)
                d = (d << 8) | ordered8x8[i*8+3-j]/8;
            printf("0x%08X,",d);
            for (int j = 0; j < 4; j++)
                d = (d << 8) | ordered8x8[i*8+4+3-j]/8;
            printf("0x%08X,\n",d);
        }
        printf("};\n\n");
    }
};

void demux_thread(void* arg)
{
    printf("demux_thread S:%d %08X\n",stack(),arg);
    ((ESPFlix*)arg)->demux();
}

ESPFlix* _espflix = 0;
void espflix_run(int standard)
{
    video_init(standard);
    _espflix = new ESPFlix(standard);
    _espflix->run();
}

