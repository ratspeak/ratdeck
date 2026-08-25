#pragma once

// Codec2 voice memo helpers (mode 1300 @ 8 kHz mono).
// Local round-trip first; mesh LXMF attach later.
// Encoded file is small (~162 B/s) — suitable for LoRa.

#include <Arduino.h>
#include <stdint.h>

namespace Codec2Voice {

// Default paths under /Files chroot.
static constexpr const char* kProbeC2Path = "/Files/voice/probe.c2";
static constexpr const char* kProbeC2WavPath = "/Files/voice/probe_c2.wav";
static constexpr int kMode = 4;  // CODEC2_MODE_1300
static constexpr int kCodecRate = 8000;

struct Result {
  bool ok = false;
  uint32_t pcm_samples_8k = 0;
  uint32_t frames = 0;
  uint32_t c2_bytes = 0;   // payload only (no header)
  uint32_t file_bytes = 0; // on-disk .c2 size
  uint32_t wav_bytes = 0;  // source or decoded wav size
  int mode = kMode;
  char err[48] = {0};
};

// Encode mono PCM WAV (8 or 16 kHz) → .c2 (header + packed frames).
// 16 kHz is decimated 2:1 (pair average). Other rates rejected.
Result encodeWavToC2(const char* wavPath, const char* c2Path = kProbeC2Path);

// Decode .c2 → mono 8 kHz PCM WAV.
Result decodeC2ToWav(const char* c2Path = kProbeC2Path,
                     const char* wavPath = kProbeC2WavPath);

// Encode probe.wav → probe.c2 → probe_c2.wav (does not play).
Result roundTripFiles(const char* wavIn, const char* c2Path = kProbeC2Path,
                      const char* wavOut = kProbeC2WavPath);

bool c2Exists(const char* path = kProbeC2Path);

// Ensure /Files and /Files/voice exist on SD so callers (LXMF ingest,
// decode, encode) can write into them without racing boot. Returns false
// if SD mkdir / exists checks fail. SD.mkdir() on an existing dir
// returns false but exists() then returns true — accept either.
bool ensureVoiceDirs();

}  // namespace Codec2Voice
