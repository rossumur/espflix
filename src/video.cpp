
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

int _pal_ = 0;

#ifdef ESP_PLATFORM
#include "esp_wifi.h"
#include "tcpip_adapter.h"
#include "esp_event_loop.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_int_wdt.h"
#include "soc/rtc.h"

#include "soc/ledc_struct.h"
#include "rom/lldesc.h"
#include "driver/periph_ctrl.h"
#include "driver/dac.h"
#include "driver/gpio.h"
#include "driver/i2s.h"
#include <string.h> // memcpy

//====================================================================================================
//====================================================================================================
//
// low level HW setup of DAC/DMA/APLL/PWM
//

lldesc_t _dma_desc[4] = {0};
intr_handle_t _isr_handle;
intr_handle_t _audio_isr_handle;

extern "C" void IRAM_ATTR video_isr(volatile void* buf);
extern "C" void IRAM_ATTR audio_isr(volatile void* buf);

// simple isr
void IRAM_ATTR i2s_intr_handler_video(void *arg)
{
    if (I2S0.int_st.out_eof)
        video_isr(((lldesc_t*)I2S0.out_eof_des_addr)->buf); // get the next line of video
    I2S0.int_clr.val = I2S0.int_st.val;                     // reset the interrupt
}

void IRAM_ATTR audio_isr(volatile void* buf)
{
}

void IRAM_ATTR i2s_intr_handler_audio(void *arg)
{
    if (I2S1.int_st.out_eof)
        audio_isr(((lldesc_t*)I2S1.out_eof_des_addr)->buf); // get the next line of video
    I2S1.int_clr.val = I2S1.int_st.val;                     // reset the interrupt
}

void fill_audio(uint32_t* dst, int len);


#define DD(_x) printf(#_x);printf(" %d\n",_x)
#define HDD(_x) printf(#_x);printf(" %08X\n",_x)
static void dump()
{
  printf("\n\n\n");
  DD(I2S1.conf.tx_mono);
  DD(I2S1.conf.tx_right_first);
  DD(I2S1.clkm_conf.clkm_div_num);
  DD(I2S1.clkm_conf.clkm_div_b);
  DD(I2S1.clkm_conf.clkm_div_a);
  DD(I2S1.sample_rate_conf.tx_bck_div_num);
  DD(I2S1.sample_rate_conf.tx_bits_mod);
  DD(I2S1.conf_chan.tx_chan_mod);
  DD(I2S1.fifo_conf.tx_fifo_mod);

  HDD(I2S1.conf.val);
  HDD(I2S1.clkm_conf.val);
  HDD(I2S1.sample_rate_conf.val);
  HDD(I2S1.conf_chan.val);
  HDD(I2S1.fifo_conf.val);
}


// 2x 2k buffers?
lldesc_t _dma_desc_audio[2] = {0};
static esp_err_t start_audio()
{
    periph_module_enable(PERIPH_I2S1_MODULE);
    I2S1.conf.val = 1;
    I2S1.conf.val = 0;
    I2S1.conf.tx_short_sync = 0;
    I2S1.conf.tx_msb_shift = 0;
    I2S1.fifo_conf.tx_fifo_mod_force_en = 1;

    /*
    // setup interrupt
    if (esp_intr_alloc(ETS_I2S1_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM, // ESP_INTR_FLAG_LEVEL1
        i2s_intr_handler_audio, 0, &_audio_isr_handle) != ESP_OK)
        return -1;
     */

    // Create TX DMA buffers
    for (int i = 0; i < 2; i++) {
        int n = 2048; // 4092 is maximum
        _dma_desc_audio[i].buf = (uint8_t*)heap_caps_calloc(1, n, MALLOC_CAP_DMA);
        if (!_dma_desc_audio[i].buf)
            return -1;

        fill_audio((uint32_t*)(_dma_desc_audio[i].buf),n/4);

        _dma_desc_audio[i].owner = 1;
        _dma_desc_audio[i].eof = 1;
        _dma_desc_audio[i].length = n;
        _dma_desc_audio[i].size = n;
        _dma_desc_audio[i].empty = (uint32_t)(i == 1 ? _dma_desc_audio : _dma_desc_audio+1);
    }
    I2S1.out_link.addr = (uint32_t)_dma_desc_audio;

    I2S1.clkm_conf.clkm_div_num = 19;            // I2S clock divider’s integral value.
    I2S1.clkm_conf.clkm_div_b = 34;              // Fractional clock divider’s numerator value.
    I2S1.clkm_conf.clkm_div_a = 63;              // Fractional clock divider’s denominator value

    I2S1.sample_rate_conf.tx_bck_div_num = 4;
    I2S1.sample_rate_conf.tx_bits_mod = 32;

    I2S1.conf_chan.tx_chan_mod = 2;             // 0-two channel;1-right;2-left;3-right;4-left
    I2S1.fifo_conf.tx_fifo_mod = 3;             // 32-bit single channel data

    I2S1.conf.tx_start = 1;                     // start DMA!
    I2S1.int_clr.val = 0xFFFFFFFF;
    //I2S1.int_ena.out_eof = 1;
    I2S1.out_link.start = 1;

    dump();
    return esp_intr_enable(_audio_isr_handle);        // start interruprs!
}

static esp_err_t start_dma(int line_width,int samples_per_cc, int ch = 1)
{
    periph_module_enable(PERIPH_I2S0_MODULE);

    // setup interrupt
    if (esp_intr_alloc(ETS_I2S0_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM, // ESP_INTR_FLAG_LEVEL1
        i2s_intr_handler_video, 0, &_isr_handle) != ESP_OK)
        return -1;

    printf("dma interrupt on core %d\n",esp_intr_get_cpu(_isr_handle));

    // reset conf
    I2S0.conf.val = 1;
    I2S0.conf.val = 0;
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_mono = (ch == 2 ? 0 : 1);

    I2S0.conf2.lcd_en = 1;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.sample_rate_conf.tx_bits_mod = 16;
    I2S0.conf_chan.tx_chan_mod = (ch == 2) ? 0 : 1;

    // Create TX DMA buffers
    for (int i = 0; i < 2; i++) {
        int n = line_width*2*ch;
        if (n >= 4092) {
            printf("DMA chunk too big:%s\n",n);
            return -1;
        }
        _dma_desc[i].buf = (uint8_t*)heap_caps_calloc(1, n, MALLOC_CAP_DMA);
        if (!_dma_desc[i].buf)
            return -1;

        _dma_desc[i].owner = 1;
        _dma_desc[i].eof = 1;
        _dma_desc[i].length = n;
        _dma_desc[i].size = n;
        _dma_desc[i].empty = (uint32_t)(i == 1 ? _dma_desc : _dma_desc+1);
    }
    I2S0.out_link.addr = (uint32_t)_dma_desc;

    //  Setup up the apll: See ref 3.2.7 Audio PLL
    //  f_xtal = (int)rtc_clk_xtal_freq_get() * 1000000;
    //  f_out = xtal_freq * (4 + sdm2 + sdm1/256 + sdm0/65536); // 250 < f_out < 500
    //  apll_freq = f_out/((o_div + 2) * 2)
    //  operating range of the f_out is 250 MHz ~ 500 MHz
    //  operating range of the apll_freq is 16 ~ 128 MHz.
    //  select sdm0,sdm1,sdm2 to produce nice multiples of colorburst frequencies

    //  see calc_freq() for math: (4+a)*10/((2 + b)*2) mhz
    //  up to 20mhz seems to work ok:
    //  rtc_clk_apll_enable(1,0x00,0x00,0x4,0);   // 20mhz for fancy DDS

    if (!_pal_) {
        switch (samples_per_cc) {
            case 3: rtc_clk_apll_enable(1,0x46,0x97,0x4,2);   break;    // 10.7386363636 3x NTSC (10.7386398315mhz)
            case 4: rtc_clk_apll_enable(1,0x46,0x97,0x4,1);   break;    // 14.3181818182 4x NTSC (14.3181864421mhz)
        }
    } else {
        rtc_clk_apll_enable(1,0x04,0xA4,0x6,1);     // 17.734476mhz ~4x PAL
    }

    I2S0.clkm_conf.clkm_div_num = 1;            // I2S clock divider’s integral value.
    I2S0.clkm_conf.clkm_div_b = 0;              // Fractional clock divider’s numerator value.
    I2S0.clkm_conf.clkm_div_a = 1;              // Fractional clock divider’s denominator value
    I2S0.sample_rate_conf.tx_bck_div_num = 1;
    I2S0.clkm_conf.clka_en = 1;                 // Set this bit to enable clk_apll.
    I2S0.fifo_conf.tx_fifo_mod = (ch == 2) ? 0 : 1; // 32-bit dual or 16-bit single channel data

    dac_output_enable(DAC_CHANNEL_1);           // DAC, video on GPIO25
    dac_i2s_enable();                           // start DAC!

    I2S0.conf.tx_start = 1;                     // start DMA!
    I2S0.int_clr.val = 0xFFFFFFFF;
    I2S0.int_ena.out_eof = 1;
    I2S0.out_link.start = 1;
    return esp_intr_enable(_isr_handle);        // start interruprs!
}

void audio_init_hw();
void video_init_hw(int line_width, int samples_per_cc)
{
    // setup apll 4x NTSC or PAL colorburst rate
    start_dma(line_width,samples_per_cc,1);

    // Now ideally we would like to use the decoupled left DAC channel to produce audio
    // But when using the APLL there appears to be some clock domain conflict that causes
    // nasty digitial spikes and dropouts. You are also limited to a single audio channel.
    // So it is back to PWM/PDM and a 1 bit DAC for us. Good news is that we can do stereo
    // if we want to and have lots of different ways of doing nice noise shaping etc.

    // PWM audio out of pin 18 -> can be anything
    // lots of other ways, PDM by hand over I2S1, spi circular buffer etc
    // but if you would like stereo the led pwm seems like a fine choice
    // needs a simple rc filter (1k->1.2k resistor & 10nf->15nf cap work fine)

    // 18 ----/\/\/\/----|------- a out
    //          1k       |
    //                  ---
    //                  --- 10nf
    //                   |
    //                   v gnd


    audio_init_hw();
}

// send an audio sample every scanline (15720hz for ntsc, 15600hz for PAL)
inline void IRAM_ATTR audio_sample(uint8_t s)
{
    auto& reg = LEDC.channel_group[0].channel[0];
    reg.duty.duty = s << 4; // 25 bit (21.4)
    reg.conf0.sig_out_en = 1; // This is the output enable control bit for channel
    reg.conf1.duty_start = 1; // When duty_num duty_cycle and duty_scale has been configured. these register won't take effect until set duty_start. this bit is automatically cleared by hardware
    reg.conf0.clk_en = 1;
}

//  Appendix

/*
static
void calc_freq(double f)
{
    f /= 1000000;
    printf("looking for sample rate of %fmhz\n",(float)f);
    int xtal_freq = 40;
    for (int o_div = 0; o_div < 3; o_div++) {
        float f_out = 4*f*((o_div + 2)*2);          // 250 < f_out < 500
        if (f_out < 250 || f_out > 500)
            continue;
        int sdm = round((f_out/xtal_freq - 4)*65536);
        float apll_freq = 40 * (4 + (float)sdm/65536)/((o_div + 2)*2);    // 16 < apll_freq < 128 MHz
        if (apll_freq < 16 || apll_freq > 128)
            continue;
        printf("f_out:%f %d:0x%06X %fmhz %f\n",f_out,o_div,sdm,apll_freq/4,f/(apll_freq/4));
    }
    printf("\n");
}

static void freqs()
{
    calc_freq(PAL_FREQUENCY*3);
    calc_freq(PAL_FREQUENCY*4);
    calc_freq(NTSC_FREQUENCY*3);
    calc_freq(NTSC_FREQUENCY*4);
    calc_freq(20000000);
}
*/

#else

//====================================================================================================
//====================================================================================================
//  Simulator
//

#define IRAM_ATTR
#define DRAM_ATTR

#include "video.h"
void video_init_hw(int line_width, int samples_per_cc);

void audio_sample(uint8_t s);

void ir_sample();

int get_hid_ir(uint8_t* buf)
{
    return 0;
}

#endif

#include "video.h"
#include "math.h"
#include "unistd.h"
#include "sbc_decoder.h"
#include "streamer.h"

#ifdef ESP_PLATFORM
#include "ir_input.h"
#endif

//====================================================================================================
//====================================================================================================
// Color conversion tables -> will be copied into ram before use
const uint32_t uv_tab[512] = {
// u
0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,
0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,
0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307E00,0x30307D00,0x30307C00,
0x30307C00,0x30307B00,0x30307A00,0x30307900,0x30307900,0x30307800,0x30307700,0x30307700,
0x30307600,0x30307500,0x30307400,0x30307400,0x30307300,0x30307200,0x30307100,0x30307100,
0x30307000,0x30306F00,0x30306F00,0x30306E00,0x30306D00,0x30306C00,0x30306C00,0x30306B00,
0x30306A00,0x30306900,0x30306900,0x30306800,0x30306700,0x30306700,0x30306600,0x30306500,
0x30306400,0x30306400,0x30306300,0x30306200,0x30306100,0x30306100,0x30306000,0x30305F01,
0x30305F01,0x30305E02,0x30305D03,0x30305C04,0x30305C04,0x30305B05,0x30305A06,0x30305907,
0x30305907,0x30305808,0x30305709,0x30305709,0x3030560A,0x3030550B,0x3030540C,0x3030540C,
0x3030530D,0x3030520E,0x3030510F,0x3030510F,0x30305010,0x30304F11,0x30304F11,0x30304E12,
0x30304D13,0x30304C14,0x30304C14,0x30304B15,0x30304A16,0x30304917,0x30304917,0x30304818,
0x30304719,0x30304719,0x3030461A,0x3030451B,0x3030441C,0x3030441C,0x3030431D,0x3030421E,
0x3030411F,0x3030411F,0x30304020,0x30303F21,0x30303F21,0x30303E22,0x30303D23,0x30303C24,
0x30303C24,0x30303B25,0x30303A26,0x30303927,0x30303927,0x30303828,0x30303729,0x30303729,
0x3030362A,0x3030352B,0x3030342C,0x3030342C,0x3030332D,0x3030322E,0x3030312F,0x3030312F,
0x30303030,0x30302F31,0x30302F31,0x30302E32,0x30302D33,0x30302C34,0x30302C34,0x30302B35,
0x30302A36,0x30302937,0x30302937,0x30302838,0x30302739,0x30302739,0x3030263A,0x3030253B,
0x3030243C,0x3030243C,0x3030233D,0x3030223E,0x3030213F,0x3030213F,0x30302040,0x30301F41,
0x30301F41,0x30301E42,0x30301D43,0x30301C44,0x30301C44,0x30301B45,0x30301A46,0x30301947,
0x30301947,0x30301848,0x30301749,0x30301749,0x3030164A,0x3030154B,0x3030144C,0x3030144C,
0x3030134D,0x3030124E,0x3030114F,0x3030114F,0x30301050,0x30300F51,0x30300F51,0x30300E52,
0x30300D53,0x30300C54,0x30300C54,0x30300B55,0x30300A56,0x30300957,0x30300957,0x30300858,
0x30300759,0x30300759,0x3030065A,0x3030055B,0x3030045C,0x3030045C,0x3030035D,0x3030025E,
0x3030015F,0x3030015F,0x30300060,0x30300061,0x30300061,0x30300062,0x30300063,0x30300064,
0x30300064,0x30300065,0x30300066,0x30300067,0x30300067,0x30300068,0x30300069,0x30300069,
0x3030006A,0x3030006B,0x3030006C,0x3030006C,0x3030006D,0x3030006E,0x3030006F,0x3030006F,
0x30300070,0x30300071,0x30300071,0x30300072,0x30300073,0x30300074,0x30300074,0x30300075,
0x30300076,0x30300077,0x30300077,0x30300078,0x30300079,0x30300079,0x3030007A,0x3030007B,
0x3030007C,0x3030007C,0x3030007D,0x3030007E,0x3030007F,0x3030007F,0x3030007F,0x3030007F,
0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,
0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,
// v
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7E003030,0x7D003030,0x7C003030,
0x7C003030,0x7B003030,0x7A003030,0x79003030,0x79003030,0x78003030,0x77003030,0x77003030,
0x76003030,0x75003030,0x74003030,0x74003030,0x73003030,0x72003030,0x71003030,0x71003030,
0x70003030,0x6F003030,0x6F003030,0x6E003030,0x6D003030,0x6C003030,0x6C003030,0x6B003030,
0x6A003030,0x69003030,0x69003030,0x68003030,0x67003030,0x67003030,0x66003030,0x65003030,
0x64003030,0x64003030,0x63003030,0x62003030,0x61003030,0x61003030,0x60003030,0x5F013030,
0x5F013030,0x5E023030,0x5D033030,0x5C043030,0x5C043030,0x5B053030,0x5A063030,0x59073030,
0x59073030,0x58083030,0x57093030,0x57093030,0x560A3030,0x550B3030,0x540C3030,0x540C3030,
0x530D3030,0x520E3030,0x510F3030,0x510F3030,0x50103030,0x4F113030,0x4F113030,0x4E123030,
0x4D133030,0x4C143030,0x4C143030,0x4B153030,0x4A163030,0x49173030,0x49173030,0x48183030,
0x47193030,0x47193030,0x461A3030,0x451B3030,0x441C3030,0x441C3030,0x431D3030,0x421E3030,
0x411F3030,0x411F3030,0x40203030,0x3F213030,0x3F213030,0x3E223030,0x3D233030,0x3C243030,
0x3C243030,0x3B253030,0x3A263030,0x39273030,0x39273030,0x38283030,0x37293030,0x37293030,
0x362A3030,0x352B3030,0x342C3030,0x342C3030,0x332D3030,0x322E3030,0x312F3030,0x312F3030,
0x30303030,0x2F313030,0x2F313030,0x2E323030,0x2D333030,0x2C343030,0x2C343030,0x2B353030,
0x2A363030,0x29373030,0x29373030,0x28383030,0x27393030,0x27393030,0x263A3030,0x253B3030,
0x243C3030,0x243C3030,0x233D3030,0x223E3030,0x213F3030,0x213F3030,0x20403030,0x1F413030,
0x1F413030,0x1E423030,0x1D433030,0x1C443030,0x1C443030,0x1B453030,0x1A463030,0x19473030,
0x19473030,0x18483030,0x17493030,0x17493030,0x164A3030,0x154B3030,0x144C3030,0x144C3030,
0x134D3030,0x124E3030,0x114F3030,0x114F3030,0x10503030,0x0F513030,0x0F513030,0x0E523030,
0x0D533030,0x0C543030,0x0C543030,0x0B553030,0x0A563030,0x09573030,0x09573030,0x08583030,
0x07593030,0x07593030,0x065A3030,0x055B3030,0x045C3030,0x045C3030,0x035D3030,0x025E3030,
0x015F3030,0x015F3030,0x00603030,0x00613030,0x00613030,0x00623030,0x00633030,0x00643030,
0x00643030,0x00653030,0x00663030,0x00673030,0x00673030,0x00683030,0x00693030,0x00693030,
0x006A3030,0x006B3030,0x006C3030,0x006C3030,0x006D3030,0x006E3030,0x006F3030,0x006F3030,
0x00703030,0x00713030,0x00713030,0x00723030,0x00733030,0x00743030,0x00743030,0x00753030,
0x00763030,0x00773030,0x00773030,0x00783030,0x00793030,0x00793030,0x007A3030,0x007B3030,
0x007C3030,0x007C3030,0x007D3030,0x007E3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
};

const uint32_t sin_u[256] = {
0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,
0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,
0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307F00,0x30307E00,0x30307D00,0x30307C00,
0x30307C00,0x30307B00,0x30307A00,0x30307900,0x30307900,0x30307800,0x30307700,0x30307700,
0x30307600,0x30307500,0x30307400,0x30307400,0x30307300,0x30307200,0x30307100,0x30307100,
0x30307000,0x30306F00,0x30306F00,0x30306E00,0x30306D00,0x30306C00,0x30306C00,0x30306B00,
0x30306A00,0x30306900,0x30306900,0x30306800,0x30306700,0x30306700,0x30306600,0x30306500,
0x30306400,0x30306400,0x30306300,0x30306200,0x30306100,0x30306100,0x30306000,0x30305F01,
0x30305F01,0x30305E02,0x30305D03,0x30305C04,0x30305C04,0x30305B05,0x30305A06,0x30305907,
0x30305907,0x30305808,0x30305709,0x30305709,0x3030560A,0x3030550B,0x3030540C,0x3030540C,
0x3030530D,0x3030520E,0x3030510F,0x3030510F,0x30305010,0x30304F11,0x30304F11,0x30304E12,
0x30304D13,0x30304C14,0x30304C14,0x30304B15,0x30304A16,0x30304917,0x30304917,0x30304818,
0x30304719,0x30304719,0x3030461A,0x3030451B,0x3030441C,0x3030441C,0x3030431D,0x3030421E,
0x3030411F,0x3030411F,0x30304020,0x30303F21,0x30303F21,0x30303E22,0x30303D23,0x30303C24,
0x30303C24,0x30303B25,0x30303A26,0x30303927,0x30303927,0x30303828,0x30303729,0x30303729,
0x3030362A,0x3030352B,0x3030342C,0x3030342C,0x3030332D,0x3030322E,0x3030312F,0x3030312F,
0x30303030,0x30302F31,0x30302F31,0x30302E32,0x30302D33,0x30302C34,0x30302C34,0x30302B35,
0x30302A36,0x30302937,0x30302937,0x30302838,0x30302739,0x30302739,0x3030263A,0x3030253B,
0x3030243C,0x3030243C,0x3030233D,0x3030223E,0x3030213F,0x3030213F,0x30302040,0x30301F41,
0x30301F41,0x30301E42,0x30301D43,0x30301C44,0x30301C44,0x30301B45,0x30301A46,0x30301947,
0x30301947,0x30301848,0x30301749,0x30301749,0x3030164A,0x3030154B,0x3030144C,0x3030144C,
0x3030134D,0x3030124E,0x3030114F,0x3030114F,0x30301050,0x30300F51,0x30300F51,0x30300E52,
0x30300D53,0x30300C54,0x30300C54,0x30300B55,0x30300A56,0x30300957,0x30300957,0x30300858,
0x30300759,0x30300759,0x3030065A,0x3030055B,0x3030045C,0x3030045C,0x3030035D,0x3030025E,
0x3030015F,0x3030015F,0x30300060,0x30300061,0x30300061,0x30300062,0x30300063,0x30300064,
0x30300064,0x30300065,0x30300066,0x30300067,0x30300067,0x30300068,0x30300069,0x30300069,
0x3030006A,0x3030006B,0x3030006C,0x3030006C,0x3030006D,0x3030006E,0x3030006F,0x3030006F,
0x30300070,0x30300071,0x30300071,0x30300072,0x30300073,0x30300074,0x30300074,0x30300075,
0x30300076,0x30300077,0x30300077,0x30300078,0x30300079,0x30300079,0x3030007A,0x3030007B,
0x3030007C,0x3030007C,0x3030007D,0x3030007E,0x3030007F,0x3030007F,0x3030007F,0x3030007F,
0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,
0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,0x3030007F,
};

const uint32_t cos_v[256] = {
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7E003030,0x7D003030,0x7C003030,
0x7C003030,0x7B003030,0x7A003030,0x79003030,0x79003030,0x78003030,0x77003030,0x77003030,
0x76003030,0x75003030,0x74003030,0x74003030,0x73003030,0x72003030,0x71003030,0x71003030,
0x70003030,0x6F003030,0x6F003030,0x6E003030,0x6D003030,0x6C003030,0x6C003030,0x6B003030,
0x6A003030,0x69003030,0x69003030,0x68003030,0x67003030,0x67003030,0x66003030,0x65003030,
0x64003030,0x64003030,0x63003030,0x62003030,0x61003030,0x61003030,0x60003030,0x5F013030,
0x5F013030,0x5E023030,0x5D033030,0x5C043030,0x5C043030,0x5B053030,0x5A063030,0x59073030,
0x59073030,0x58083030,0x57093030,0x57093030,0x560A3030,0x550B3030,0x540C3030,0x540C3030,
0x530D3030,0x520E3030,0x510F3030,0x510F3030,0x50103030,0x4F113030,0x4F113030,0x4E123030,
0x4D133030,0x4C143030,0x4C143030,0x4B153030,0x4A163030,0x49173030,0x49173030,0x48183030,
0x47193030,0x47193030,0x461A3030,0x451B3030,0x441C3030,0x441C3030,0x431D3030,0x421E3030,
0x411F3030,0x411F3030,0x40203030,0x3F213030,0x3F213030,0x3E223030,0x3D233030,0x3C243030,
0x3C243030,0x3B253030,0x3A263030,0x39273030,0x39273030,0x38283030,0x37293030,0x37293030,
0x362A3030,0x352B3030,0x342C3030,0x342C3030,0x332D3030,0x322E3030,0x312F3030,0x312F3030,
0x30303030,0x2F313030,0x2F313030,0x2E323030,0x2D333030,0x2C343030,0x2C343030,0x2B353030,
0x2A363030,0x29373030,0x29373030,0x28383030,0x27393030,0x27393030,0x263A3030,0x253B3030,
0x243C3030,0x243C3030,0x233D3030,0x223E3030,0x213F3030,0x213F3030,0x20403030,0x1F413030,
0x1F413030,0x1E423030,0x1D433030,0x1C443030,0x1C443030,0x1B453030,0x1A463030,0x19473030,
0x19473030,0x18483030,0x17493030,0x17493030,0x164A3030,0x154B3030,0x144C3030,0x144C3030,
0x134D3030,0x124E3030,0x114F3030,0x114F3030,0x10503030,0x0F513030,0x0F513030,0x0E523030,
0x0D533030,0x0C543030,0x0C543030,0x0B553030,0x0A563030,0x09573030,0x09573030,0x08583030,
0x07593030,0x07593030,0x065A3030,0x055B3030,0x045C3030,0x045C3030,0x035D3030,0x025E3030,
0x015F3030,0x015F3030,0x00603030,0x00613030,0x00613030,0x00623030,0x00633030,0x00643030,
0x00643030,0x00653030,0x00663030,0x00673030,0x00673030,0x00683030,0x00693030,0x00693030,
0x006A3030,0x006B3030,0x006C3030,0x006C3030,0x006D3030,0x006E3030,0x006F3030,0x006F3030,
0x00703030,0x00713030,0x00713030,0x00723030,0x00733030,0x00743030,0x00743030,0x00753030,
0x00763030,0x00773030,0x00773030,0x00783030,0x00793030,0x00793030,0x007A3030,0x007B3030,
0x007C3030,0x007C3030,0x007D3030,0x007E3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
};

const uint32_t cos_v_neg[256] = {
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,
0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007F3030,0x007E3030,0x007D3030,0x007C3030,
0x007C3030,0x007B3030,0x007A3030,0x00793030,0x00793030,0x00783030,0x00773030,0x00773030,
0x00763030,0x00753030,0x00743030,0x00743030,0x00733030,0x00723030,0x00713030,0x00713030,
0x00703030,0x006F3030,0x006F3030,0x006E3030,0x006D3030,0x006C3030,0x006C3030,0x006B3030,
0x006A3030,0x00693030,0x00693030,0x00683030,0x00673030,0x00673030,0x00663030,0x00653030,
0x00643030,0x00643030,0x00633030,0x00623030,0x00613030,0x00613030,0x00603030,0x015F3030,
0x015F3030,0x025E3030,0x035D3030,0x045C3030,0x045C3030,0x055B3030,0x065A3030,0x07593030,
0x07593030,0x08583030,0x09573030,0x09573030,0x0A563030,0x0B553030,0x0C543030,0x0C543030,
0x0D533030,0x0E523030,0x0F513030,0x0F513030,0x10503030,0x114F3030,0x114F3030,0x124E3030,
0x134D3030,0x144C3030,0x144C3030,0x154B3030,0x164A3030,0x17493030,0x17493030,0x18483030,
0x19473030,0x19473030,0x1A463030,0x1B453030,0x1C443030,0x1C443030,0x1D433030,0x1E423030,
0x1F413030,0x1F413030,0x20403030,0x213F3030,0x213F3030,0x223E3030,0x233D3030,0x243C3030,
0x243C3030,0x253B3030,0x263A3030,0x27393030,0x27393030,0x28383030,0x29373030,0x29373030,
0x2A363030,0x2B353030,0x2C343030,0x2C343030,0x2D333030,0x2E323030,0x2F313030,0x2F313030,
0x30303030,0x312F3030,0x312F3030,0x322E3030,0x332D3030,0x342C3030,0x342C3030,0x352B3030,
0x362A3030,0x37293030,0x37293030,0x38283030,0x39273030,0x39273030,0x3A263030,0x3B253030,
0x3C243030,0x3C243030,0x3D233030,0x3E223030,0x3F213030,0x3F213030,0x40203030,0x411F3030,
0x411F3030,0x421E3030,0x431D3030,0x441C3030,0x441C3030,0x451B3030,0x461A3030,0x47193030,
0x47193030,0x48183030,0x49173030,0x49173030,0x4A163030,0x4B153030,0x4C143030,0x4C143030,
0x4D133030,0x4E123030,0x4F113030,0x4F113030,0x50103030,0x510F3030,0x510F3030,0x520E3030,
0x530D3030,0x540C3030,0x540C3030,0x550B3030,0x560A3030,0x57093030,0x57093030,0x58083030,
0x59073030,0x59073030,0x5A063030,0x5B053030,0x5C043030,0x5C043030,0x5D033030,0x5E023030,
0x5F013030,0x5F013030,0x60003030,0x61003030,0x61003030,0x62003030,0x63003030,0x64003030,
0x64003030,0x65003030,0x66003030,0x67003030,0x67003030,0x68003030,0x69003030,0x69003030,
0x6A003030,0x6B003030,0x6C003030,0x6C003030,0x6D003030,0x6E003030,0x6F003030,0x6F003030,
0x70003030,0x71003030,0x71003030,0x72003030,0x73003030,0x74003030,0x74003030,0x75003030,
0x76003030,0x77003030,0x77003030,0x78003030,0x79003030,0x79003030,0x7A003030,0x7B003030,
0x7C003030,0x7C003030,0x7D003030,0x7E003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,0x7F003030,
};

uint32_t _color_tab[256*3];

//====================================================================================================
//====================================================================================================

// Color clock frequency is 315/88 (3.57954545455)
// DAC_MHZ is 315/11 or 8x color clock
// 455/2 color clocks per line, round up to maintain phase
// HSYNCH period is 44/315*455 or 63.55555..us
// Field period is 262*44/315*455 or 16651.5555us

#define IRE(_x)          ((uint32_t)(((_x)+40)*255/3.3/147.5) << 8)   // 3.3V DAC
#define SYNC_LEVEL       IRE(-40)
#define BLANKING_LEVEL   IRE(0)
#define BLACK_LEVEL      IRE(7.5)
#define GRAY_LEVEL       IRE(50)
#define WHITE_LEVEL      IRE(100)


#define P0 (color >> 16)
#define P1 (color >> 8)
#define P2 (color)
#define P3 (color << 8)

volatile int _line_counter = 0;
volatile int _frame_counter = 0;

int _active_lines;
int _line_count;

int _line_width;
int _samples_per_cc;

float _sample_rate;

int _hsync;
int _hsync_long;
int _hsync_short;
int _burst_start;
int _burst_width;
int _active_start = 0;

int16_t* _burst0 = 0; // pal bursts
int16_t* _burst1 = 0;

static int usec(float us)
{
    uint32_t r = (uint32_t)(us*_sample_rate);
    return ((r + _samples_per_cc)/(_samples_per_cc << 1))*(_samples_per_cc << 1);  // multiple of color clock, word align
}

#define NTSC_COLOR_CLOCKS_PER_SCANLINE 228       // really 227.5 for NTSC but want to avoid half phase fiddling for now
#define NTSC_FREQUENCY (315000000.0/88)
#define NTSC_LINES 262

#define PAL_COLOR_CLOCKS_PER_SCANLINE 284        // really 283.75 ?
#define PAL_FREQUENCY 4433618.75
#define PAL_LINES 312

void pal_init();

SBC_Decode _sbc;

void video_init(int ntsc)
{
    _samples_per_cc = 4;
    uint32_t f = 15720;
    if (ntsc) {
        _sample_rate = 315.0/88 * _samples_per_cc;   // DAC rate
        _line_width = NTSC_COLOR_CLOCKS_PER_SCANLINE*_samples_per_cc;
        _line_count = NTSC_LINES;
        _hsync_long = usec(63.555-4.7);
        _active_start = usec(_samples_per_cc == 4 ? 10 : 10.5);
        _hsync = usec(4.7);
        _pal_ = 0;
        memcpy(_color_tab,uv_tab,256*4);
        memcpy(_color_tab+256,uv_tab+256,256*4);
        memcpy(_color_tab+512,uv_tab+256,256*4);
    } else {
        pal_init();
        memcpy(_color_tab,sin_u,256*4);
        memcpy(_color_tab+256,cos_v,256*4);
        memcpy(_color_tab+512,cos_v_neg,256*4);
        _pal_ = 1;
    }

    sbc_init(&_sbc);

    // PAL has 272 active lines - 40 lines top and bottom of letterbox w 192
    // NTSC has 240 active lines - 24 lines top and bottom of letterbox w 192
    _active_lines = 240;
    video_init_hw(_line_width,_samples_per_cc);    // init the hardware
}

//===================================================================================================
//===================================================================================================
// PAL

void pal_init()
{
    int cc_width = 4;
    _sample_rate = PAL_FREQUENCY*cc_width/1000000.0;       // DAC rate in mhz
    _line_width = PAL_COLOR_CLOCKS_PER_SCANLINE*cc_width;
    _line_count = PAL_LINES;
    _hsync_short = usec(2);
    _hsync_long = usec(30);
    _hsync = usec(4.7);
    _burst_start = usec(5.6);
    _burst_width = (int)(10*cc_width + 4) & 0xFFFE;
    _active_start = usec(10.4);

    // make colorburst tables for even and odd lines
    _burst0 = new int16_t[_burst_width];
    _burst1 = new int16_t[_burst_width];
    float phase = 2*M_PI/2;
    for (int i = 0; i < _burst_width; i++)
    {
        _burst0[i] = BLANKING_LEVEL + sin(phase + 3*M_PI/4) * BLANKING_LEVEL/1.5;
        _burst1[i] = BLANKING_LEVEL + sin(phase - 3*M_PI/4) * BLANKING_LEVEL/1.5;
        phase += 2*M_PI/cc_width;
    }
}

void IRAM_ATTR blit_pal(uint8_t* src, uint16_t* dst)
{
}

void IRAM_ATTR burst_pal(uint16_t* line)
{
    line += _burst_start;
    int16_t* b = (_line_counter & 1) ? _burst0 : _burst1;
    for (int i = 0; i < _burst_width; i += 2) {
        line[i^1] = b[i];
        line[(i+1)^1] = b[i+1];
    }
}

//===================================================================================================
//===================================================================================================

#define PERF
#ifndef min
#define min(_a,_b) (((_a) < (_b)) ? (_a) : (_b))
#define max(_a,_b) (((_a) > (_b)) ? (_a) : (_b))
#endif

#ifdef PERF
#define BEGIN_TIMING()  uint32_t t = cpu_ticks()
#define END_TIMING() t = cpu_ticks() - t; _blit_ticks_min = min(_blit_ticks_min,t); _blit_ticks_max = max(_blit_ticks_max,t);
#define ISR_BEGIN() uint32_t t = cpu_ticks()
#define ISR_END() t = cpu_ticks() - t;_isr_us += (t+120)/240;
uint32_t _blit_ticks_min = 0;
uint32_t _blit_ticks_max = 0;
uint32_t _isr_us = 0;
#else
#define BEGIN_TIMING()
#define END_TIMING()
#define ISR_BEGIN()
#define ISR_END()
#endif

#define CHROMA_EVEN(_u,_v) (((_color_tab[(uint8_t)(_u)] + _color_tab[256 + (uint8_t)(_v)]) & 0xFCFCFCFC) >> 2)
#define CHROMA_ODD(_u,_v) (((_color_tab[(uint8_t)(_u)] + _color_tab[512 + (uint8_t)(_v)]) & 0xFCFCFCFC) >> 2)

uint32_t dither4x4[] = {
    0x00020301, // 4 lines, 2 phases
    0x03010002,
    0x02030100,
    0x01000203,

    0x03010002,
    0x00020301,
    0x01000203,
    0x02030100,
};

// draw a line of video in NTSC
// horizontally interpolates luma
// could vertically interpolate chroma
// x must be a multiple of 8

void IRAM_ATTR blit(Frame* frame, uint16_t* dst, int line, int x, int width)
{
    BEGIN_TIMING();
    x &= ~3;
    uint8_t* y_ptr = frame->get_y(line) + x;
    uint32_t* u_ptr = (uint32_t*)(frame->get_cr(line>>1) + (x >> 1));
    uint32_t* v_ptr = (uint32_t*)(frame->get_cb(line>>1) + (x >> 1));

    if (_pal_)
        dst += 80;

    uint32_t dither = dither4x4[(line&3) + ((_frame_counter & 1) << 2)];   // cheap/fancy 4x4 dither
    uint32_t p0,p1,c;
    uint8_t lum = 0;    // interpolate luma

    if (line & 1) {
        int n = (line>>1) + (line == 191 ? 0 : 1);
        uint32_t* u_ptr2 = (uint32_t*)(frame->get_cr(n) + (x >> 1));    // interpolate chroma
        uint32_t* v_ptr2 = (uint32_t*)(frame->get_cb(n) + (x >> 1));

        for (int i = 0; i < width; i += 8) {
            uint32_t u4,v4;
            u4 = *u_ptr++;
            v4 = *v_ptr++;
            #if 1
            u4 = ((u4 >> 1) & 0x7F7F7F7F) + ((*u_ptr2++ >> 1) & 0x7F7F7F7F);
            v4 = ((v4 >> 1) & 0x7F7F7F7F) + ((*v_ptr2++ >> 1) & 0x7F7F7F7F);
            #endif

            p0 = (*((uint32_t*)y_ptr) + dither) & 0xFCFCFCFC;   // 3 2 1 0
            p1 = ((p0 >> 1)+(p0 >> 9)) & 0xFCFCFCFC;            // (3>>1)(2+3)(1+2)(0+1)
            p0 >>= 2;
            p1 >>= 2;

            c = CHROMA_ODD(u4,v4);

            lum = ((uint8_t)p0 + lum) >> 1;
            ((uint32_t*)dst)[0] = ((lum << 24) | ((p0 & 0xFF) << 8)) + c;
            ((uint32_t*)dst)[1] = ((p1 << 24) | (p0 & 0xFF00)) + (c << 8);

            c = CHROMA_ODD(u4>>8,v4>>8);

            ((uint32_t*)dst)[2] = ((p1 << 16) | (p0 >> 8)) + c;
            ((uint32_t*)dst)[3] = (((p1 << 8) & 0xFF000000) | (p0 >> 16)) + (c << 8);
            lum = p0 >> 24;
            dst += 8;

            p0 = (*((uint32_t*)(y_ptr+4)) + dither) & 0xFCFCFCFC;   // 3 2 1 0
            p1 = ((p0 >> 1)+(p0 >> 9)) & 0xFCFCFCFC;                // (3>>1)(2+3)(1+2)(0+1)
            p0 >>= 2;
            p1 >>= 2;

            c = CHROMA_ODD(u4>>16,v4>>16);

            lum = ((uint8_t)p0 + lum) >> 1;
            ((uint32_t*)dst)[0] = ((lum << 24) | ((p0 & 0xFF) << 8)) + c;
            ((uint32_t*)dst)[1] = ((p1 << 24) | (p0 & 0xFF00)) + (c << 8);

            c = CHROMA_ODD(u4>>24,v4>>24);

            ((uint32_t*)dst)[2] = ((p1 << 16) | (p0 >> 8)) + c;
            ((uint32_t*)dst)[3] = (((p1 << 8) & 0xFF000000) | (p0 >> 16)) + (c << 8);
            lum = p0 >> 24;

            dst += 8;
            y_ptr += 8;
        }

    } else {
        for (int i = 0; i < width; i += 8) {
            uint32_t u4,v4;
            u4 = *u_ptr++;
            v4 = *v_ptr++;

            p0 = (*((uint32_t*)y_ptr) + dither) & 0xFCFCFCFC;   // 3 2 1 0
            p1 = ((p0 >> 1)+(p0 >> 9)) & 0xFCFCFCFC;            // (3>>1)(2+3)(1+2)(0+1)
            p0 >>= 2;
            p1 >>= 2;

            c = CHROMA_EVEN(u4,v4);

            lum = ((uint8_t)p0 + lum) >> 1;
            ((uint32_t*)dst)[0] = ((lum << 24) | ((p0 & 0xFF) << 8)) + c;
            ((uint32_t*)dst)[1] = ((p1 << 24) | (p0 & 0xFF00)) + (c << 8);

            c = CHROMA_EVEN(u4>>8,v4>>8);

            ((uint32_t*)dst)[2] = ((p1 << 16) | (p0 >> 8)) + c;
            ((uint32_t*)dst)[3] = (((p1 << 8) & 0xFF000000) | (p0 >> 16)) + (c << 8);
            lum = p0 >> 24;
            dst += 8;

            p0 = (*((uint32_t*)(y_ptr+4)) + dither) & 0xFCFCFCFC;   // 3 2 1 0
            p1 = ((p0 >> 1)+(p0 >> 9)) & 0xFCFCFCFC;                // (3>>1)(2+3)(1+2)(0+1)
            p0 >>= 2;
            p1 >>= 2;

            c = CHROMA_EVEN(u4>>16,v4>>16);

            lum = ((uint8_t)p0 + lum) >> 1;
            ((uint32_t*)dst)[0] = ((lum << 24) | ((p0 & 0xFF) << 8)) + c;
            ((uint32_t*)dst)[1] = ((p1 << 24) | (p0 & 0xFF00)) + (c << 8);

            c = CHROMA_EVEN(u4>>24,v4>>24);

            ((uint32_t*)dst)[2] = ((p1 << 16) | (p0 >> 8)) + c;
            ((uint32_t*)dst)[3] = (((p1 << 8) & 0xFF000000) | (p0 >> 16)) + (c << 8);
            lum = p0 >> 24;

            dst += 8;
            y_ptr += 8;
        }
    }
    END_TIMING();
}

void IRAM_ATTR burst(uint16_t* line)
{
    if (_pal_) {
        burst_pal(line);
        return;
    }

    int i,phase;
    switch (_samples_per_cc) {
        case 4:
            // 4 samples per color clock
            for (i = _hsync; i < _hsync + (4*10); i += 4) {
                line[i+1] = BLANKING_LEVEL;
                line[i+0] = BLANKING_LEVEL + BLANKING_LEVEL/2;
                line[i+3] = BLANKING_LEVEL;
                line[i+2] = BLANKING_LEVEL - BLANKING_LEVEL/2;
            }
            break;
        case 3:
            // 3 samples per color clock
            phase = 0.866025*BLANKING_LEVEL/2;
            for (i = _hsync; i < _hsync + (3*10); i += 6) {
                line[i+1] = BLANKING_LEVEL;
                line[i+0] = BLANKING_LEVEL + phase;
                line[i+3] = BLANKING_LEVEL - phase;
                line[i+2] = BLANKING_LEVEL;
                line[i+5] = BLANKING_LEVEL + phase;
                line[i+4] = BLANKING_LEVEL - phase;
            }
            break;
    }
}

//  Compositing buffer for drawing elapsed time / progress bar
//  Also does fade to black etc
uint8_t _video_composite[VIDEO_COMPOSITE_HEIGHT*VIDEO_COMPOSITE_WIDTH];
int _video_composite_blend;     // -1 always show, 1-31 blend, >=32 show
int _video_composite_progress;

void IRAM_ATTR composite(uint16_t* dst, int line)
{
    if (!_video_composite_blend)    // don't show
        return;

    if (_pal_)
        dst += 80;

    uint32_t* d32 = (uint32_t*)dst;
    d32 += 8;
    const uint8_t* src = _video_composite + line*VIDEO_COMPOSITE_WIDTH;
    int n = VIDEO_COMPOSITE_WIDTH;
    int scale = 255/4;
    if (_video_composite_blend != -1 && _video_composite_blend < 32)
        scale = (scale*_video_composite_blend)>>5;

    while (n--) {
        uint32_t p = BLACK_LEVEL + *src++*scale;
        *d32++ = (p << 16) | p;
    }

    if (line < 3 || line > 8)
        return;

    // draw a progress bar
    d32 += 8;
    uint32_t c0 = BLACK_LEVEL + (scale << 8);
    uint32_t c1 = BLACK_LEVEL + (scale << 7);
    uint32_t a0 = (c0 << 16) | c0;
    uint32_t a1 = (c0 << 16) | c0;
    uint32_t b0 = (c1 << 16) | c1;
    uint32_t b1 = (c1 << 16) | c1;
    for (int i = 0; i < VIDEO_COMPOSITE_PROGRESS_WIDTH; i += 2)
    {
        if (i < _video_composite_progress) {
            *d32++ = a0;
            *d32++ = a1;
        } else {
            *d32++ = b0;
            *d32++ = b1;
        }
    }
}

void IRAM_ATTR sync(uint16_t* line, int syncwidth)
{
    for (int i = 0; i < syncwidth; i++)
        line[i] = SYNC_LEVEL;
}

inline void fill32(uint16_t* line, uint32_t c, int n)
{
    uint32_t* d32 = (uint32_t*)line;
    c |= c << 16;
    n >>= 1;
    while (n--)
        *d32++ = c;
}

void IRAM_ATTR blanking(uint16_t* line, bool vbl = false)
{
    int syncwidth = vbl ? _hsync_long : _hsync;
    sync(line,syncwidth);
    uint32_t *dst = (uint32_t*)(line + syncwidth);

    fill32(line+syncwidth,vbl ? BLANKING_LEVEL : BLACK_LEVEL,_line_width-syncwidth);

    if (!vbl)
        burst(line);    // no burst during vbl
}

// Fancy pal non-interlace
// http://martin.hinner.info/vga/pal.html
void IRAM_ATTR pal_sync2(uint16_t* line, int width, int swidth)
{
    swidth = swidth ? _hsync_long : _hsync_short;
    int i;
    for (i = 0; i < swidth; i++)
        line[i] = SYNC_LEVEL;
    for (; i < width; i++)
        line[i] = BLANKING_LEVEL;
}

uint8_t DRAM_ATTR _sync_type[8] = {0,0,0,3,3,2,0,0};
void IRAM_ATTR pal_sync(uint16_t* line, int i)
{
    uint8_t t = _sync_type[i-304];
    pal_sync2(line,_line_width/2, t & 2);
    pal_sync2(line+_line_width/2,_line_width/2, t & 1);
}

int8_t _next_frame = -1;
uint32_t _next_frame_time = 0;
int8_t _current_frame = -1;
int16_t _hscroll = 0;
int16_t _vscroll = 0;
int16_t _animate = 0;
int16_t _animate_index = 0;
Frame* _frames = 0;

uint32_t _audio_pts = 0;
uint32_t _video_pts = 0;
uint32_t _pts_origin = 0;
uint32_t _video_frame_counter_origin = 0;
uint32_t _pause_ = 0;

//========================================================================================
//========================================================================================
// Audio decode uses the lovely sbc codec -ar 48000 -b:a 192k
// 192k = 64 byte packets for 128 samples
// A 4k buffer is 1/6th of a second

uint8_t _sbc_buf[4096] = {0};
uint32_t _sbc_r = 0;
uint32_t _sbc_w = 0;
int _sbc_frame_size = 0;

void write_pcm_16(const int16_t* s, int n, int channels);

int decode_audio()
{
    if (!_sbc_frame_size) { // _sbc_frame_size == 64 at 32k
        if (_sbc_w == _sbc_r)
            return 0;
        int16_t dst[128];
        int bytes_decoded = 0;
        _sbc_frame_size = sbc_decoder(&_sbc,_sbc_buf,64,dst,sizeof(dst),&bytes_decoded);
        printf("frame_size = %d, bytes %d\n",_sbc_frame_size,bytes_decoded);
    }

    if (!_sbc_frame_size || (_sbc_w - _sbc_r < _sbc_frame_size))
        return 0;

    int16_t mono[128];          // mono for now
    const uint8_t* buf = _sbc_buf + (_sbc_r & (sizeof(_sbc_buf)-1));
    int fs = sbc_decoder(&_sbc,buf,_sbc_frame_size,mono,sizeof(mono),0);
    _sbc_r += _sbc_frame_size;

    if (fs != _sbc_frame_size)
        printf("#### _sbc_frame_size:%d\n",fs);
    write_pcm_16(mono,128,1);
    return 1;
}

// decode audio or generate PDM silence
extern "C" void audio_thread(void* arg)
{
    for (;;) {
        if (!_pause_) {
            while (decode_audio())
                ;
        }
        if (_sbc_w == _sbc_r) {
            write_pcm_16(0,128,1);  // silence
            write_pcm_16(0,128,1);  //
            printf(".\n");          // should not happen under normal playback
        }
        vTaskDelay(2);              // called in ui, when paused or when video is getting behind
    }
}

// add audio to ring buffer for audio_thread
void push_audio(const uint8_t* data, int len, int64_t pts, bool pes_complete)
{
    PLOG(PUSH_AUDIO);
    if (pts != -1)
        _audio_pts = uint32_t(pts/(_pal_ ? 1800 : 1500)); // convert to frame counter counts

    if ((_sbc_w - _sbc_r + len) > sizeof(_sbc_buf))
        printf("##### SBC OVERFLOW %d!\n",_sbc_w - _sbc_r + len);   // this should never happen

    while (len--)
        _sbc_buf[_sbc_w++ & ((sizeof(_sbc_buf)-1))] = *data++;
}

//========================================================================================
//========================================================================================

IRAM_ATTR
void push_video(Frame* f, int front, int64_t pts, int mode)
{
    PLOG(PUSH_VIDEO);
    _frames = f;
    pts /= _pal_ ? 1800 : 1500;     // convert to frame counter counts
    _video_pts = (uint32_t)pts;     // in frame times
    if (_video_frame_counter_origin == 0) {
        _pts_origin = _video_pts;
        _video_frame_counter_origin = _frame_counter;
    }
    uint32_t d = (_video_pts - _pts_origin) + _video_frame_counter_origin;    // when to display

    uint32_t bt = _blit_ticks_min;
    _blit_ticks_min = 0xFFFFFFFF;

    if (mode) {   // force immediate for displaying posters etc
        d = _frame_counter;
        _animate = mode;
    }

    // Queue the frame, wait for it to be presented.
    if (d < _frame_counter) {
        int late = _frame_counter - d;
        printf("v late:%d\n",late);
        if (late > 2) {
            printf("resetting v timing\n");
            _video_frame_counter_origin = 0;
        }
    }
    _next_frame_time = d;
    _next_frame = front;
    wait_events(VIDEO_READY);
    clear_events(VIDEO_READY);
}

// handle pausing
void video_pause(int p)
{
    if (p)
        _pause_ = _frame_counter;
    else {
        _video_frame_counter_origin = 0;
        _pause_ = 0;
    }
}

void video_reset()
{
    _sbc_r = _sbc_w = _sbc_frame_size = _pause_ = 0;
    _pts_origin = _video_frame_counter_origin = _video_pts = _audio_pts = 0;
}

// ease in / ease out animator updated 1 per frame
int16_t DRAM_ATTR _easd[16] = { 0,8,16,24, 48,72,104,136, 176,216,248,280, 304,328,336,344 };
void IRAM_ATTR animate()
{
    if (_animate_index == 0) {
        _hscroll = 0;
        return;
    }
    if (_animate_index < 0)
        _hscroll = -_easd[- ++_animate_index];
    else
        _hscroll = _easd[--_animate_index];
}

//========================================================================================
//========================================================================================
// Sampling profiler and Task profiler

#if 0
extern void* _trace_min;
uint16_t _tb[1280] = {0};   // collect a certain range of addresses starting at _trace_min
#define SAMPLING_PROF() \
if (_trace_min) { \
    int32_t addr = (int32_t)((uint8_t*)__builtin_return_address(2) - (uint8_t*)_trace_min); \
    addr >>= 2; \
    if ((addr >= 0) && (addr < sizeof(_tb)/2)) { \
        if (_tb[addr] != 0xFFFF) \
           _tb[addr]++; \
    } \
}
#else
#define SAMPLING_PROF()
#endif

#if 0
extern void task_prof();
#define TASK_PROF task_prof
#else
#define TASK_PROF()
#endif

//========================================================================================
//========================================================================================
// Workhorse ISR handles video updates
// Also handles scrolling between two frame buffers and compositing progress bar

extern "C"
void IRAM_ATTR video_isr(volatile void* vbuf)
{
    ISR_BEGIN();
    SAMPLING_PROF();
    TASK_PROF();

#ifdef IR_PIN
    ir_sample();
#endif

    int i = _line_counter++;
    uint16_t* buf = (uint16_t*)vbuf;
    const int _active_top = 32 + (_pal_ ? 32 : 0);      // center PAL
    const int _active_bottom = _active_top + 192;       // Fixed height of videos
    const int _vsync_start = _line_count - (_pal_ ? 8 : 3);

    // ntsc
    if ((i >= _active_top) && (i < _active_bottom) && (_current_frame != -1))
    {
        i -= _active_top;
        sync(buf,_hsync);
        burst(buf);
        uint16_t* dst = buf + _active_start + 16;
        int f = _current_frame;
        int h = _hscroll;
        if (h < 0) {
            h += 352;
            f ^= 1;
        }
        blit(&_frames[f],dst,i,h,352-h);
        if (h)
            blit(&_frames[f^1],dst + (352-h)*2,i,0,h);
    }
    else if (i >= _vsync_start)
    {
        if (_pal_)
            pal_sync(buf,i);                    // 8 lines of sync 304-312
        else
            blanking(buf,true);                 // 3 lines 259-262
    }
    else
    {
        if (_next_frame != -1) { // flip buffers in blanking
            if (_frame_counter >= _next_frame_time) {
                _current_frame = _next_frame;
                switch (_animate) {
                    case 2: _animate_index = -16; break;
                    case 3: _animate_index = 16; break;
                }
                _animate = 0;
                _next_frame = -1;
                animate();
                set_events_isr(VIDEO_READY);
                PLOG(VIDEO_READY_P);
            }
        }
        blanking(buf);                      // black line

        // draw time/progress bar
        int ptop = _active_bottom + 2;
        if (i >= ptop && i<(ptop+VIDEO_COMPOSITE_HEIGHT))
        {
            uint16_t* dst = buf + _active_start + 16;
            composite(dst,i-ptop);
        }
    }

    if (_line_counter == _line_count) {         // check for VBL
        _line_counter = 0;                      // frame is done
        _frame_counter++;
        if (_video_composite_blend > 0)
            --_video_composite_blend;
        animate();
    }
    ISR_END();
}
