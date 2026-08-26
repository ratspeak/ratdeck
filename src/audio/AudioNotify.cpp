// Audio output for T-Deck Plus via I2S speaker amplifier
#include "AudioNotify.h"
#include "config/BoardConfig.h"
#include "config/Config.h"
#include <driver/i2s.h>
#include <math.h>
#include <SD.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE  16000
#define I2S_PORT           I2S_NUM_0

void AudioNotify::begin() {
    i2s_config_t i2s_config = {};
    i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    i2s_config.sample_rate = AUDIO_SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    i2s_config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    i2s_config.dma_buf_count = 4;
    i2s_config.dma_buf_len = 256;
    i2s_config.use_apll = false;
    i2s_config.tx_desc_auto_clear = true;

    i2s_pin_config_t pin_config = {};
    pin_config.mck_io_num = I2S_MCLK;
    pin_config.bck_io_num = I2S_BCK;
    pin_config.ws_io_num = I2S_WS;
    pin_config.data_out_num = I2S_DOUT;
    pin_config.data_in_num = I2S_PIN_NO_CHANGE;

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] I2S install failed: %d\n", err);
        return;
    }

    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[AUDIO] I2S pin config failed: %d\n", err);
        i2s_driver_uninstall(I2S_PORT);
        return;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    _i2sReady = true;
    Serial.println("[AUDIO] I2S initialized");
}

void AudioNotify::end() {
    if (_i2sReady) {
        i2s_driver_uninstall(I2S_PORT);
        _i2sReady = false;
    }
}

void AudioNotify::writeTone(uint16_t freq, uint16_t durationMs) {
    if (!_enabled || !_i2sReady) return;

    int numSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
    int16_t* buf = (int16_t*)ps_malloc(numSamples * sizeof(int16_t));
    if (!buf) buf = (int16_t*)malloc(numSamples * sizeof(int16_t));
    if (!buf) return;

    float vol = (_volume / 100.0f) * 16000.0f;
    int fadeN = AUDIO_SAMPLE_RATE / 100; // 10ms fade

    for (int i = 0; i < numSamples; i++) {
        float t = (float)i / AUDIO_SAMPLE_RATE;
        // Fundamental + 2nd/3rd harmonics for warmth
        float s = sinf(2.0f * M_PI * freq * t) * 0.70f
                + sinf(2.0f * M_PI * freq * 2.0f * t) * 0.20f
                + sinf(2.0f * M_PI * freq * 3.0f * t) * 0.10f;
        // Fade envelope
        float env = 1.0f;
        if (i < fadeN) env = (float)i / fadeN;
        if (i > numSamples - fadeN) env = (float)(numSamples - i) / fadeN;
        buf[i] = (int16_t)(s * env * vol);
    }

    size_t written = 0;
    i2s_write(I2S_PORT, buf, numSamples * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
    free(buf);
}

void AudioNotify::writeSilence(uint16_t durationMs) {
    if (!_i2sReady) return;
    int numSamples = (AUDIO_SAMPLE_RATE * durationMs) / 1000;
    size_t bufSize = numSamples * sizeof(int16_t);
    int16_t* buf = (int16_t*)ps_malloc(bufSize);
    if (!buf) buf = (int16_t*)malloc(bufSize);
    if (!buf) return;
    memset(buf, 0, bufSize);
    size_t written = 0;
    i2s_write(I2S_PORT, buf, numSamples * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
    free(buf);
}

void AudioNotify::playMessage() {
    if (!_enabled || !_i2sReady) return;

    const int sr = AUDIO_SAMPLE_RATE;
    const int toneMs = 34;
    const int gapMs = 28;
    const int tailMs = 16;
    const int totalMs = toneMs + gapMs + toneMs + tailMs;
    const int totalSamples = sr * totalMs / 1000;

    int16_t* buf = (int16_t*)ps_malloc(totalSamples * sizeof(int16_t));
    if (!buf) buf = (int16_t*)malloc(totalSamples * sizeof(int16_t));
    if (!buf) return;
    memset(buf, 0, totalSamples * sizeof(int16_t));

    float vol = (_volume / 100.0f) * 12000.0f;
    int pos = 0;

    auto addTone = [&](float freq, int ms) {
        int n = sr * ms / 1000;
        int fadeN = sr * 5 / 1000;
        if (fadeN < 1) fadeN = 1;
        for (int i = 0; i < n && (pos + i) < totalSamples; i++) {
            float t = (float)i / sr;
            float s = sinf(2.0f * M_PI * freq * t) * 0.80f
                    + sinf(2.0f * M_PI * freq * 2.0f * t) * 0.15f
                    + sinf(2.0f * M_PI * freq * 3.0f * t) * 0.05f;
            float env = 1.0f;
            if (i < fadeN) env = (float)i / fadeN;
            if (i > n - fadeN) env = (float)(n - i) / fadeN;
            buf[pos + i] = (int16_t)(s * env * vol);
        }
        pos += n;
    };

    addTone(1000.0f, toneMs);
    pos += sr * gapMs / 1000;
    addTone(1000.0f, toneMs);
    pos += sr * tailMs / 1000;

    size_t written = 0;
    i2s_write(I2S_PORT, buf, totalSamples * sizeof(int16_t), &written, pdMS_TO_TICKS(150));
    free(buf);
}

void AudioNotify::requestMessage() {
    if (!_enabled) return;
    _messagePending = true;
}

void AudioNotify::loop() {
    if (!_messagePending) return;
    _messagePending = false;
    playMessage();
}

void AudioNotify::playAnnounce() {
    if (!_enabled) return;
    writeTone(800, 30);
    writeSilence(20);
}

void AudioNotify::playError() {
    if (!_enabled) return;
    for (int i = 0; i < 3; i++) {
        writeTone(400, 100);
        if (i < 2) writeSilence(50);
    }
    writeSilence(30);
}

void AudioNotify::playBoot() {
    if (!_enabled || !_i2sReady) return;
#if !RSDECK_PLAY_BOOT_SOUND
    return;
#endif

    // === RSDECK BOOT SEQUENCE ===
    // Sci-fi computer startup: sweep -> digital arpeggio -> confirmation
    // Total ~550ms

    const int sr = AUDIO_SAMPLE_RATE;
    const int totalMs = 560;
    const int totalSamples = sr * totalMs / 1000;

    int16_t* buf = (int16_t*)ps_malloc(totalSamples * sizeof(int16_t));
    if (!buf) {
        buf = (int16_t*)malloc(totalSamples * sizeof(int16_t));
        if (!buf) return;
    }
    memset(buf, 0, totalSamples * sizeof(int16_t));

    float vol = (_volume / 100.0f) * 16000.0f;
    int pos = 0;

    // Helper: add a tone with harmonics at current position
    auto addTone = [&](float freq, int ms) {
        int n = sr * ms / 1000;
        int fadeN = sr * 8 / 1000; // 8ms fade
        for (int i = 0; i < n && (pos + i) < totalSamples; i++) {
            float t = (float)i / sr;
            float s = sinf(2.0f * M_PI * freq * t) * 0.65f
                    + sinf(2.0f * M_PI * freq * 2.0f * t) * 0.22f
                    + sinf(2.0f * M_PI * freq * 3.0f * t) * 0.13f;
            float env = 1.0f;
            if (i < fadeN) env = (float)i / fadeN;
            if (i > n - fadeN) env = (float)(n - i) / fadeN;
            buf[pos + i] = (int16_t)(s * env * vol);
        }
        pos += n;
    };

    // Helper: frequency sweep with harmonics
    auto addSweep = [&](float startF, float endF, int ms) {
        int n = sr * ms / 1000;
        int fadeN = sr * 8 / 1000;
        float phase = 0;
        for (int i = 0; i < n && (pos + i) < totalSamples; i++) {
            float t = (float)i / n; // 0..1 progress
            float freq = startF + (endF - startF) * t * t; // quadratic sweep (accelerating)
            phase += 2.0f * M_PI * freq / sr;
            float s = sinf(phase) * 0.65f
                    + sinf(phase * 2.0f) * 0.22f
                    + sinf(phase * 3.0f) * 0.08f;
            float env = 1.0f;
            if (i < fadeN) env = (float)i / fadeN;
            if (i > n - fadeN) env = (float)(n - i) / fadeN;
            buf[pos + i] = (int16_t)(s * env * vol);
        }
        pos += n;
    };

    auto addSilence = [&](int ms) {
        pos += sr * ms / 1000;
    };

    // Phase 1: Rising power sweep 300->1200Hz (160ms) — "systems powering up"
    addSweep(300, 1200, 160);
    addSilence(25);

    // Phase 2: Three quick ascending staccato notes — E5, G#5, B5
    // (E major triad in 2nd inversion — bright, triumphant, slightly edgy)
    addTone(659,  45);   // E5
    addSilence(12);
    addTone(831,  45);   // G#5
    addSilence(12);
    addTone(988,  45);   // B5
    addSilence(25);

    // Phase 3: Descending glitch sweep 2400->1600Hz (60ms) — "digital handshake"
    addSweep(2400, 1600, 60);
    addSilence(20);

    // Phase 4: Final confirmation — E6 (1319Hz), 100ms with clean decay — "online"
    addTone(1319, 100);

    // Write entire sequence at once for seamless playback
    size_t written = 0;
    i2s_write(I2S_PORT, buf, pos * sizeof(int16_t), &written, pdMS_TO_TICKS(200));

    // Flush with silence
    memset(buf, 0, 512 * sizeof(int16_t));
    i2s_write(I2S_PORT, buf, 512 * sizeof(int16_t), &written, pdMS_TO_TICKS(200));

    free(buf);
}

// =============================================================================
// WAV playback — Mono PCM16 from SD.
// Plus I2S is ONLY_LEFT at 16 kHz (no stereo pairs; amp is mono anyway).
//
// Loudness model (ported from Pro audio.cpp, gated-RMS + soft knee 4:1):
//   1. Read WAV header, locate data chunk.
//   2. Pass 1a: mean (DC remove). PDM probe.wav carries a large DC offset.
//   3. Pass 1b: peak of AC (sample − dc). Sets the gate threshold.
//   4. Pass 1c: speech-gated RMS of AC (gate = max(peakAC/16, 100)).
//   5. gainQ8 = clamp(kTargetRms * 256 / rmsActive, [kUnityQ8, kMaxGainQ8]).
//      Fallback: peak-norm to kTargetPeakFallback if too few active samples.
//      Never attenuate below unity.
//   6. Pass 2: subtract DC, apply gain + soft knee, write I2S.
//
// 8 kHz mono WAVs are upsampled (hold) inline so we don't have to reprogram
// the I2S clock (changing rate after install was silent on PCM5102A).
// =============================================================================

namespace {

constexpr int32_t kTargetRms = 16000;     // ≈ −6 dBFS speech body (+6 dB vs 8000)
constexpr int32_t kKneeT = 24576;         // soft knee at 75% FS
constexpr int32_t kMaxGainQ8 = 64 * 256;  // absolute cap 64x
constexpr int32_t kUnityQ8 = 256;
constexpr int32_t kTargetPeakFallback = 28000;  // near-silent file fallback

inline int16_t scaleSampleQ8(int32_t v, int32_t gainQ8, uint32_t* limited,
                             uint32_t* clipped) {
  int32_t x = (v * gainQ8) >> 8;
  int32_t ax = (x < 0) ? -x : x;
  if (ax > kKneeT) {
    if (limited) (*limited)++;
    int32_t over = (ax - kKneeT) >> 2;  // 4:1 above knee
    ax = kKneeT + over;
    if (ax > 32767) {
      ax = 32767;
      if (clipped) (*clipped)++;
    }
    x = (x < 0) ? -ax : ax;
  }
  return (int16_t)x;
}

struct WavInfo {
  bool ok = false;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint32_t rate = 0;
  uint32_t dataBytes = 0;
  uint32_t dataOffset = 0;
};

bool parseWavHeader(File& f, WavInfo& info) {
  info = {};
  if (!f) return false;
  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
    return false;
  }

  uint16_t audioFormat = 0;
  uint16_t channels = 0;
  uint32_t rate = 0;
  uint16_t bits = 0;
  uint32_t dataBytes = 0;
  uint32_t dataOffset = 0;
  bool gotFmt = false;
  bool gotData = false;

  while (f.available() >= 8) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) break;
    uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                  ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
    uint32_t chunkStart = f.position();

    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      size_t n = f.read(fmt, sz > 16 ? 16 : sz);
      if (n < 16) return false;
      audioFormat = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
      channels = (uint16_t)fmt[2] | ((uint16_t)fmt[3] << 8);
      rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) |
             ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      bits = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
      gotFmt = true;
      if (sz > 16) f.seek(chunkStart + sz);
    } else if (memcmp(ch, "data", 4) == 0) {
      dataBytes = sz;
      dataOffset = f.position();
      gotData = true;
      break;
    } else {
      f.seek(chunkStart + sz);
    }
    if (sz & 1) f.seek(f.position() + 1);
  }

  if (!gotFmt || !gotData) return false;
  if (audioFormat != 1) return false;
  if (bits != 16) return false;
  if (channels != 1 && channels != 2) return false;
  if (rate < 8000 || rate > 48000) return false;

  info.ok = true;
  info.channels = channels;
  info.bits = bits;
  info.rate = rate;
  info.dataBytes = dataBytes;
  info.dataOffset = dataOffset;
  return true;
}

}  // namespace

bool AudioNotify::wavExists(const char* path) {
  if (!path || !path[0]) return false;
  if (!SD.exists(path)) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint64_t sz = f.size();
  f.close();
  return sz > 44;
}

bool AudioNotify::playWav(const char* path) {
  if (!path || !path[0]) return false;

  // Ensure driver is up (begin() is idempotent).
  if (!_i2sReady) begin();
  if (!_i2sReady) {
    Serial.println("[audio] play: I2S not ready");
    return false;
  }

  File f = SD.open(path, FILE_READ);
  if (!f) {
    Serial.printf("[audio] play: open %s failed\n", path);
    return false;
  }

  WavInfo info;
  if (!parseWavHeader(f, info)) {
    f.close();
    Serial.println("[audio] play: bad WAV header");
    return false;
  }

  const size_t frameBytes = (size_t)info.channels * sizeof(int16_t);
  if (frameBytes == 0 || info.dataBytes < frameBytes) {
    f.close();
    Serial.println("[audio] play: empty WAV");
    return false;
  }

  // Pass 1a: mean (DC) + raw peak.
  f.seek(info.dataOffset);
  int64_t sum = 0;
  uint32_t nFrames = 0;
  int32_t peakRaw = 0;
  {
    constexpr size_t kMono = 512;
    int16_t mono[kMono * 2];
    uint32_t left = info.dataBytes;
    while (left >= frameBytes) {
      size_t frames = kMono;
      if (frames * frameBytes > left) frames = left / frameBytes;
      size_t want = frames * frameBytes;
      int n = f.read((uint8_t*)mono, want);
      if (n <= 0) break;
      size_t gotFrames = (size_t)n / frameBytes;
      if (gotFrames == 0) break;
      for (size_t i = 0; i < gotFrames; i++) {
        int32_t s;
        if (info.channels == 1) {
          s = mono[i];
        } else {
          s = ((int32_t)mono[i * 2] + (int32_t)mono[i * 2 + 1]) / 2;
        }
        sum += s;
        nFrames++;
        int32_t a = (s < 0) ? -s : s;
        if (a > peakRaw) peakRaw = a;
      }
      left -= (uint32_t)(gotFrames * frameBytes);
      yield();
    }
  }
  const int32_t dc = (nFrames > 0) ? (int32_t)(sum / (int64_t)nFrames) : 0;

  // Pass 1b: peak of AC (sample − dc).
  f.seek(info.dataOffset);
  int32_t peakAC = 0;
  {
    constexpr size_t kMono = 512;
    int16_t mono[kMono * 2];
    uint32_t left = info.dataBytes;
    while (left >= frameBytes) {
      size_t frames = kMono;
      if (frames * frameBytes > left) frames = left / frameBytes;
      size_t want = frames * frameBytes;
      int n = f.read((uint8_t*)mono, want);
      if (n <= 0) break;
      size_t gotFrames = (size_t)n / frameBytes;
      if (gotFrames == 0) break;
      for (size_t i = 0; i < gotFrames; i++) {
        int32_t s;
        if (info.channels == 1) {
          s = mono[i];
        } else {
          s = ((int32_t)mono[i * 2] + (int32_t)mono[i * 2 + 1]) / 2;
        }
        int32_t ac = s - dc;
        int32_t a = (ac < 0) ? -ac : ac;
        if (a > peakAC) peakAC = a;
      }
      left -= (uint32_t)(gotFrames * frameBytes);
      yield();
    }
  }
  if (peakAC < 1) peakAC = 1;

  // Pass 1c: speech-gated RMS of AC.
  const int32_t gate = peakAC / 16;
  const int32_t gateFloor = (gate > 100) ? gate : 100;
  int64_t sumSq = 0;
  uint32_t nActive = 0;
  {
    f.seek(info.dataOffset);
    constexpr size_t kMono = 512;
    int16_t mono[kMono * 2];
    uint32_t left = info.dataBytes;
    while (left >= frameBytes) {
      size_t frames = kMono;
      if (frames * frameBytes > left) frames = left / frameBytes;
      size_t want = frames * frameBytes;
      int n = f.read((uint8_t*)mono, want);
      if (n <= 0) break;
      size_t gotFrames = (size_t)n / frameBytes;
      if (gotFrames == 0) break;
      for (size_t i = 0; i < gotFrames; i++) {
        int32_t s;
        if (info.channels == 1) {
          s = mono[i];
        } else {
          s = ((int32_t)mono[i * 2] + (int32_t)mono[i * 2 + 1]) / 2;
        }
        int32_t ac = s - dc;
        int32_t a = (ac < 0) ? -ac : ac;
        if (a >= gateFloor) {
          sumSq += (int64_t)ac * (int64_t)ac;
          nActive++;
        }
      }
      left -= (uint32_t)(gotFrames * frameBytes);
      yield();
    }
  }

  int32_t rmsActive = 0;
  if (nActive > 0) {
    rmsActive = (int32_t)sqrt((double)sumSq / (double)nActive);
  }
  if (rmsActive < 1) rmsActive = 1;

  int32_t gainQ8 = kUnityQ8;
  const bool useRms = (nFrames > 0) && (nActive >= (nFrames / 64u));
  if (useRms && rmsActive >= 16) {
    int64_t g = ((int64_t)kTargetRms * 256) / (int64_t)rmsActive;
    if (g > (int64_t)kMaxGainQ8) g = kMaxGainQ8;
    if (g < (int64_t)kUnityQ8) g = kUnityQ8;
    gainQ8 = (int32_t)g;
  } else if (peakAC >= 16) {
    int64_t g = ((int64_t)kTargetPeakFallback * 256) / (int64_t)peakAC;
    if (g < (int64_t)kUnityQ8) g = kUnityQ8;
    if (g > (int64_t)kMaxGainQ8) g = kMaxGainQ8;
    gainQ8 = (int32_t)g;
  }

  const int32_t rmsOutEst = (int32_t)(((int64_t)rmsActive * gainQ8) >> 8);
  const uint32_t actPct =
      (nFrames > 0) ? (uint32_t)((nActive * 100ull) / nFrames) : 0;

  Serial.printf(
      "[audio] play %s ch=%u rate=%lu bytes=%lu dc=%ld peakRaw=%ld peakAC=%ld "
      "rmsAct=%ld actPct=%lu gain=x%.2f rmsOutEst=%ld (%s)\n",
      path, (unsigned)info.channels, (unsigned long)info.rate,
      (unsigned long)info.dataBytes, (long)dc, (long)peakRaw, (long)peakAC,
      (long)rmsActive, (unsigned long)actPct, (double)gainQ8 / 256.0,
      (long)rmsOutEst, useRms ? "rms" : "peak-fb");

  // Reinstall I2S for a clean state — heavy SD read loops can leave the
  // DMA chain idle but warm; this resets TX state.
  end();
  delay(10);
  begin();
  if (!_i2sReady) {
    f.close();
    Serial.println("[audio] play: I2S reinstall failed");
    return false;
  }

  // 8 kHz mono → upsample (hold) inline; keep I2S at 16 kHz ONLY_LEFT.
  const bool upsample8 = (info.rate == 8000u && info.channels == 1);

  // Pass 2: subtract DC, apply gain + soft knee, write I2S.
  f.seek(info.dataOffset);
  constexpr size_t kMono = 256;
  int16_t mono[kMono * 2];
  int16_t out[kMono * 2];   // upsample 1→2 OR stereo pair (mono to I2S)
  size_t written = 0;
  uint32_t left = info.dataBytes;
  int16_t peakOut = 0;
  uint32_t limited = 0;
  uint32_t clipped = 0;
  uint32_t outFrames = 0;

  while (left >= frameBytes) {
    size_t frames = kMono;
    if (frames * frameBytes > left) frames = left / frameBytes;
    size_t want = frames * frameBytes;

    int n = f.read((uint8_t*)mono, want);
    if (n <= 0) break;
    size_t gotFrames = (size_t)n / frameBytes;
    if (gotFrames == 0) break;

    if (info.channels == 1) {
      if (upsample8) {
        for (size_t i = 0; i < gotFrames; i++) {
          int16_t v =
              scaleSampleQ8((int32_t)mono[i] - dc, gainQ8, &limited, &clipped);
          int16_t a = (v < 0) ? (int16_t)(-v) : v;
          if (a > peakOut) peakOut = a;
          out[i * 2] = v;
          out[i * 2 + 1] = v;  // hold 8k→16k
        }
        i2s_write(I2S_PORT, out, gotFrames * 2 * sizeof(int16_t), &written,
                  portMAX_DELAY);
      } else {
        for (size_t i = 0; i < gotFrames; i++) {
          int16_t v =
              scaleSampleQ8((int32_t)mono[i] - dc, gainQ8, &limited, &clipped);
          int16_t a = (v < 0) ? (int16_t)(-v) : v;
          if (a > peakOut) peakOut = a;
          out[i] = v;
        }
        i2s_write(I2S_PORT, out, gotFrames * sizeof(int16_t), &written,
                  portMAX_DELAY);
      }
    } else {
      // Stereo WAV → downmix to mono, then write once each sample.
      for (size_t i = 0; i < gotFrames; i++) {
        int32_t m =
            ((int32_t)mono[i * 2] + (int32_t)mono[i * 2 + 1]) / 2;
        int16_t v = scaleSampleQ8(m - dc, gainQ8, &limited, &clipped);
        int16_t a = (v < 0) ? (int16_t)(-v) : v;
        if (a > peakOut) peakOut = a;
        out[i] = v;
      }
      i2s_write(I2S_PORT, out, gotFrames * sizeof(int16_t), &written,
                portMAX_DELAY);
    }
    outFrames += (uint32_t)gotFrames;
    left -= (uint32_t)(gotFrames * frameBytes);
    yield();
  }

  // Flush with ~30 ms silence so amp doesn't click.
  {
    int16_t silence[256];
    memset(silence, 0, sizeof(silence));
    size_t w = 0;
    i2s_write(I2S_PORT, silence, sizeof(silence), &w, pdMS_TO_TICKS(200));
  }

  f.close();

  Serial.printf(
      "[audio] play done peakOut=%d rmsOutEst=%ld limited=%lu clipped=%lu/%lu\n",
      (int)peakOut, (long)rmsOutEst, (unsigned long)limited,
      (unsigned long)clipped, (unsigned long)outFrames);
  return true;
}
