/*
 * video_container.h
 * Demuxing for the video player: finds where every video frame and audio
 * packet lives in the file, plus what codecs they need.
 *
 *   .avi        MJPEG or H.264 video; PCM (8/16-bit), MP3 or AAC audio
 *   .mp4/.mov   MJPEG or H.264 video; AAC, MP3 or PCM audio
 *   .mjpeg      raw MJPEG stream (back-to-back JPEGs, no audio). Indexed
 *               progressively while it plays; frame rate from a "15fps"
 *               style tag in the file name, else 24.
 *
 * H.264: only constrained-baseline streams decode (tinyH264); others are
 * refused here with a clear message rather than played as garbage.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef VIDEO_HOST_TEST
#include "host_file.h"
#else
#include <SD.h>
#endif

struct U32Vec {
    uint32_t *v = nullptr;
    int n = 0, cap = 0;
    bool push(uint32_t x);
    void clear();
};

enum VCodec : uint8_t { VC_NONE, VC_MJPEG, VC_H264 };
enum ACodec : uint8_t { AC_NONE, AC_PCM16LE, AC_PCM16BE, AC_PCM8, AC_MP3, AC_AAC };

struct VidInfo {
    bool ok = false;
    VCodec vcodec = VC_NONE;
    int w = 0, h = 0;               // display size
    uint32_t frame_us = 41667;      // nominal frame duration
    char fourcc[5] = {0};
    U32Vec voff, vsize;             // per frame
    U32Vec vkey;                    // keyframe indices, ascending (H.264); empty = all key
    // H.264
    uint8_t *avc_hdr = nullptr;     // SPS+PPS in Annex-B form (MP4 avcC)
    int avc_hdr_len = 0;
    int nal_len_size = 0;           // 1-4 = MP4 length-prefixed NALs, 0 = Annex-B already
    int h264_profile = 0;
    // raw MJPEG progressive index
    bool raw_mjpeg = false, raw_done = false;
    uint32_t raw_pos = 0, raw_size = 0, raw_cur = 0xFFFFFFFF;
    // audio
    bool has_audio = false;
    ACodec acodec = AC_NONE;
    int ach = 0;
    uint32_t arate = 0;
    uint32_t abytes_per_sec = 0;    // PCM: exact; used for partial-packet seeks
    uint8_t aac_aot = 2, aac_freq_idx = 4, aac_chan_cfg = 2;   // for ADTS headers
    U32Vec aoff, asize, ams;        // per packet: offset, size, start time (ms)
    uint32_t audio_ms = 0;          // soundtrack length

    void clear();
    uint32_t duration_ms() const { return (uint32_t)((uint64_t)voff.n * frame_us / 1000); }
    int keyframe_at_or_before(int idx) const;
};

// Parse `f`. poster_only = stop after the first usable video frame.
// On failure returns false with a user-facing reason in err.
bool video_parse(File &f, const char *name, VidInfo &vi, bool poster_only, char *err, size_t errlen);

// Raw MJPEG: index some more of the file (bounded work). Returns true when done.
bool video_index_more(File &f, VidInfo &vi, uint32_t max_bytes);

// Converts one MP4 sample (length-prefixed NALs) to Annex-B in `out`
// (start codes). Returns bytes written, 0 on malformed input.
size_t avcc_to_annexb(const uint8_t *in, size_t len, int nal_len_size, uint8_t *out, size_t out_cap);

// 7-byte ADTS header for one raw AAC frame of `payload` bytes.
void adts_header(const VidInfo &vi, size_t payload, uint8_t out[7]);
