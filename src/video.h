
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

#ifndef video_h
#define video_h

#include "stdlib.h"
#include "stdio.h"
#include "stdint.h"

#define VIDEO_PIN   25
#define AUDIO_PIN   18  // can be any pin
#define IR_PIN      0   // TSOP4838 or equivalent on any pin if desired


#define FB_WIDTH 352
#define FB_HEIGHT 192
#define FB_STRIDE (FB_WIDTH*3/2) // nice if it is a constant
#define FB_SLICE_HEIGHT 16
#define FB_SLICES (FB_HEIGHT/FB_SLICE_HEIGHT)

class Frame {
public:
    uint8_t* _slices[FB_SLICES];
    void init();
    uint8_t* get_y(int y);
    uint8_t* get_cr(int y);
    uint8_t* get_cb(int y);
    void erase();
};

void video_init(int ntsc);
void video_reset();
void video_pause(int p);
void push_video(Frame* f, int front, int64_t pts, int mode);              // in video.h
void push_audio(const uint8_t* data, int len, int64_t pts, bool pes_complete);

#define VIDEO_COMPOSITE_WIDTH 80
#define VIDEO_COMPOSITE_HEIGHT 16
#define VIDEO_COMPOSITE_PROGRESS_WIDTH (352-VIDEO_COMPOSITE_WIDTH-32)
extern uint8_t _video_composite[VIDEO_COMPOSITE_HEIGHT*VIDEO_COMPOSITE_WIDTH];
extern int _video_composite_blend;
extern int _video_composite_progress;

#endif // video_h
