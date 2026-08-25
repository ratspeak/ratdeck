// Codec2 encode/decode for short voice memos (mode 1300).
// Encode/decode run on a dedicated FreeRTOS task with a large stack —
// codec2_encode_1300 / analyse_one_frame blow the default ~8KB Arduino loop stack.
//
// Plus port: Arduino SD.h API (SD is already begun by SDStore in main).
// Pro uses an SdCard::* facade; Plus uses the global SD object directly.

#include "util/Codec2Voice.h"
#include <SD.h>

#include <codec2.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

namespace Codec2Voice {
namespace {

// File magic: "C2V1" + mode u16 LE + nframes u32 LE + packed frames
static constexpr char kMagic[4] = {'C', '2', 'V', '1'};
static constexpr size_t kHeaderSize = 4 + 2 + 4;
// Codec2 FFT/analysis needs well over 8KB; 40KB is comfortable on S3.
static constexpr uint32_t kWorkerStackWords = 40960 / sizeof(StackType_t);
static constexpr uint32_t kWorkerTimeoutMs = 60000;

int16_t* allocPcm(size_t samples) {
  size_t bytes = samples * sizeof(int16_t);
  int16_t* p =
      (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  return p;
}

uint8_t* allocBytes(size_t n) {
  uint8_t* p =
      (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t*)heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}

void setErr(Result& r, const char* msg) {
  r.ok = false;
  strncpy(r.err, msg, sizeof(r.err) - 1);
  r.err[sizeof(r.err) - 1] = 0;
}

// Plus: SD is owned by SDStore (mounted at boot). mkdir() on an existing
// dir returns false but exists() is true — accept either.
bool ensureVoiceDirs() {
  SD.mkdir("/Files");
  if (!SD.exists("/Files")) return false;
  SD.mkdir("/Files/voice");
  if (!SD.exists("/Files/voice")) return false;
  return true;
}

void feedWdt() {
  yield();
  esp_task_wdt_reset();
}

void writeWavHeader(File& f, uint32_t dataBytes, uint32_t sampleRate) {
  const uint16_t channels = 1;
  const uint16_t bits = 16;
  const uint32_t byteRate = sampleRate * channels * (bits / 8);
  const uint16_t blockAlign = channels * (bits / 8);
  const uint32_t riffSize = 36 + dataBytes;

  auto w16 = [&](uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xff), (uint8_t)(v >> 8)};
    f.write(b, 2);
  };
  auto w32 = [&](uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
                    (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff)};
    f.write(b, 4);
  };

  f.write((const uint8_t*)"RIFF", 4);
  w32(riffSize);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  w32(16);
  w16(1);
  w16(channels);
  w32(sampleRate);
  w32(byteRate);
  w16(blockAlign);
  w16(bits);
  f.write((const uint8_t*)"data", 4);
  w32(dataBytes);
}

// Load mono 16-bit PCM from WAV. *outRate set. Caller frees *outPcm.
bool loadMonoPcm(const char* path, int16_t** outPcm, uint32_t* outSamples,
                 uint32_t* outRate, char* err, size_t errLen) {
  *outPcm = nullptr;
  *outSamples = 0;
  *outRate = 0;
  if (err && errLen) err[0] = 0;

  File f = SD.open(path, FILE_READ);
  if (!f) {
    if (err) strncpy(err, "open wav fail", errLen - 1);
    return false;
  }

  uint8_t hdr[12];
  if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
      memcmp(hdr + 8, "WAVE", 4) != 0) {
    f.close();
    if (err) strncpy(err, "not WAV", errLen - 1);
    return false;
  }

  uint16_t audioFormat = 0, channels = 0, bits = 0;
  uint32_t rate = 0, dataBytes = 0, dataOffset = 0;
  bool gotFmt = false, gotData = false;

  while (f.available() >= 8) {
    uint8_t ch[8];
    if (f.read(ch, 8) != 8) break;
    uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) |
                  ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
    uint32_t chunkStart = f.position();

    if (memcmp(ch, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      size_t n = f.read(fmt, sz > 16 ? 16 : sz);
      if (n < 16) {
        f.close();
        if (err) strncpy(err, "bad fmt", errLen - 1);
        return false;
      }
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

  if (!gotFmt || !gotData || audioFormat != 1 || bits != 16) {
    f.close();
    if (err) strncpy(err, "need PCM16 WAV", errLen - 1);
    return false;
  }
  if (channels != 1 && channels != 2) {
    f.close();
    if (err) strncpy(err, "bad channels", errLen - 1);
    return false;
  }
  if (rate != 8000 && rate != 16000) {
    f.close();
    if (err) strncpy(err, "need 8/16 kHz", errLen - 1);
    return false;
  }

  const size_t frameBytes = (size_t)channels * sizeof(int16_t);
  uint32_t nFrames = dataBytes / frameBytes;
  if (nFrames == 0) {
    f.close();
    if (err) strncpy(err, "empty wav", errLen - 1);
    return false;
  }

  // Cap ~12 s @ 16 kHz to bound RAM.
  const uint32_t kMaxIn = 16000u * 12u;
  if (nFrames > kMaxIn) nFrames = kMaxIn;

  int16_t* pcm = allocPcm(nFrames);
  if (!pcm) {
    f.close();
    if (err) strncpy(err, "OOM pcm", errLen - 1);
    return false;
  }

  f.seek(dataOffset);
  if (channels == 1) {
    size_t want = nFrames * sizeof(int16_t);
    size_t got = f.read((uint8_t*)pcm, want);
    f.close();
    if (got < sizeof(int16_t)) {
      free(pcm);
      if (err) strncpy(err, "read short", errLen - 1);
      return false;
    }
    *outSamples = (uint32_t)(got / sizeof(int16_t));
  } else {
    // Downmix stereo → mono.
    constexpr size_t kChunk = 256;
    int16_t tmp[kChunk * 2];
    uint32_t out = 0;
    while (out < nFrames) {
      size_t need = nFrames - out;
      if (need > kChunk) need = kChunk;
      size_t got = f.read((uint8_t*)tmp, need * 2 * sizeof(int16_t));
      size_t frames = got / (2 * sizeof(int16_t));
      if (frames == 0) break;
      for (size_t i = 0; i < frames; i++) {
        int32_t m = ((int32_t)tmp[i * 2] + (int32_t)tmp[i * 2 + 1]) / 2;
        pcm[out + i] = (int16_t)m;
      }
      out += (uint32_t)frames;
      feedWdt();
    }
    f.close();
    *outSamples = out;
  }

  *outPcm = pcm;
  *outRate = rate;
  return *outSamples > 0;
}

// 16 kHz → 8 kHz pair-average decimation (in-place shrink).
uint32_t decimate16to8(int16_t* pcm, uint32_t n) {
  uint32_t out = n / 2;
  for (uint32_t i = 0; i < out; i++) {
    int32_t a = pcm[i * 2];
    int32_t b = pcm[i * 2 + 1];
    pcm[i] = (int16_t)((a + b) / 2);
  }
  return out;
}

// ---- heavy work (must run on large-stack task) ----

Result encodeWavToC2_impl(const char* wavPath, const char* c2Path) {
  Result r;
  r.mode = kMode;
  if (!wavPath || !c2Path) {
    setErr(r, "bad path");
    return r;
  }
  if (!ensureVoiceDirs()) {
    setErr(r, "SD dirs fail");
    return r;
  }

  Serial.printf("[c2] encode begin free=%u largest=%u stackHWM=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  int16_t* pcm = nullptr;
  uint32_t n = 0, rate = 0;
  char err[40] = {0};
  if (!loadMonoPcm(wavPath, &pcm, &n, &rate, err, sizeof(err))) {
    setErr(r, err[0] ? err : "load wav fail");
    return r;
  }
  r.wav_bytes = 44 + n * sizeof(int16_t);

  if (rate == 16000) {
    n = decimate16to8(pcm, n);
    rate = 8000;
  }
  if (rate != 8000 || n < 160) {
    free(pcm);
    setErr(r, "too short/bad rate");
    return r;
  }

  // Codec2 assumes zero-mean speech. PDM probe.wav carries a large DC offset;
  // encoding DC-shifted PCM yields rail spikes on decode that starve playWav.
  {
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += pcm[i];
    const int32_t mean = (int32_t)(sum / (int64_t)n);
    if (mean != 0) {
      for (uint32_t i = 0; i < n; i++) {
        int32_t v = (int32_t)pcm[i] - mean;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
      }
    }
    Serial.printf("[c2] encode DC remove mean=%ld samples8k=%lu\n", (long)mean,
                  (unsigned long)n);
  }

  Serial.printf("[c2] before create samples8k=%lu free=%u\n", (unsigned long)n,
                (unsigned)ESP.getFreeHeap());
  struct CODEC2* c2 = codec2_create(CODEC2_MODE_1300);
  if (!c2) {
    free(pcm);
    setErr(r, "codec2_create fail");
    return r;
  }
  Serial.printf("[c2] codec2_create ok stackHWM=%u\n",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  const int spf = codec2_samples_per_frame(c2);
  const int bpf = codec2_bytes_per_frame(c2);
  if (spf <= 0 || bpf <= 0) {
    codec2_destroy(c2);
    free(pcm);
    setErr(r, "bad frame size");
    return r;
  }

  const uint32_t nframes = n / (uint32_t)spf;
  if (nframes == 0) {
    codec2_destroy(c2);
    free(pcm);
    setErr(r, "no full frames");
    return r;
  }

  const size_t payload = (size_t)nframes * (size_t)bpf;
  uint8_t* packed = allocBytes(payload);
  if (!packed) {
    codec2_destroy(c2);
    free(pcm);
    setErr(r, "OOM c2 buf");
    return r;
  }

  Serial.printf("[c2] encode mode=1300 spf=%d bpf=%d frames=%lu\n", spf, bpf,
                (unsigned long)nframes);

  for (uint32_t i = 0; i < nframes; i++) {
    codec2_encode(c2, packed + (size_t)i * (size_t)bpf,
                  pcm + (size_t)i * (size_t)spf);
    if ((i & 7) == 0) {
      feedWdt();
      if (i == 0) {
        Serial.printf("[c2] first frame ok stackHWM=%u\n",
                      (unsigned)uxTaskGetStackHighWaterMark(nullptr));
      }
    }
  }
  codec2_destroy(c2);
  free(pcm);

  File f = SD.open(c2Path, FILE_WRITE);
  if (!f) {
    free(packed);
    setErr(r, "open c2 fail");
    return r;
  }

  f.write((const uint8_t*)kMagic, 4);
  uint8_t m[2] = {(uint8_t)(kMode & 0xff), (uint8_t)((kMode >> 8) & 0xff)};
  f.write(m, 2);
  uint8_t nf[4] = {(uint8_t)(nframes & 0xff), (uint8_t)((nframes >> 8) & 0xff),
                   (uint8_t)((nframes >> 16) & 0xff),
                   (uint8_t)((nframes >> 24) & 0xff)};
  f.write(nf, 4);
  size_t w = f.write(packed, payload);
  f.flush();
  f.close();
  free(packed);

  if (w != payload) {
    setErr(r, "c2 write short");
    return r;
  }

  r.ok = true;
  r.pcm_samples_8k = nframes * (uint32_t)spf;
  r.frames = nframes;
  r.c2_bytes = (uint32_t)payload;
  r.file_bytes = (uint32_t)(kHeaderSize + payload);
  Serial.printf("[c2] wrote %s payload=%lu file=%lu stackHWM=%u\n", c2Path,
                (unsigned long)r.c2_bytes, (unsigned long)r.file_bytes,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  return r;
}

Result decodeC2ToWav_impl(const char* c2Path, const char* wavPath) {
  Result r;
  r.mode = kMode;
  if (!c2Path || !wavPath) {
    setErr(r, "bad path");
    return r;
  }
  if (!ensureVoiceDirs()) {
    setErr(r, "SD dirs fail");
    return r;
  }
  if (!SD.exists(c2Path)) {
    setErr(r, "no c2 file");
    return r;
  }

  Serial.printf("[c2] decode begin free=%u stackHWM=%u\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  File f = SD.open(c2Path, FILE_READ);
  if (!f) {
    setErr(r, "open c2 fail");
    return r;
  }

  uint8_t hdr[kHeaderSize];
  if (f.read(hdr, kHeaderSize) != (int)kHeaderSize) {
    f.close();
    setErr(r, "short c2 hdr");
    return r;
  }
  if (memcmp(hdr, kMagic, 4) != 0) {
    f.close();
    setErr(r, "bad c2 magic");
    return r;
  }
  int mode = (int)hdr[4] | ((int)hdr[5] << 8);
  uint32_t nframes = (uint32_t)hdr[6] | ((uint32_t)hdr[7] << 8) |
                     ((uint32_t)hdr[8] << 16) | ((uint32_t)hdr[9] << 24);
  if (mode != kMode) {
    f.close();
    setErr(r, "c2 mode != 1300");
    return r;
  }
  if (nframes == 0 || nframes > 5000) {
    f.close();
    setErr(r, "bad nframes");
    return r;
  }

  struct CODEC2* c2 = codec2_create(CODEC2_MODE_1300);
  if (!c2) {
    f.close();
    setErr(r, "codec2_create fail");
    return r;
  }
  const int spf = codec2_samples_per_frame(c2);
  const int bpf = codec2_bytes_per_frame(c2);
  if (spf <= 0 || bpf <= 0) {
    codec2_destroy(c2);
    f.close();
    setErr(r, "bad frame size");
    return r;
  }

  const size_t payload = (size_t)nframes * (size_t)bpf;
  uint8_t* packed = allocBytes(payload);
  if (!packed) {
    codec2_destroy(c2);
    f.close();
    setErr(r, "OOM c2 buf");
    return r;
  }
  size_t got = f.read(packed, payload);
  f.close();
  if (got != payload) {
    free(packed);
    codec2_destroy(c2);
    setErr(r, "c2 read short");
    return r;
  }

  const uint32_t nsamp = nframes * (uint32_t)spf;
  int16_t* pcm = allocPcm(nsamp);
  if (!pcm) {
    free(packed);
    codec2_destroy(c2);
    setErr(r, "OOM pcm");
    return r;
  }

  Serial.printf("[c2] decode frames=%lu spf=%d bpf=%d\n", (unsigned long)nframes,
                spf, bpf);
  int16_t peak8 = 0;
  for (uint32_t i = 0; i < nframes; i++) {
    codec2_decode(c2, pcm + (size_t)i * (size_t)spf,
                  packed + (size_t)i * (size_t)bpf);
    if ((i & 7) == 0) feedWdt();
  }
  codec2_destroy(c2);
  free(packed);

  // Leave decode levels as-is. playWav applies DC remove + gated-RMS norm +
  // soft limiter so probe.wav and probe_c2.wav match perceived loudness
  // (pre-play gain is a no-op under the normalizer).
  for (uint32_t i = 0; i < nsamp; i++) {
    int16_t v = pcm[i];
    int16_t a = (v < 0) ? (int16_t)(-v) : v;
    if (a > peak8) peak8 = a;
  }
  Serial.printf("[c2] decode peak8=%d (loudness deferred to playWav)\n",
                (int)peak8);

  // Upsample 8 kHz → 16 kHz (linear hold) so playWav uses the same I2S clock
  // as probe.wav. Changing I2S to 8 kHz after a 16 kHz install was silent on
  // this PCM5102A path (amp pop only).
  const uint32_t nsamp16 = nsamp * 2u;
  int16_t* pcm16 = allocPcm(nsamp16);
  if (!pcm16) {
    free(pcm);
    setErr(r, "OOM upsample");
    return r;
  }
  for (uint32_t i = 0; i < nsamp; i++) {
    int16_t s = pcm[i];
    pcm16[i * 2] = s;
    pcm16[i * 2 + 1] = s;
  }
  free(pcm);

  File out = SD.open(wavPath, FILE_WRITE);
  if (!out) {
    free(pcm16);
    setErr(r, "open out wav fail");
    return r;
  }
  const uint32_t dataBytes = nsamp16 * sizeof(int16_t);
  writeWavHeader(out, dataBytes, 16000u);
  const size_t kW = 1024;
  uint32_t left = nsamp16;
  const int16_t* p = pcm16;
  while (left > 0) {
    size_t n = left > kW ? kW : left;
    if (out.write((const uint8_t*)p, n * sizeof(int16_t)) !=
        n * sizeof(int16_t)) {
      out.close();
      free(pcm16);
      setErr(r, "wav write short");
      return r;
    }
    p += n;
    left -= (uint32_t)n;
    feedWdt();
  }
  out.flush();
  out.close();
  free(pcm16);

  r.ok = true;
  r.pcm_samples_8k = nsamp;
  r.frames = nframes;
  r.c2_bytes = (uint32_t)payload;
  r.file_bytes = (uint32_t)(kHeaderSize + payload);
  r.wav_bytes = 44 + dataBytes;
  Serial.printf("[c2] decoded %s wav16=%lu peak8=%d stackHWM=%u\n", wavPath,
                (unsigned long)r.wav_bytes, (int)peak8,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  return r;
}

Result roundTripFiles_impl(const char* wavIn, const char* c2Path,
                           const char* wavOut) {
  Result e = encodeWavToC2_impl(wavIn, c2Path);
  if (!e.ok) return e;
  Result d = decodeC2ToWav_impl(c2Path, wavOut);
  if (!d.ok) return d;
  d.wav_bytes = e.wav_bytes;  // original wav size for ratio display
  return d;
}

// ---- worker dispatch ----

enum class JobOp : uint8_t { Encode, Decode, RoundTrip };

struct Job {
  JobOp op;
  char pathA[96];
  char pathB[96];
  char pathC[96];
  Result result;
  SemaphoreHandle_t done;
};

void workerTask(void* arg) {
  Job* j = static_cast<Job*>(arg);
  // Subscribe so feedWdt() is valid on this task.
  esp_task_wdt_add(nullptr);
  Serial.printf("[c2] worker start op=%u stackHWM=%u\n", (unsigned)j->op,
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));

  switch (j->op) {
    case JobOp::Encode:
      j->result = encodeWavToC2_impl(j->pathA, j->pathB);
      break;
    case JobOp::Decode:
      j->result = decodeC2ToWav_impl(j->pathA, j->pathB);
      break;
    case JobOp::RoundTrip:
      j->result = roundTripFiles_impl(j->pathA, j->pathB, j->pathC);
      break;
  }

  Serial.printf("[c2] worker done ok=%d err=%s stackHWM=%u\n",
                j->result.ok ? 1 : 0, j->result.err[0] ? j->result.err : "-",
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  esp_task_wdt_delete(nullptr);
  xSemaphoreGive(j->done);
  vTaskDelete(nullptr);
}

Result runOnWorker(JobOp op, const char* a, const char* b, const char* c) {
  Result r;
  r.mode = kMode;
  if (!a || !b) {
    setErr(r, "bad path");
    return r;
  }

  Job job{};
  job.op = op;
  job.result = Result{};
  job.result.mode = kMode;
  strncpy(job.pathA, a, sizeof(job.pathA) - 1);
  strncpy(job.pathB, b, sizeof(job.pathB) - 1);
  if (c) strncpy(job.pathC, c, sizeof(job.pathC) - 1);

  job.done = xSemaphoreCreateBinary();
  if (!job.done) {
    setErr(r, "sem fail");
    return r;
  }

  // Pin to app core (1); large stack for Codec2 FFT locals.
  BaseType_t ok = xTaskCreatePinnedToCore(
      workerTask, "c2work", kWorkerStackWords, &job, 1, nullptr, 1);
  if (ok != pdPASS) {
    vSemaphoreDelete(job.done);
    setErr(r, "task create fail");
    return r;
  }

  const uint32_t t0 = millis();
  while (xSemaphoreTake(job.done, pdMS_TO_TICKS(200)) != pdTRUE) {
    feedWdt();
    if (millis() - t0 > kWorkerTimeoutMs) {
      // Worker stuck or crashed — do not free job (worker may still touch it).
      Serial.println("[c2] worker timeout");
      setErr(r, "c2 timeout");
      // Leak semaphore rather than UAF if worker still alive.
      return r;
    }
  }
  vSemaphoreDelete(job.done);
  return job.result;
}

}  // namespace

Result encodeWavToC2(const char* wavPath, const char* c2Path) {
  return runOnWorker(JobOp::Encode, wavPath, c2Path, nullptr);
}

Result decodeC2ToWav(const char* c2Path, const char* wavPath) {
  return runOnWorker(JobOp::Decode, c2Path, wavPath, nullptr);
}

Result roundTripFiles(const char* wavIn, const char* c2Path,
                      const char* wavOut) {
  return runOnWorker(JobOp::RoundTrip, wavIn, c2Path, wavOut);
}

bool c2Exists(const char* path) {
  if (!path || !path[0]) return false;
  if (!SD.exists(path)) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint64_t sz = f.size();
  f.close();
  return sz > kHeaderSize;
}

}  // namespace Codec2Voice
