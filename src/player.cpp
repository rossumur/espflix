//
//  player.cpp
//
//  Created by Peter Barrett on 6/10/20.
//  Copyright © 2020 Peter Barrett. All rights reserved.
//

#include "stdlib.h"
#include "stdio.h"
#include "unistd.h"
#include "string.h"

#include <vector>
#include <string>
using namespace std;

#include "player.h"
#include "streamer.h"

// Frame Buffer is for a 352x192 display
// 16x(352*3/2) == 8448 byte chunks
// YYYYUU
// YYYYVV

void Frame::init()
{
    for (int i = 0; i < FB_SLICES; i++) {
        _slices[i] = (uint8_t*)malloc32(FB_STRIDE*FB_SLICE_HEIGHT + 4,"FB");  // +4 is to allow overread in mocomp
        memset(_slices[i],0,FB_STRIDE*FB_SLICE_HEIGHT + 4);
    }
}

uint8_t* IRAM_ATTR Frame::get_y(int y)
{
    return _slices[y >> 4] + (y&0xF)*FB_STRIDE;
}

uint8_t* IRAM_ATTR Frame::get_cr(int y)
{
    return _slices[y >> 3] + (y&0x7)*FB_STRIDE + FB_WIDTH;
}

uint8_t* IRAM_ATTR Frame::get_cb(int y)
{
    return _slices[y >> 3] + ((y&0x7) + 8)*FB_STRIDE + FB_WIDTH;
}

void Frame::erase()
{
    for (int i = 0; i < FB_SLICES; i++)
        memset(_slices[i],0x30,FB_STRIDE*FB_SLICE_HEIGHT + 4);
}

//========================================================================================
//========================================================================================
// Inspired by Java MPEG-1 Video Decoder and Player
// KORANDI Zoltan <korandi_z@users.sourceforge.net>

const uint32_t macroblock_address_increment[75] = {
    0x01020000,0x03040000,0x00000001,0x05060000,0x07080000,0x090A0000,0x0B0C0000,0x00000003,
    0x00000002,0x0D0E0000,0x0F100000,0x00000005,0x00000004,0x11120000,0x13140000,0x00000007,
    0x00000006,0x15160000,0x17180000,0x191A0000,0x1B1C0000,0xFF1D0000,0xFF1E0000,0x1F200000,
    0x21220000,0x23240000,0x25260000,0x00000009,0x00000008,0x27280000,0x292A0000,0x2B2C0000,
    0x2D2E0000,0x0000000F,0x0000000E,0x0000000D,0x0000000C,0x0000000B,0x0000000A,0x2FFF0000,
    0xFF300000,0x31320000,0x33340000,0x35360000,0x37380000,0x393A0000,0x3B3C0000,0x3DFF0000,
    0xFF3E0000,0x3F400000,0x41420000,0x43440000,0x45460000,0x47480000,0x494A0000,0x00000015,
    0x00000014,0x00000013,0x00000012,0x00000011,0x00000010,0x00000023,0x00000022,0x00000021,
    0x00000020,0x0000001F,0x0000001E,0x0000001D,0x0000001C,0x0000001B,0x0000001A,0x00000019,
    0x00000018,0x00000017,0x00000016
};

const uint32_t macroblock_type_I[4] = {
    0x01020000,0xFF030000,0x00000001,0x00000011
};

const uint32_t macroblock_type_P[14] = {
    0x01020000,0x03040000,0x0000000A,0x05060000,0x00000002,0x07080000,0x00000008,0x090A0000,
    0x0B0C0000,0xFF0D0000,0x00000012,0x0000001A,0x00000001,0x00000011
};

const uint32_t macroblock_type_B[22] = {
    0x01020000,0x03050000,0x04060000,0x08070000,0x0000000C,0x090A0000,0x0000000E,0x0D0E0000,
    0x0C0B0000,0x00000004,0x00000006,0x12100000,0x0F110000,0x00000008,0x0000000A,0xFF130000,
    0x00000001,0x14150000,0x0000001E,0x00000011,0x00000016,0x0000001A
};

const uint32_t coded_block_pattern[126] = {
    0x02010000,0x03060000,0x04050000,0x080B0000,0x0C0D0000,0x09070000,0x0A0E0000,0x14130000,
    0x12100000,0x17110000,0x1B190000,0x151C0000,0x0F160000,0x181A0000,0x0000003C,0x23280000,
    0x2C300000,0x26240000,0x2A2F0000,0x1D1F0000,0x27200000,0x00000020,0x2D2E0000,0x21290000,
    0x2B220000,0x00000004,0x1E250000,0x00000008,0x00000010,0x0000002C,0x32380000,0x0000001C,
    0x00000034,0x0000003E,0x3D3B0000,0x343C0000,0x00000001,0x37360000,0x0000003D,0x00000038,
    0x393A0000,0x00000002,0x00000028,0x333E0000,0x00000030,0x403F0000,0x31350000,0x00000014,
    0x0000000C,0x50530000,0x0000003F,0x4D4B0000,0x41490000,0x54420000,0x00000018,0x00000024,
    0x00000003,0x45570000,0x514F0000,0x44470000,0x464E0000,0x434C0000,0x484A0000,0x56550000,
    0x58520000,0xFF5E0000,0x5F610000,0x00000021,0x00000009,0x6A6E0000,0x66740000,0x00000005,
    0x0000000A,0x5D590000,0x00000006,0x00000012,0x00000011,0x00000022,0x71770000,0x67680000,
    0x5A5C0000,0x6D6B0000,0x75760000,0x65630000,0x62600000,0x645B0000,0x72730000,0x696C0000,
    0x706F0000,0x797D0000,0x00000029,0x0000000E,0x00000015,0x7C7A0000,0x787B0000,0x0000000B,
    0x00000013,0x00000007,0x00000023,0x0000000D,0x00000032,0x00000031,0x0000003A,0x00000025,
    0x00000019,0x0000002D,0x00000039,0x0000001A,0x0000001D,0x00000026,0x00000035,0x00000017,
    0x0000002B,0x0000002E,0x0000002A,0x00000016,0x00000036,0x00000033,0x0000000F,0x0000001E,
    0x00000027,0x0000002F,0x00000037,0x0000001B,0x0000003B,0x0000001F
};

const uint32_t motion_vec[67] = {
    0x01020000,0x04030000,0x00000000,0x06050000,0x08070000,0x0000FFFF,0x00000001,0x090A0000,
    0x0C0B0000,0x00000002,0x0000FFFE,0x0E0F0000,0x100D0000,0x14120000,0x00000003,0x0000FFFD,
    0x11130000,0xFF170000,0x1B190000,0x1A150000,0x18160000,0x201C0000,0x1D1F0000,0xFF210000,
    0x24230000,0x0000FFFC,0x1E220000,0x00000004,0x0000FFF9,0x00000005,0x25290000,0x0000FFFB,
    0x00000007,0x26280000,0x2A270000,0x0000FFFA,0x00000006,0x33360000,0x32310000,0x2D2E0000,
    0x342F0000,0x2B350000,0x2C300000,0x0000000A,0x00000009,0x00000008,0x0000FFF8,0x39420000,
    0x0000FFF7,0x3C400000,0x383D0000,0x373E0000,0x3A3F0000,0x0000FFF6,0x3B410000,0x0000000C,
    0x00000010,0x0000000D,0x0000000E,0x0000000B,0x0000000F,0x0000FFF0,0x0000FFF4,0x0000FFF2,
    0x0000FFF1,0x0000FFF5,0x0000FFF3
};

// not used at runtime
const uint32_t dct_coeff[224] = {
    0x01020000,0x04030000,0x00000001,0x07080000,0x06050000,0x0D090000,0x0B0A0000,0x0E0C0000,
    0x00000101,0x14160000,0x12150000,0x10130000,0x00000201,0x110F0000,0x00000002,0x00000003,
    0x1B190000,0x1D1F0000,0x181A0000,0x201E0000,0x00000401,0x171C0000,0x00000301,0x00000102,
    0x00000701,0x0000FFFF,0x00000601,0x25240000,0x00000501,0x23220000,0x27260000,0x212A0000,
    0x28290000,0x34320000,0x36350000,0x30310000,0x2B2D0000,0x2E2C0000,0x00000801,0x00000004,
    0x00000202,0x00000901,0x332F0000,0x37390000,0x3C380000,0x3B3A0000,0x3D3E0000,0x00000A01,
    0x00000D01,0x00000006,0x00000103,0x00000005,0x00000302,0x00000B01,0x00000C01,0x4C4B0000,
    0x43460000,0x49470000,0x4E4A0000,0x484D0000,0x45400000,0x443F0000,0x42410000,0x51570000,
    0x5B500000,0x524F0000,0x53560000,0x5D5C0000,0x54550000,0x5A5E0000,0x58590000,0x00000203,
    0x00000104,0x00000007,0x00000402,0x00000502,0x00001001,0x00000F01,0x00000E01,0x696B0000,
    0x6F720000,0x68610000,0x7D770000,0x60620000,0xFF7B0000,0x5F650000,0x6A790000,0x63660000,
    0x71670000,0x70740000,0x6E640000,0x7C730000,0x757A0000,0x6D760000,0x786C0000,0x7F880000,
    0x8B8C0000,0x827E0000,0x91920000,0x80810000,0x00000802,0x84860000,0x9B9A0000,0x00000008,
    0x89850000,0x8F900000,0x978A0000,0x8E8D0000,0x0000000A,0x00000009,0x0000000B,0x00001501,
    0x00000602,0x00000303,0x00001401,0x00000702,0x00001101,0x00001201,0x00001301,0x94980000,
    0x00000403,0x99960000,0x00000105,0x83870000,0x00000204,0x95930000,0xACAD0000,0xA29E0000,
    0xAAA10000,0xA8A60000,0x9DB30000,0xA9A70000,0xAEAB0000,0xB2B10000,0x9C9F0000,0xA4A50000,
    0xB7B60000,0xAFB00000,0x00000107,0x00000A02,0x00000902,0x00001601,0x00001701,0x00001901,
    0x00001801,0x00000503,0x00000304,0x0000000D,0x0000000C,0x0000000E,0x0000000F,0x00000205,
    0x00001A01,0x00000106,0xB4B50000,0xA0A30000,0xC4C70000,0x0000001B,0xCBB90000,0xCAC90000,
    0x00000013,0x00000016,0xC5CF0000,0x00000012,0xBFC00000,0xBCBE0000,0x00000014,0xB8C20000,
    0x00000015,0xBAC10000,0x00000017,0xCCC60000,0x00000019,0x00000018,0xC8CD0000,0x0000001F,
    0x0000001E,0x0000001C,0x0000001D,0x0000001A,0x00000011,0x00000010,0xBDCE0000,0xBBC30000,
    0xDAD30000,0x00000025,0xD7D80000,0x00000024,0xD2D40000,0x00000022,0xD5D10000,0xDDDE0000,
    0xDBD00000,0xD9D60000,0xDFDC0000,0x00000023,0x0000010B,0x00000028,0x0000010C,0x0000010A,
    0x00000020,0x00000108,0x00000109,0x00000026,0x0000010D,0x0000010E,0x00000021,0x00000027,
    0x00001F01,0x00001B01,0x00001E01,0x00001002,0x00001D01,0x00001C01,0x0000010F,0x00000112,
    0x00000111,0x00000110,0x00000603,0x00000B02,0x00000E02,0x00000D02,0x00000C02,0x00000F02
};

uint8_t zig_zag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

uint8_t scale_dct_q[] = {
    32, 44, 42, 38, 32, 25, 17,  9,
    44, 62, 58, 52, 44, 35, 24, 12,
    42, 58, 55, 49, 42, 33, 23, 12,
    38, 52, 49, 44, 38, 30, 20, 10,
    32, 44, 42, 38, 32, 25, 17,  9,
    25, 35, 33, 30, 25, 20, 14,  7,
    17, 24, 23, 20, 17, 14,  9,  5,
     9, 12, 12, 10,  9,  7,  5,  2
};

const uint8_t default_intra_q[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

// pin between 0-248 to prevent dither overflow
// TODO. mem access is probably slower than explit check
const uint8_t _pin[256+256+256] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
    0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x61,0x62,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x6B,0x6C,0x6D,0x6E,0x6F,
    0x70,0x71,0x72,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xEB,0xEC,0xED,0xEE,0xEF,
    0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
    0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,0xF8,
};
const uint8_t* _pin_ = _pin + 256;
#define PIN(_x) _pin_[(_x)]

//========================================================================================
//========================================================================================


#include <map>
#include <string>
using namespace std;

void make_vlc_tab()
{
    map<string,int16_t> nv;
    const uint32_t* vlc = dct_coeff;
    int tab_bits = 16;
    for (int b = 0; b < (1<<tab_bits); b++) {
        int m = (1 << (tab_bits-1));
        int bit = b;
        uint8_t state = 0;
        int n = 0;
        char s[32] = {0};
        do {
            state = vlc[state] >> ((bit & m) ? 16 : 24);
            if (state == 0xFF)
                break;
            s[n] = (bit & m) ? '1' : '0';
            bit <<= 1;
        } while ((++n < tab_bits) && (vlc[state] >> 24));
        if (state != 0xFF)
            nv[s] = (int16_t)vlc[state];
    }
    int len = 0;
    for (auto kv : nv) {
        uint8_t run = kv.second >> 8;
        uint8_t level = (uint8_t)kv.second;
        if (len != kv.first.length()) {
            printf("\n");
            len = kv.first.length();
        }
        //printf("%s : %d %d,%d\n",kv.first.c_str(),kv.first.length(),run,level);
        printf("C(%d,%d), ",run,level);
    }
/*
    printf("// 8:bits8:coeff16\n");
    printf("uint32t _vlc_dct_fast[2048] = {\n");
    for (int i = 0; i < 2048; i++)
    {
        printf("0x%08X,",_vlc_dct_fast_[i]);
        if ((i & 0x7) == 0x7)
            printf("\n");
    }
    printf("};\n");
 */
}

//========================================================================================
//========================================================================================

inline uint16_t be16(const uint8_t* d)
{
    return (d[0] << 8) | d[1];
}

static int64_t parse_pts(const uint8_t* d, int flags)
{
    flags = (flags >> 2) & 0x30;
    if ((d[0] & 0xF0) != flags)
        return -1;
    int64_t n = ((int64_t)(d[0] & 0x0E)) << 29;
    n += (be16(d+1) >> 1) << 15;
    return n + (be16(d+3) >> 1);
}
void* _trace_min = (void*)(&parse_pts);

static const char* marker_name(uint8_t m)
{
    switch (m) {
        case 0x00:  return "PICTURE";
        case 0xB2:  return "USER_DATA";
        case 0xB3:  return "SEQUENCE";
        case 0xB5:  return "SEQUENCE_EXTENSION";
        case 0xB7:  return "SEQUENCE_END";
        case 0xB8:  return "GOP";
        case 0xB9:  return "PACK_END";
        case 0xBA:  return "PACK_START";
        case 0xBB:  return "SYSTEM_HEADER";
        case 0xE0:  return "VIDEO_PES";
        case 0xC0:  return "AUDIO_PES";
    }
    static char c[16];
    if (m >= 0x01 && m <= 0xAF)
        sprintf(c,"SLICE:%02X",m);
    else
        sprintf(c,"### BAD MARKER:%02X",m);
    return c;
}

#if 0
uint32_t _picture_ticks = 0;
uint32_t _predict_ticks = 0;
uint32_t _block_ticks = 0;
uint32_t _idct_ticks = 0;
uint32_t _vlc_ticks = 0;
#define MEASURE(_m) AddTicks ticks(_m)
#define REPORT() if (!_picture_ticks) _picture_ticks++; printf("MPEG: %d p:%d%% b:%d%% i:%d%% v:%d%%\n", \
_picture_ticks/240,_predict_ticks*100/_picture_ticks,_block_ticks*100/_picture_ticks,_idct_ticks*100/_picture_ticks,_vlc_ticks*100/_picture_ticks); \
_picture_ticks = _block_ticks = _predict_ticks = _idct_ticks = _vlc_ticks = 0;
#else
#define MEASURE(_m)
#define REPORT()
#endif

#define FILL_BITS() \
while (_b_count < 24) { \
    _b = (_b << 8) | ((_data < _end) ? *_data++ : more()); \
    _b_count += 8; \
}

MpegDecoder::MpegDecoder(Frame* fb0, Frame* fb1)
{
    _fb[0] = fb0;
    _fb[1] = fb1;
    _fb_index = 0;
    _reference = _fb[_fb_index++ & 1];
    _current = _fb[_fb_index & 1];
    _last_pts = _pts = _audio_pts = -1;

    // reset data source
    _b_count = _b = 0;
    _data = _end = 0;

    for (int i = 0; i < 4; i++)
        _empty_q.push(new Buffer());
}

void MpegDecoder::push_full(Buffer* b)
{
    _full_q.push(b);
}

Buffer* MpegDecoder::pop_empty()
{
    return (Buffer*)_empty_q.pop();
}

int MpegDecoder::demux(int pid, const uint8_t* d, const uint8_t* end, int payload_unit_start)
{
    const uint8_t* payload = d;
    int64_t pts = -1;
    int64_t dts = -1;
    int expected = 0;
    if (payload_unit_start)
    {
        // stream_marker = d[3];
        expected = be16(d+4);  // always zero for video

        d += 6;
        int flags = be16(d);
        payload = d + 3 + d[2];
        if (expected)
            expected -= 3 + d[2];
        d += 3;

        if (flags & 0x0080) // PES_PTS
        {
            pts = parse_pts(d,flags);
            d += 5;
        }
        if (flags & 0x0040) // PES_DTS
            dts = parse_pts(d,flags);
    }
    if (pid == 0x100) {
        _data = payload;
        _end = end;
        /*
        for (int i = 0; i < (int)(end - payload); i++)
            printf("%02X",payload[i]);
        printf("\n");
         */
        if (payload_unit_start)
            PLOG(VIDEO_PES);
        if (pts != -1)
            _pts = pts;
        return *_data++;
    }
    if (pid == 0x101 || pid == 0x102) {
        if (payload_unit_start) {
            if (_audio_pts == -1)
                printf("restarting audio\n");
            _audio_expected = expected;
            _audio_mark = 0;
            _audio_pts = pts;
            PLOG(AUDIO_PES);
        }
        if (_audio_pts != -1) {
            _audio_mark += end-payload;
            push_audio(payload,(int)(end-payload),pts,_audio_mark == _audio_expected);     // got more compressed audio
        }
    }
    return -1;
}

// reset bitstream state
void MpegDecoder::reset()
{
    printf("Resetting mpeg\n");
    if (_buffer) {
        _empty_q.push(_buffer);
        _buffer = 0;
    }
    _b_count = _b = 0;
    _data = _end = 0;
    while (!_full_q.empty())
        _empty_q.push(_full_q.pop());   // release buffers
    video_reset();                      // reset timing
    _last_pts = -1;
    _audio_pts = -1;
}

// pad with eos
const uint8_t _eos[] = { 0x00, 0x00, 0x01, 0xB7, 0x00, 0x00, 0x01, 0xB7 };

// Demux transport stream to find new data to decode
uint8_t MpegDecoder::more()
{
    for (;;) {
        if (_buffer && (_mark == _buffer->len)) {
            _empty_q.push(_buffer);
            _buffer = 0;
        }
        if (!_buffer) {
            _buffer = (Buffer*)_full_q.pop();
            _mark = 0;
            if (_buffer->len <= 0) {
                _data = _eos;
                _end = _data + sizeof(_eos);
                return 0;   // No more buffers comming
            }
        }
        uint8_t* d = _buffer->data + _mark;
        _mark += 188;
        if (*d != 0x47) {
            printf("ts lost sync\n");
            return 0;
        }

        // consume the next transport packet
        int pid = ((d[1] << 8) + d[2]) & 0x1fff;
        const uint8_t* data = d + 4;
        if (d[3] & 0x20)            // adaptation field
            data = d + 5 + d[4];
        if (d[3] & 0x10) {          // data
            int b = demux(pid,data,d+188,d[1] & 0x40);
            if (b != -1)
                return b;           // another blob of video ready
        }
    }
}

inline
int MpegDecoder::get_bit()
{
    FILL_BITS();
    return (_b >> --_b_count) & 1;
}

__attribute__((always_inline))
int MpegDecoder::peek_bits(int n)
{
    FILL_BITS();
    return (_b >> (_b_count - n)) & ((1 << n)-1);
}

inline
int MpegDecoder::get_bits(int n)
{
    FILL_BITS();
    return (_b >> (_b_count -= n)) & ((1 << n)-1);
}

inline
int MpegDecoder::get_vlc(const uint32_t* vlc)
{
    FILL_BITS();

    uint8_t state = 0;
    int b = _b;
    int b_count = _b_count; // copy to local regs
    do {
        int bit = (b >> --b_count) & 1;
        state = vlc[state] >> (bit ? 16 : 24);
    } while (vlc[state] >> 24);
    _b_count = b_count;
    return (int16_t)vlc[state];
}

#define C(_run,_len) (((_run) << 8) | (_len))

// iso13818-2 table B-14 in fast fussy form
const int16_t t_001[4] =      { 0,C(0,3),C(4,1),C(3,1) };
const int16_t t_00100[8] =    { C(13,1),C(0,6),C(12,1),C(11,1),C(3,2),C(1,3),C(0,5),C(10,1) };
const int16_t t_0001[4] =     { C(7,1),C(6,1),C(1,2),C(5,1) };
const int16_t t_00001[4] =    { C(2,2),C(9,1),C(0,4),C(8,1) };
const int16_t t_0000001[8] =  { C(16,1), C(5,2), C(0,7), C(2,3),  C(1,4), C(15,1), C(14,1), C(4,2) };
const int16_t t_0000X[5*16] = {
    C(0,11),C(8,2), C(4,3), C(0,10),C(2,4), C(7,2), C(21,1),C(20,1),C(0,9), C(19,1),C(18,1),C(1,5), C(3,3), C(0,8), C(6,2), C(17,1),
    C(10,2),C(9,2), C(5,3), C(3,4), C(2,5), C(1,7), C(1,6), C(0,15),C(0,14),C(0,13),C(0,12),C(26,1),C(25,1),C(24,1),C(23,1),C(22,1),
    C(0,31),C(0,30),C(0,29),C(0,28),C(0,27),C(0,26),C(0,25),C(0,24),C(0,23),C(0,22),C(0,21),C(0,20),C(0,19),C(0,18),C(0,17),C(0,16),
    C(0,40),C(0,39),C(0,38),C(0,37),C(0,36),C(0,35),C(0,34),C(0,33),C(0,32),C(1,14),C(1,13),C(1,12),C(1,11),C(1,10),C(1,9), C(1,8),
    C(1,18),C(1,17),C(1,16),C(1,15),C(6,3), C(16,2),C(15,2),C(14,2),C(13,2),C(12,2),C(11,2),C(31,1),C(30,1),C(29,1),C(28,1),C(27,1),
};

__attribute__((always_inline))
int MpegDecoder::get_vlc_dct()
{
    FILL_BITS();        // 12
    int16_t pb = (_b >> (_b_count - 16)) & ((1 << 16)-1); // peek 16 bits
    if (pb < 0)
    {
        --_b_count;
        return 1;
    }
    if ((pb >> 10) > 1) {
        if (pb & 0x4000) {
            if (pb & 0x2000) {
                _b_count -= 3;
                return (1 << 8) | 1; // 011 : 3 1,1
            }
            _b_count -= 4;
            return (pb & 0x1000) ? ((2 << 8) | 1) : 2;    // 0100 : 4 0,2   0101 : 4 2,1
        }
        if (pb & 0x2000) {
            int b5 = (pb >> 11) & 3;
            if (b5) {
                /*
                 00101 : 5 0,3
                 00110 : 5 4,1
                 00111 : 5 3,1
                 */
                _b_count -= 5;
                return t_001[b5];
            }
            /*
            00100000 : 8 13,1
            00100001 : 8 0,6
            00100010 : 8 12,1
            00100011 : 8 11,1
            00100100 : 8 3,2
            00100101 : 8 1,3
            00100110 : 8 0,5
            00100111 : 8 10,1
            */
            _b_count -= 8;
            return t_00100[(pb >> 8) & 7];
        } else {
            if (pb & 0x1000) {
                /*
                000100 : 6 7,1
                000101 : 6 6,1
                000110 : 6 1,2
                000111 : 6 5,1
                */
                _b_count -= 6;
                return t_0001[(pb >> 10) & 3];
            }
            /*
            0000100 : 7 2,2
            0000101 : 7 9,1
            0000110 : 7 0,4
            0000111 : 7 8,1
            */
            _b_count -= 7;
            return t_00001[(pb >> 9) & 3];
        }
    }
    else if ((pb >> 10) == 1)
    {
        _b_count -= 12;
        return ((pb>>4) & 0x3F) << 8;  // escape has 6 bits of run, 0 level
    }

    if (pb & 0x0200) {
        /*
         0000001000 : 10 16,1
         0000001001 : 10 5,2
         0000001010 : 10 0,7
         0000001011 : 10 2,3
         0000001100 : 10 1,4
         0000001101 : 10 15,1
         0000001110 : 10 14,1
         0000001111 : 10 4,2
         */
        _b_count -= 10;
        return t_0000001[(pb >> 6) & 7];
    }

    // 12 to 16 bit codes
    if (pb == 0)
        printf("this is not a code that should exist");

    int z = 0;
    while (pb < 0x0100)
    {
        pb <<= 1;
        z++;
    }
    _b_count -= 12 + z;
    return t_0000X[(z << 4) + ((pb >> 4) & 0xF)];
}

const uint8_t* MpegDecoder::read_matrix(uint8_t* dst)
{
    for (int i = 0; i < 64; i++)
        dst[i] = get_bits(8);
    return dst;
}

int64_t  MpegDecoder::get_pts()
{
    return _last_pts;   // actually last presented frame
}

void MpegDecoder::sequence()
{
    horizontal_size = get_bits(12);
    vertical_size = get_bits(12);
    pel_aspect_ratio = get_bits(4);
    picture_rate  = get_bits(4);
    bit_rate = get_bits(18);
    get_bits(12);
    if (get_bit())
        read_matrix(intra_q);
    else
        memcpy(intra_q,default_intra_q,64);
    if (get_bit())
        read_matrix(non_intra_q);
    else
        memset(non_intra_q,16,64);

    mb_width = (horizontal_size+15) >> 4;
    mb_height = (vertical_size+15) >> 4;
    mb_size = mb_width*mb_height;
}

void MpegDecoder::gop()
{
    int t = get_bits(25);
    drop_frame = ((t & 0x1000000) != 0) && (picture_rate != 4);
    hours = (t >> 19) & 0x1F;
    minutes = (t >> 13) & 0x3F;
    seconds = (t >>  6) & 0x3F;
    pictures =  t & 0x3F;
    get_bits(7);
   // printf("%02d:%02d:%02d:%02d\n",hours,minutes,seconds,pictures);
}

void MpegDecoder::flush_picture(int mode)
{
    if (_last_pts != -1 || mode) {
        push_video(_fb[0],_fb_index & 1,_last_pts,mode);  // this is the last picture
        _reference = _fb[_fb_index++ & 1];
        _current = _fb[_fb_index & 1];
    }
    if (!mode)
        _last_pts = _pts;
    REPORT();
}

void MpegDecoder::picture()
{
    flush_picture();

    int temporal_reference = get_bits(10);
    picture_coding_type = get_bits(3);
    switch (picture_coding_type) {
        case I_FRAME:
        case P_FRAME:
            //printf("%s:%d\n",picture_coding_type == I_FRAME ? "I_FRAME":"P_FRAME",temporal_reference);
            break;
        default:
            return; // Ignore B or D frames
    }
    get_bits(16);   // vbv_delay

    if (picture_coding_type == P_FRAME) {
        full_pel_forward = get_bit();
        forward_r_size = get_bits(3)-1;
    }
}

void MpegDecoder::reset_predictors()
{
    y_dc = cr_dc = cb_dc = 128; // reset DC prediction
    forward_motion_h = forward_motion_v = 0;    // reset motion vectors
}

uint32_t _src_align[5*17];
void MpegDecoder::mocomp(uint8_t* dst, int pos_x, int pos_y, int size, int c)
{
    int xy = ((pos_y & 1) << 1) | (pos_x & 1);
    pos_y >>= 1;
    pos_x >>= 1;

    // copy reference block into a 8 bit addressable buffer
    uint32_t* d32 = _src_align;
    for (int y = 0; y < (size + (xy >> 1)); y++) {
        const uint8_t* src_line;
        switch (c) {
            case 1: src_line = _reference->get_cr(pos_y); break;
            case 2: src_line = _reference->get_cb(pos_y); break;
            default: src_line = _reference->get_y(pos_y);
        }
        src_line += pos_x & ~3;
        const uint32_t *s32  = (const uint32_t*)(src_line);
        d32[0] = s32[0];
        d32[1] = s32[1];
        d32[2] = s32[2];
        if (size == 16) {
            d32[3] = s32[3];
            d32[4] = s32[4];
        }
        pos_y++;
        d32 += 5;
    }

    // src is copied into 8 bit mem. do the mocomp
    const uint8_t* src = ((uint8_t*)_src_align) + (pos_x & 3);
    const uint8_t* src2;
    int dst_stride = FB_STRIDE >> 2;
    d32 = (uint32_t*)(dst + size*mb_x);

    switch (xy)
    {
        case 0:
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size/4; x++)
                    d32[x] = ((uint32_t*)src)[x];
                src += 5*4;
                d32 += dst_stride;
            }
            break;
        case 1:
            for (int y = 0; y < size; y++) {
                int b = src[0];
                int a;
                for (int x = 0; x < size; x += 4) {
                    uint32_t p = (b + (a=src[x+1]) + 1) >> 1;
                    p |= ((a + (b=src[x+2]) + 1) >> 1) << 8;
                    p |= ((b + (a=src[x+3]) + 1) >> 1) << 16;
                    p |= ((a + (b=src[x+4]) + 1) >> 1) << 24;
                    d32[x>>2] = p;
                }
                src += 5*4;
                d32 += dst_stride;
            }
            break;
        case 2:
            for (int y = 0; y < size; y++) {
                src2 = src + 5*4;
                for (int x = 0; x < size; x += 4) {
                    d32[x>>2] =
                    (((src[x+0] + src2[x+0] + 1) >> 1) << 0) |
                    (((src[x+1] + src2[x+1] + 1) >> 1) << 8) |
                    (((src[x+2] + src2[x+2] + 1) >> 1) << 16) |
                    (((src[x+3] + src2[x+3] + 1) >> 1) << 24);
                }
                src += 5*4;
                d32 += dst_stride;
            }
            break;
        case 3:
            for (int y = 0; y < size; y++) {
                src2 = src + 5*4;
                for (int x = 0; x < size; x += 4) { // see case 1 above
                    d32[x>>2] =
                    (((src[x+0] + src[x+1] + src2[x+0] + src2[x+1] + 2) >> 2) << 0) |
                    (((src[x+1] + src[x+2] + src2[x+1] + src2[x+2] + 2) >> 2) << 8) |
                    (((src[x+2] + src[x+3] + src2[x+2] + src2[x+3] + 2) >> 2) << 16) |
                    (((src[x+3] + src[x+4] + src2[x+3] + src2[x+4] + 2) >> 2) << 24);
                }
                src += 5*4;
                d32 += dst_stride;
            }
            break;
    }
}

void MpegDecoder::inc_mb(int n )
{
    mb_x += 1;
    while (mb_x >= mb_width) {
        mb_x -= mb_width;
        mb_y++;
        y_addr = _current->get_y(mb_y << 4);
        cr_addr = y_addr + FB_WIDTH;
        cb_addr = cr_addr + FB_STRIDE*8;
    }
}

inline
void MpegDecoder::blit(uint8_t* dst, uint8_t* src, int size)
{
    int x = mb_x*size;
    uint32_t* s32 = (uint32_t*)(src+x);
    uint32_t* d32 = (uint32_t*)(dst+x);
    int stride = FB_STRIDE >> 2;
    if (size == 8) {
        for (int i = 0; i < size; i++) {
            d32[0] = s32[0];
            d32[1] = s32[1];
            d32 += stride;
            s32 += stride;
        }
    } else {
        for (int i = 0; i < size; i++) {
            d32[0] = s32[0];
            d32[1] = s32[1];
            d32[2] = s32[2];
            d32[3] = s32[3];
            d32 += stride;
            s32 += stride;
        }
    }
}

void MpegDecoder::predict_zero()
{
    uint8_t* ref = _reference->get_y(mb_y << 4);
    blit(y_addr,ref);
    blit(cr_addr,ref + FB_WIDTH,8);
    blit(cb_addr,ref + FB_WIDTH + FB_STRIDE*8,8);
}


void MpegDecoder::predict()
{
    int h = forward_motion_h;
    int v = forward_motion_v;
    if (h == 0 && v == 0) {
        predict_zero();
        return;
    }
    if (full_pel_forward) {
        h <<= 1;
        v <<= 1;
    }
    int x = (mb_x << 5) + h;
    int y = (mb_y << 5) + v;
    mocomp(y_addr,x,y,16);
    x >>= 1;
    y >>= 1;
    mocomp(cr_addr,x,y,8,1);
    mocomp(cb_addr,x,y,8,2);
}

int MpegDecoder::motion_vector(int m, int r_size)
{
    int d;
    int scale = 1 << r_size;
    int code = get_vlc(motion_vec);
    if ((code != 0) && (scale != 1))
    {
        d = ((abs(code) - 1) << r_size) + get_bits(r_size) + 1;
        if (code < 0)
            d = -d;
    } else
        d = code;

    m += d;
    if (m > (scale << 4) - 1)
        m -= scale << 5;
    else if (m < ((-scale) << 4))
        m += scale << 5;
    return m;
}

void MpegDecoder::motion_vectors(bool fw)
{
    if (!fw) {
        forward_motion_h = forward_motion_v = 0;    // reset motion vectors
        return;
    }
    forward_motion_h = motion_vector(forward_motion_h,forward_r_size);
    forward_motion_v = motion_vector(forward_motion_v,forward_r_size);
}

void MpegDecoder::idct(int* b, int n)
{
   // MEASURE(_idct_ticks);
    if (n == 1) {   // DC only block
        int dc = b[0] >> 8;
        for (int i = 0; i < 64; i++)
            b[i] = dc;
        return;
    }

    // See http://vsr.informatik.tu-chemnitz.de/~jan/MPEG/HTML/IDCT.html
    int b1, b3, b4, b6, b7, tmp1, tmp2, m0;
    int x0, x1, x2, x3, x4, y3, y4, y5, y6, y7;
    int i;

    // Transform columns
    for (i = 0; i < 8; ++i) {
        b1 =  b[4*8+i];
        b3 =  b[2*8+i] + b[6*8+i];
        b4 =  b[5*8+i] - b[3*8+i];
        tmp1 = b[1*8+i] + b[7*8+i];
        tmp2 = b[3*8+i] + b[5*8+i];
        b6 = b[1*8+i] - b[7*8+i];
        b7 = tmp1 + tmp2;
        m0 =  b[0*8+i];
        x4 =  ((b6*473 - b4*196 + 128) >> 8) - b7;
        x0 =  x4 - (((tmp1 - tmp2)*362 + 128) >> 8);
        x1 =  m0 - b1;
        x2 =  (((b[2*8+i] - b[6*8+i])*362 + 128) >> 8) - b3;
        x3 =  m0 + b1;
        y3 =  x1 + x2;
        y4 =  x3 + b3;
        y5 =  x1 - x2;
        y6 =  x3 - b3;
        y7 = -x0 - ((b4*473 + b6*196 + 128) >> 8);
        b[0*8+i] =  b7 + y4;
        b[1*8+i] =  x4 + y3;
        b[2*8+i] =  y5 - x0;
        b[3*8+i] =  y6 - y7;
        b[4*8+i] =  y6 + y7;
        b[5*8+i] =  x0 + y5;
        b[6*8+i] =  y3 - x4;
        b[7*8+i] =  y4 - b7;
    }

    // Transform rows
    for (i = 0; i < 64; i += 8) {
        b1 =  b[4+i];
        b3 =  b[2+i] + b[6+i];
        b4 =  b[5+i] - b[3+i];
        tmp1 = b[1+i] + b[7+i];
        tmp2 = b[3+i] + b[5+i];
        b6 = b[1+i] - b[7+i];
        b7 = tmp1 + tmp2;
        m0 =  b[0+i];
        x4 =  ((b6*473 - b4*196 + 128) >> 8) - b7;
        x0 =  x4 - (((tmp1 - tmp2)*362 + 128) >> 8);
        x1 =  m0 - b1;
        x2 =  (((b[2+i] - b[6+i])*362 + 128) >> 8) - b3;
        x3 =  m0 + b1;
        y3 =  x1 + x2;
        y4 =  x3 + b3;
        y5 =  x1 - x2;
        y6 =  x3 - b3;
        y7 = -x0 - ((b4*473 + b6*196 + 128) >> 8);
        b[0+i] =  (b7 + y4 + 128) >> 8;
        b[1+i] =  (x4 + y3 + 128) >> 8;
        b[2+i] =  (y5 - x0 + 128) >> 8;
        b[3+i] =  (y6 - y7 + 128) >> 8;
        b[4+i] =  (y6 + y7 + 128) >> 8;
        b[5+i] =  (x0 + y5 + 128) >> 8;
        b[6+i] =  (y3 - x4 + 128) >> 8;
        b[7+i] =  (y4 - b7 + 128) >> 8;
    }
}

// 8x8
int MpegDecoder::block(int block, bool intra)
{
    //MEASURE(_block_ticks);

    const uint8_t* q = non_intra_q;
    int n = 0;

    int b[64];
    for (int i = 0; i < 64; i += 4)
        b[i] = b[i+1] = b[i+2] = b[i+3] = 0;

    if (intra)  // get DC
    {
        int pb = peek_bits(10);
        int dc_size;
        if (block < 4) {
            b[0] = y_dc;

            // Table B-12 --- Variable length codes for dct_dc_size_luminance
            pb >>= 1;
            if (!(pb & 0x100)) {
                dc_size = 1 + (pb >> 7);
                _b_count -= 2;
            } else if (!(pb & 0x80)) {
                dc_size = pb & 0x40 ? 3 : 0;
                _b_count -= 3;
            } else {
                dc_size = 4;
                pb <<= 2;
                while (pb & 0x100) {
                    pb <<= 1;
                    dc_size++;
                }
                _b_count -= dc_size-1;
            }
        }
        else {
            // Table B-13 --- Variable length codes for dct_dc_size_chrominance
            b[0] = (block == 4 ? cr_dc : cb_dc);
            if (!(pb & 0x200)) {
                dc_size = pb >> 8;
                _b_count -= 2;
            } else {
                dc_size = 1;
                do {
                    pb <<= 1;
                    dc_size++;
                } while (pb & 0x200);
                _b_count -= min(dc_size,10);
            }
        }

        if (dc_size)
        {
            int delta = get_bits(dc_size);
            if (delta & (1 << (dc_size - 1)))
                b[0] += delta;
            else
                b[0] += ((-1 << dc_size)|(delta+1));

            switch (block) {
                case 4: cr_dc = b[0]; break;
                case 5: cb_dc = b[0]; break;
                default: y_dc = b[0]; break;    // update DC prediction
            }
        }
        b[0] <<= 8; // scale
        q = intra_q;
        n = 1;
    }

    for (;;) {       // get AC
        int run,v,zz;

        int p = peek_bits(2);       // fill >= 24
        if (n) {                    // EOB can't be on first
            if (p == 0x2) {
                _b_count -= 2;      // End of block
                break;
            }
        }

        int run_value = p >> 1;
        if (run_value)
            --_b_count;
        else
            run_value = get_vlc_dct();
        if ((run_value == 1) && n)
            --_b_count; // get_bit

        run = run_value >> 8;
        v = (int8_t)run_value;

        if (v == 0) {           // escape
            v = get_bits(8);
            if (v == 0)
                v = get_bits(8);
            else if (v == 128)
                v = get_bits(8) - 256;
            else if (v > 128)
                v = v - 256;
        } else {
            if ((_b >> --_b_count) & 1) // get_bit
                v = -v;
        }

        n += run;
        if (n >= 64)
            return -1;
        zz = zig_zag[n++];

        v <<= 1;
        if (!intra)
            v += (v < 0 ? -1 : 1);
        v = (v*quantizer_scale*q[zz]) / 16;
        if ((v & 1) == 0)
            v -= v > 0 ? 1 : -1;
        if (v > 2047)
            v = 2047;
        else if (v < -2048)
            v = -2048;

        b[zz] = v*scale_dct_q[zz];
    }

    uint8_t* dst = y_addr + (mb_x << 4);
    switch (block) {
        case 1: dst += 8; break;
        case 2: dst += FB_STRIDE*8; break;
        case 3: dst += FB_STRIDE*8 + 8; break;
        case 4: dst = cr_addr + (mb_x << 3); break;
        case 5: dst = cb_addr + (mb_x << 3); break;
    }

    if (n == 1) {
        int dc = b[0] >> 8;
        if (intra)
            copy_block_dc(dst,dc);
        else
            add_block_dc(dst,dc);
        return 0;
    }

    idct(b,n);
    if (intra)
        copy_block(dst,b);
    else
        add_block(dst,b);
    return 0;
}

// copy block to destination
void MpegDecoder::copy_block(uint8_t* dst, int* b)
{
    int i = 8;
    int stride = FB_STRIDE;
    while (i--) {
        uint32_t d;
        uint32_t* d32 = (uint32_t*)dst;

        d = PIN(b[0]);
        d |= PIN(b[1]) << 8;
        d |= PIN(b[2]) << 16;
        d |= PIN(b[3]) << 24;
        *d32++ = d;
        d = PIN(b[4]);
        d |= PIN(b[5]) << 8;
        d |= PIN(b[6]) << 16;
        d |= PIN(b[7]) << 24;
        *d32 = d;

        dst += stride;
        b += 8;
    }
}

void MpegDecoder::copy_block_dc(uint8_t* dst, int dc)
{
    int i = 8;
    int stride = FB_STRIDE;
    dc |= dc << 8;
    dc |= dc << 16;
    uint32_t* d32 = (uint32_t*)dst;
    while (i--) {
        d32[0] = dc;
        d32[1] = dc;
        d32 += stride >> 2;
    }
}

void MpegDecoder::add_block(uint8_t* dst, int* b)
{
    int i = 8;
    int stride = FB_STRIDE;
    while (i--) {
        uint32_t d;
        uint32_t* d32 = (uint32_t*)dst;
        uint32_t s = *d32;

        d = PIN(b[0] + (uint8_t)(s));
        d |= PIN(b[1] + (uint8_t)(s >> 8)) << 8;
        d |= PIN(b[2] + (uint8_t)(s >> 16)) << 16;
        d |= PIN(b[3] + (uint8_t)(s >> 24)) << 24;
        *d32++ = d;
        s = *d32;
        d = PIN(b[4] + (uint8_t)(s));
        d |= PIN(b[5] + (uint8_t)(s >> 8)) << 8;
        d |= PIN(b[6] + (uint8_t)(s >> 16)) << 16;
        d |= PIN(b[7] + (uint8_t)(s >> 24)) << 24;
        *d32 = d;
        dst += stride;
        b += 8;
    }
}

void MpegDecoder::add_block_dc(uint8_t* dst, int dc)
{
    int i = 8;
    int stride = FB_STRIDE;
    while (i--) {
        uint32_t d;
        uint32_t* d32 = (uint32_t*)dst;
        uint32_t s = *d32;

        d = PIN(dc + (uint8_t)(s));
        d |= PIN(dc + (uint8_t)(s >> 8)) << 8;
        d |= PIN(dc + (uint8_t)(s >> 16)) << 16;
        d |= PIN(dc + (uint8_t)(s >> 24)) << 24;
        *d32++ = d;
        s = *d32;
        d = PIN(dc + (uint8_t)(s));
        d |= PIN(dc + (uint8_t)(s >> 8)) << 8;
        d |= PIN(dc + (uint8_t)(s >> 16)) << 16;
        d |= PIN(dc + (uint8_t)(s >> 24)) << 24;
        *d32 = d;
        dst += stride;
    }
}

bool MpegDecoder::slice_done()
{
    int m = peek_bits(23);
    if (!m)
        return true;        // marker
    int n = _b_count;
    while (n--) {           // Any non-zero bits left?
        if ((_b >> n) & 1)
            return false;
    }
    return true;            // we should never get here - bad stream if we do
}

int MpegDecoder::slice(int s)
{
    MEASURE(_picture_ticks);

    mb_y = s-2;
    mb_x = mb_width-1;  // will correct on first increment
    if (mb_y >= mb_height)
        return -1;

    reset_predictors();
    quantizer_scale = get_bits(5);
    while (get_bit())
        get_bits(8);

    // ready for macroblocks
    for (int mb = 0; !slice_done(); mb++) {
        int increment = 0;
        int i = get_vlc(macroblock_address_increment);
        while (i == 34)     // mb stuffing
            i = get_vlc(macroblock_address_increment);
        while (i == 35) {   // mb escape
            increment += 33;
            i = get_vlc(macroblock_address_increment);
        }
        increment += i;

        if (mb == 0) {
            inc_mb(increment);
        } else {
            if (increment > 1)
                reset_predictors();

            while (increment > 1)
            {
                inc_mb();
                predict_zero();  // copy skipped macroblocks
                increment--;
            }
            inc_mb();
        }

        int mb_type = get_vlc(picture_coding_type == I_FRAME ? macroblock_type_I : macroblock_type_P);
        int intra = mb_type & 0x01;

        if (mb_type & 0x10)
            quantizer_scale = get_bits(5);

        if (intra) // Intra
        {
            forward_motion_h = forward_motion_v = 0;    // reset motion vectors
        } else {
            y_dc = cr_dc = cb_dc = 128;                 // reset DC prediction
            motion_vectors(mb_type & 0x08);
            predict();
        }

        int cbp = mb_type & 0x02 ? get_vlc(coded_block_pattern) : intra ? 63:0;  // coded block pattern
        int mask = 0x20;
        for (int i = 0; i < 6; i++) {
            if (cbp & mask)
                block(i,intra);
            mask >>= 1;
        }
    }
    return 0;
}

int MpegDecoder::marker(int m)
{
    switch (m) {
        case SEQUENCE_START:    sequence(); break;
        case GROUP:             gop();      break;
        case PICTURE:           picture();  break;
        case SEQUENCE_END:
            printf("sequence end\n");
            pause();
            break;
        case USER_DATA:
        case EXTENSION:
            break;
        default:
            if (m >= SLICE_FIRST && m <= SLICE_LAST)
                slice(m);
            else {
                printf("bad marker? %02X\n",m);
                return -1;
            }
    }
    return 0;
}

void MpegDecoder::pause()
{
    printf("MpegDecoder pausing\n");
    clear_events(DECODER_RUN);
    set_events(DECODER_PAUSED);
    if (_empty_q.empty())
        _empty_q.push(0);   // unstick if someone is waiting in decode_next
    wait_events(DECODER_RUN);
    clear_events(DECODER_PAUSED);
    printf("MpegDecoder unpaused\n");
}

// buffers pulled by bitstream reads
void MpegDecoder::run()
{
    for (;;) {
        if (!(get_events() & DECODER_RUN))  // always pause/resume at payload unit start?
            pause();
        while (peek_bits(24) == 0)
            get_bit();
        get_bits(24); // == 1;
        int m = get_bits(8);
        //printf("%s\n",marker_name(m));
        marker(m);
    }
}
