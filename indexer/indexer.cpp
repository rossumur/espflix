//
//  indexer.cpp
//
//  Created by Peter Barrett on 6/29/20.
//  Copyright © 2020 Peter Barrett. All rights reserved.
//

#include <stdio.h>
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include "unistd.h"
#include <sys/stat.h>

using namespace std;

//====================================================================================
//====================================================================================
// Generate index tables for mpts

typedef struct {
    int64_t first_pts;
    int64_t last_pts;
    uint32_t bin_size;
    uint32_t trick_speed;
    uint32_t sample_count;
} idx_rec;

typedef struct {
    uint32_t sig;
    uint32_t len;   // 3
    idx_rec video;
    idx_rec fwd;
    idx_rec rev;
} idx_hdr;

static uint16_t be16(const uint8_t* d)
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

static int parse(const uint8_t* d, const uint8_t* end, int64_t& pts, int64_t& dts)
{
    int expected = be16(d+4);  // always zero for video
    const uint8_t* payload;
    pts = dts = 0;
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

    return payload[3];  // marker
}

// 1/4 second mapping of pts to sequence offset
typedef struct {
    int64_t pts;
    uint32_t pos188;
} seq;

typedef struct {
    vector<seq> seqs;
    int64_t first_pts;
    int64_t last_pts;
    vector<uint32_t> samples;
} idx;

// find all the pts points in the video
void make_index(const string& src, vector<idx>& idxs)
{
    uint8_t buf[1024*188];
    FILE* f = fopen(src.c_str(),"rb");
    int packet = 0;
    int video_packet = 0;
    int frame_packets = 0;
    int gop_packets = 0;
    int max_frame_packets = 0;
    int64_t pts = 0,dts;
    int64_t origin = -1;
    int64_t audio_pts = -1;
    int64_t video_pts = -1;
    int64_t gop_pts = -1;
    int64_t audio_origin = -1;
    int64_t audio_delta_max = -900000;
    int64_t audio_delta_min = 900000;
    int video_kbits = 0;

    vector<seq> seqs;
    vector<seq> audio;

    printf(">%s index\n",src.c_str());
    // get all the sequence start data
    for (;;) {
        size_t n = fread(buf,1,sizeof(buf),f);
        if (!n)
            break;
        for (int i = 0; i < n; i += 188) {
            const uint8_t* d = buf + i;
            int pid = ((d[1] << 8) + d[2]) & 0x1fff;
            const uint8_t* data = d + 4;
            if (d[3] & 0x20)            // adaptation field
                data = d + 5 + d[4];
            if (d[3] & 0x10) {          // has data
                if (d[1] & 0x40) {      // payload unit start
                    int m = parse(data,d+188,pts,dts);
                    switch (pid) {
                        case 0x100:     // video
                            if (m == 0xB3) {    // start of sequence
                                if (origin == -1)
                                    origin = pts;
                                seqs.push_back({pts,(uint32_t)packet});
                                if (gop_pts == -1)
                                    gop_pts = pts;
                                else {
                                    int bitrate = gop_packets*188*8/((pts-gop_pts)/90);
                                    //printf("gop packets %d %dkb/s\n",gop_packets,bitrate);
                                    video_kbits = max(bitrate,video_kbits);
                                }
                                gop_pts = pts;
                                gop_packets = 0;
                            }
                            video_pts = pts;
                            video_packet = packet;
                            //printf("%d in frame\n",frame_packets);
                            max_frame_packets = max(frame_packets,max_frame_packets);
                            gop_packets += frame_packets;
                            frame_packets = 0;
                            break;
                        case 0x102:             // audio
                            if (audio_origin == -1)
                                audio_origin = pts;
                            audio.push_back({pts,(uint32_t)packet});
                            audio_pts = pts;
                            audio_delta_max = max(audio_delta_max,audio_pts-video_pts);
                            audio_delta_min = min(audio_delta_min,audio_pts-video_pts);
                            //printf("Audio offset %dms (%d packets)\n",(int)((audio_pts-video_pts)/90),packet-video_packet);
                            break;
                    }
                }
                if (pid == 0x100)
                    frame_packets++;
            }
            packet++;
        }
    }
    fclose(f);

    printf("Audio between %dms early and %dms late\n",(int)(audio_delta_max/90),(int)(audio_delta_min/90));
    printf("Max video frame packets was %d (%dk), max gop bitrate was %dkbit/s\n",max_frame_packets,max_frame_packets*188/1024,video_kbits);

    idxs.push_back({seqs,origin,video_pts});
}

// map a pts in fwd and reverse
// map from a random access point in the trick streams to the main stream
// by default pts delay is 129750/90000 or ~1.5 seconds in ffmpeg
static
uint32_t pts2pos(int64_t pts, vector<seq>& s)
{
    int mini = 0;
    int mine = 0x7FFFFFF;
    for (int i = 0; i < s.size(); i++) {
        int e = (int)abs(s[i].pts-pts);
        if (e < mine) {
            mine = e;
            mini = i;
        }
    }
    return s[mini].pos188;
}

idx_rec pts2seq(idx& id, int speedx, int bin_size)
{
    int64_t end = id.last_pts-id.first_pts;
    int64_t pts = 0;
    while (pts <= end) {
        id.samples.push_back(pts2pos(pts+id.first_pts,id.seqs));    // zero based
        pts += bin_size;
    }

    auto& d = id.samples;
    printf("%d->%d, produced %d from %d\n",(int)id.first_pts,(int)id.last_pts,(int)d.size(),(int)id.seqs.size());
    //for (int i = 0; i < d.size(); i++)
    //    printf("%d\n",d[i]);

    idx_rec r;
    r.first_pts = id.first_pts;
    r.last_pts = id.last_pts;
    r.sample_count = (uint32_t)id.samples.size();
    r.bin_size = bin_size;
    r.trick_speed = speedx;
    return r;
}

void merge_index(vector<idx>& all, const string& path)
{
    int speedx = 15;
    auto& video = all[0];
    auto& fwd = all[1];
    auto& rwd = all[2];

    idx_hdr hdr;
    hdr.sig = ('I' << 0) | ('D' << 8) | ('X' << 16);
    hdr.len = 3;
    hdr.video = pts2seq(video,1,90000/12);
    hdr.fwd = pts2seq(fwd,speedx,90000/12);
    hdr.rev = pts2seq(rwd,speedx,90000/12);

    string p = path + "/video.idx";
    FILE* f = fopen(p.c_str(),"wb");
    fwrite(&hdr,1,sizeof(hdr),f);
    fwrite(&video.samples[0],1,4*video.samples.size(),f);
    fwrite(&fwd.samples[0],1,4*fwd.samples.size(),f);
    fwrite(&rwd.samples[0],1,4*rwd.samples.size(),f);
    fclose(f);
}

string exec(const string& cmd)
{
    char buffer[2048];
    string result = "";
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (pipe) {
        while (fgets(buffer, sizeof(buffer)-1, pipe) != NULL) {
            printf(buffer);
            result += buffer;
        }
        int e = pclose(pipe);
        printf("pclose %d\n",e);
    }
    return result;
}

vector<string> get_mainfest(const string& root)
{
    fstream file;
    string path = string(root) + "/manifest.txt";
    file.open(path);
    string str;
    vector<string> m;
    while (std::getline(file, str))
        m.push_back(str);
    return m;
}

static
void make_index(const string& path)
{
    const char* files[3] = {"/video.ts","/video_fwd.ts","/video_rwd.ts"};
    vector<idx> all;
    for (int i = 0; i < 3; i++)
        make_index(path + files[i],all);
    merge_index(all,path);
}

static
void make_indexes(const string& root)
{
    auto m = get_mainfest(root);
    for (string str : m) {
        string path = string(root) + "media/" + str;
        make_index(path);
    }
}

static
bool remake(const string& path, bool force)
{
    if (access(path.c_str(),F_OK) == -1)
        return true;
    return force;
}

// extract audio
// ffmpeg -i video.ts -acodec copy -map i:258 audio.sbc
// ffmpeg -i audio.sbc audio.wav
// croping, resize
// ffmpeg -y -i video.mp4 -filter:v "crop=992:546:144:0" -bf 0 -b:v 2000k -s 352x192 out.mp4
// ffmpeg -y -i video.mp4 -vf cropdetect=24:16:0 dummy.mp4

void make_video(const string& src, const string& dst, bool clean = false)
{
    char buf[2048];
    const char* ffmpeg = "/usr/local/bin/ffmpeg";
    const char* poster =  "%s -y -i %s/poster.png -s 352x192 -f mpegts -codec:v mpeg1video -filter:v fps=fps=24 -bf 0 -q 2 %s/poster.ts";
    const char* video =  "%s -y -loglevel verbose -i %s/video.mp4 -s 352x192 -f mpegts -pat_period 30 -streamid 1:258 -pes_payload_size 512 -codec:v mpeg1video -qmin 3 -qmax 1000 -b:v 1500k -maxrate 1500k -bufsize 0.25M -bf 0 -codec:a sbc -ar 48000 -b:a 192k -ac 1 %s/video.ts";
    const char* fwd = "%s -y -i %s/video.ts -g 3 -s 352x192 -f mpegts -pat_period 30 -codec:v mpeg1video -qmin 3 -qmax 1000 -b:v 1500k -maxrate 1500k -bufsize 0.25M -bf 0 -an -filter:v \"setpts=PTS/15\" %s/video_fwd.ts";
    const char* rwd = "%s -y -i %s/video_fwd.ts -g 3 -f mpegts -pat_period 30 -codec:v mpeg1video -qmin 3 -qmax 1000 -b:v 1500k -maxrate 1500k -bufsize 0.25M -bf 0 -vf reverse %s/video_rwd.ts";

    exec("mkdir -p " + dst);

    if (remake(dst + "/poster.ts",clean)) {
        sprintf(buf,poster,ffmpeg,src.c_str(),dst.c_str());
        exec(buf);
    }
    if (remake(dst + "/video.ts",clean)) {
        sprintf(buf,video,ffmpeg,src.c_str(),dst.c_str());
        exec(buf);
    }
    if (remake(dst + "/video_fwd.ts",clean)) {
        sprintf(buf,fwd,ffmpeg,dst.c_str(),dst.c_str());
        exec(buf);
    }
    if (remake(dst + "/video_rwd.ts",clean)) {
        sprintf(buf,rwd,ffmpeg,dst.c_str(),dst.c_str());
        exec(buf);
    }
    make_index(dst);
}

void make_service(const string& assets, const string& service)
{
    auto m = get_mainfest(assets);
    exec("cp " + assets + "/manifest.txt " + service + "/manifest.txt");
    for (string str : m)
        make_video(assets + "/media/" + str,service + "/media/" + str);
}

