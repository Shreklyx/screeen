// Lossless sound: FLAC – our own encoder and decoder (no library). The sharer's sound bit for bit (24-bit),
// not a lossy codec's approximation. Each packet is one standard FLAC frame (RFC 9639): a few milliseconds of one
// or two channels at 48 kHz, 24-bit; per channel the shortest of FLAC's fixed predictors (order 0–4), its
// residual Rice-coded in partitions; stereo as mid/side, left/side or right/side when that is shorter;
// silence a constant. Frames stand alone (a lost one takes nothing else with it) and any FLAC decoder
// plays them – the browsers' too (WebCodecs "flac"). Plain C++: the apps and the website's WebAssembly
// (native/web) build the same code.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ss {
namespace lossless {

const int kMaxChannels = 8;
const int kMaxFrames = 512; // (a packet is a few milliseconds: 120–480 frames)
const int kRate = 48000;
const int kBits = 24;

/** The most a frame of this size can take (a buffer this big is always enough). */
inline size_t maxPacket(int channels, int frames) { return 32 + size_t(channels) * (size_t(frames) * 4 + 16); }

/** Encodes `frames` frames of `channels` interleaved floats (−1 … 1; 24-bit) as one FLAC frame into out:
 *  the bytes written, 0 when it does not fit (or the size is wrong). `number` is the frame's number. */
size_t encode(const float* pcm, int channels, int frames, uint8_t* out, size_t cap, uint32_t number = 0);

/** Encodes 24-bit integers (interleaved) – what encode does after rounding; the tests. */
size_t encodeInt(const int32_t* pcm, int channels, int frames, uint8_t* out, size_t cap, uint32_t number = 0);

/** A frame's channels and frames (false: not a FLAC frame of 48 kHz 24-bit sound). */
bool info(const uint8_t* data, size_t n, int* channels, int* frames);

/** Decodes a FLAC frame into interleaved floats (room for maxFrames × its channels): its frames, 0 on a
 *  bad frame (its checksums are checked). */
int decode(const uint8_t* data, size_t n, float* out, int maxFrames, int* channels);

/** Decodes into 24-bit integers – the tests (bit for bit). */
int decodeInt(const uint8_t* data, size_t n, int32_t* out, int maxFrames, int* channels);

/** The FLAC stream header a decoder may need ("fLaC" + STREAMINFO, 42 bytes): 48 kHz, 24-bit, these
 *  channels and frames a packet. */
size_t streamHeader(int channels, int frames, uint8_t out[42]);

}  // namespace lossless
}  // namespace ss
