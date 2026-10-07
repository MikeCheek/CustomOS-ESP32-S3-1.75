/*
 * app_video.h
 * Video player for the SD card.
 *
 *   .avi        MJPEG or H.264 video + PCM / MP3 / AAC sound
 *   .mp4 .mov   MJPEG or H.264 video + AAC / MP3 / PCM sound
 *   .mjpeg      raw MJPEG stream (no sound; "_15fps" in the name sets the rate)
 *
 * H.264 is decoded in software (Espressif's tinyH264, library
 * ESP_H264_Decoder) and must be *constrained baseline*, up to 640x480 -
 * expect ~25 fps at 320x180, ~10 fps at 640x480. Phone/camera MP4s are
 * High profile and are refused with a message; re-encode them:
 *
 *   ffmpeg -i in.mp4 -vf "scale=320:-2,fps=20" -c:v libx264 -profile:v baseline \
 *          -pix_fmt yuv420p -g 20 -c:a aac -b:a 96k -ac 1 out.mp4
 *
 * Best quality per CPU (no decode limit on size, any frame rate):
 *   ffmpeg -i in.mp4 -vf "scale=466:-2,fps=24" -c:v mjpeg -q:v 6 \
 *          -c:a pcm_s16le -ar 22050 -ac 1 out.avi
 *
 * AAC/HE-AAC needs the "libhelix" library (Arduino Library Manager).
 *
 * Sync: a background task plays the soundtrack and counts the samples it
 * has played; that count is the clock. The UI task shows whichever frame
 * matches it - MJPEG skips frames when behind; H.264 has to decode every
 * frame, so when it falls behind it jumps ahead to the next keyframe.
 */
#pragma once
#include "ui.h"
#include "media_decode.h"

extern Screen video_player_screen;

// Opens the player on `path`.
void video_open(const char *path);

// First frame of a video as an image (Gallery thumbnails/posters).
bool video_poster(const char *path, int max_w, int max_h, bool cover, DecodedImage *out);
