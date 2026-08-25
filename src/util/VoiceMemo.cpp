// Voice memo wire helpers — Plus side.
//
// The wire format must match Pro exactly so cross-device playback works.
//
//   Legacy (one LXMF message): "C2 " + base64(entire .c2 file)
//   Chunked (one LXMF message per slice):
//     "C2P " + <id8hex> + " " + <i>/<n> + " " + <base64-slice>
//
// Base64 here is plain RFC 4648 standard alphabet (+, /). We do not URL-
// encode or pad-strip — outbound always carries full padding, inbound
// tolerates unpadded inputs (some message stores trim trailing '=').
//
// Receive side runs a single-flight reassembler: at most one in-progress
// memo at a time. A different id arriving mid-stream abandons the old one
// (returns Fail so the caller can log).

#include "util/VoiceMemo.h"
#include <SD.h>

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <esp_random.h>

namespace VoiceMemo {
namespace {

// Standard RFC 4648 base64 alphabet.
static const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Decode-side char → 6-bit value. Returns -1 for invalid bytes,
// -2 for '=' padding.
int b64Val(char c) {
  if (c == '=') return -2;
  const char* p = strchr(kB64Alphabet, c);
  return p ? (int)(p - kB64Alphabet) : -1;
}

inline bool startsWith(const std::string& s, const char* pfx, size_t plen) {
  if (s.size() < plen) return false;
  return memcmp(s.data(), pfx, plen) == 0;
}

inline bool startsWithLegacy(const std::string& s) {
  return startsWith(s, kPrefixLegacy, strlen(kPrefixLegacy));
}

inline bool startsWithChunk(const std::string& s) {
  return startsWith(s, kPrefixChunk, strlen(kPrefixChunk));
}

size_t b64EncodedLen(size_t rawLen) {
  return ((rawLen + 2) / 3) * 4;
}

size_t b64DecodedMaxLen(const std::string& s) {
  size_t n = s.size();
  while (n > 0 && s[n - 1] == '=') --n;
  return (n * 3) / 4 + 4;  // +4 slack for off-by-one on partial groups
}

bool b64Encode(const uint8_t* in, size_t inLen, char* out, size_t outCap,
               size_t* outLen) {
  if (!in || !out || !outLen) return false;
  size_t need = b64EncodedLen(inLen);
  if (outCap < need + 1) return false;

  size_t o = 0;
  size_t i = 0;
  while (i + 3 <= inLen) {
    uint32_t v = ((uint32_t)in[i] << 16) |
                 ((uint32_t)in[i + 1] << 8) |
                 ((uint32_t)in[i + 2]);
    out[o++] = kB64Alphabet[(v >> 18) & 0x3F];
    out[o++] = kB64Alphabet[(v >> 12) & 0x3F];
    out[o++] = kB64Alphabet[(v >> 6) & 0x3F];
    out[o++] = kB64Alphabet[v & 0x3F];
    i += 3;
  }
  size_t rem = inLen - i;
  if (rem == 1) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = kB64Alphabet[(v >> 18) & 0x3F];
    out[o++] = kB64Alphabet[(v >> 12) & 0x3F];
    out[o++] = '=';
    out[o++] = '=';
  } else if (rem == 2) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = kB64Alphabet[(v >> 18) & 0x3F];
    out[o++] = kB64Alphabet[(v >> 12) & 0x3F];
    out[o++] = kB64Alphabet[(v >> 6) & 0x3F];
    out[o++] = '=';
  }
  out[o] = '\0';
  *outLen = o;
  return true;
}

bool b64Decode(const std::string& in, uint8_t* out, size_t outCap,
               size_t* outLen) {
  if (!out || !outLen) return false;
  size_t maxLen = b64DecodedMaxLen(in);
  if (maxLen > outCap) return false;

  uint32_t buf = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < in.size(); i++) {
    char ch = in[i];
    // Tolerate stray whitespace — some senders insert \n every 76 chars.
    if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
    int v = b64Val(ch);
    if (v == -1) return false;  // Not a valid b64 char
    if (v == -2) continue;      // Padding '=' — flush handled below.
    buf = (buf << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= outCap) return false;
      out[o++] = (uint8_t)((buf >> bits) & 0xFF);
    }
  }
  *outLen = o;
  return true;
}

// Voice-memo receiver: /Files/voice must exist before we can write into
// it. SD.mkdir() returns false on an existing dir (not an error here).
bool ensureVoiceDir() {
  SD.mkdir("/Files");
  if (!SD.exists("/Files")) return false;
  SD.mkdir(kRxDir);
  if (!SD.exists(kRxDir)) return false;
  return true;
}

// Build "/Files/voice/rx_<first8hex>.c2" with zero padding if the hash
// is shorter than 8 chars. Returns false if outPath is too small.
bool buildRxPath(const char* srcHashHex, char* outPath, size_t outPathLen) {
  if (!outPath || outPathLen == 0) return false;
  const char* hex = (srcHashHex && srcHashHex[0]) ? srcHashHex : "00000000";
  char tag[9] = {'0','0','0','0','0','0','0','0','\0'};
  size_t i = 0;
  while (i < 8 && hex[i]) {
    char c = hex[i];
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F')) {
      tag[i] = (c >= 'A' && c <= 'F') ? (c - 'A' + 'a') : c;
    } else {
      tag[i] = '0';
    }
    i++;
  }
  snprintf(outPath, outPathLen, "%s/rx_%s.c2", kRxDir, tag);
  return true;
}

// Write concatenated raw bytes to outPath. Returns true on success.
bool writeC2(const char* outPath, const uint8_t* data, size_t len) {
  if (!ensureVoiceDir()) {
    Serial.println("[voice] writeC2: dir fail");
    return false;
  }
  File f = SD.open(outPath, FILE_WRITE);
  if (!f) {
    Serial.printf("[voice] writeC2: open %s failed\n", outPath);
    return false;
  }
  size_t w = f.write(data, len);
  f.flush();
  f.close();
  if (w != len) {
    Serial.printf("[voice] writeC2: short write %u/%u\n",
                  (unsigned)w, (unsigned)len);
    return false;
  }
  return true;
}

// Single-flight chunked reassembler. Only one memo at a time; if the id
// changes mid-stream the previous one is abandoned and ingest() returns
// Fail so the caller can log.
struct Inflight {
  bool active = false;
  char idHex[9] = {0};
  size_t totalParts = 0;
  std::vector<std::vector<uint8_t>> parts;
  std::vector<bool> got;
  size_t gotCount = 0;
};

static Inflight s_inflight;

void resetInflight() {
  s_inflight.active = false;
  s_inflight.idHex[0] = '\0';
  s_inflight.totalParts = 0;
  s_inflight.parts.clear();
  s_inflight.got.clear();
  s_inflight.gotCount = 0;
}

bool isHex8(const char* s) {
  if (!s) return false;
  for (int i = 0; i < 8; i++) {
    char c = s[i];
    if (c == '\0') return false;
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return s[8] == '\0';
}

// Returns true on success and writes (id, i, n) + b64 view of the chunk.
// Failures produce false with a Serial log.
bool parseChunk(const std::string& content, char idOut[9], size_t& iOut,
                size_t& nOut, std::string& b64Out) {
  const size_t pfxLen = strlen(kPrefixChunk);
  if (content.size() <= pfxLen) {
    Serial.println("[voice] chunk: too short");
    return false;
  }
  const char* p = content.c_str() + pfxLen;
  const size_t rem = content.size() - pfxLen;

  // Layout after prefix: "<id8hex> <i>/<n> <b64>"
  // Copy id8 into a small buffer for isHex8() + null term.
  char id[10] = {0};
  size_t idLen = 0;
  while (idLen < 8 && idLen < rem && p[idLen] != ' ') {
    id[idLen] = p[idLen];
    idLen++;
  }
  if (idLen != 8 || p[idLen] != ' ') {
    Serial.println("[voice] chunk: bad id8");
    return false;
  }
  id[8] = '\0';
  if (!isHex8(id)) {
    Serial.println("[voice] chunk: id not hex");
    return false;
  }
  size_t cursor = idLen + 1;  // past ' '
  if (cursor >= rem || !((p[cursor] >= '0' && p[cursor] <= '9'))) {
    Serial.println("[voice] chunk: missing i");
    return false;
  }

  // Parse "<i>/<n>".
  size_t iVal = 0, nVal = 0;
  bool sawSlash = false;
  while (cursor < rem && p[cursor] >= '0' && p[cursor] <= '9') {
    iVal = iVal * 10 + (size_t)(p[cursor] - '0');
    cursor++;
  }
  if (cursor >= rem || p[cursor] != '/') {
    Serial.println("[voice] chunk: missing /");
    return false;
  }
  cursor++;
  if (cursor >= rem || !((p[cursor] >= '0' && p[cursor] <= '9'))) {
    Serial.println("[voice] chunk: missing n");
    return false;
  }
  while (cursor < rem && p[cursor] >= '0' && p[cursor] <= '9') {
    nVal = nVal * 10 + (size_t)(p[cursor] - '0');
    cursor++;
  }
  if (cursor >= rem || p[cursor] != ' ') {
    Serial.println("[voice] chunk: missing space after n");
    return false;
  }
  cursor++;
  if (nVal == 0 || iVal == 0 || iVal > nVal) {
    Serial.printf("[voice] chunk: bad i/n i=%u n=%u\n",
                  (unsigned)iVal, (unsigned)nVal);
    return false;
  }
  if (nVal > kMaxParts) {
    Serial.printf("[voice] chunk: n=%u > kMaxParts=%u\n",
                  (unsigned)nVal, (unsigned)kMaxParts);
    return false;
  }

  memcpy(idOut, id, 9);
  iOut = iVal;
  nOut = nVal;
  b64Out.assign(p + cursor, rem - cursor);
  return true;
}

}  // namespace

bool isVoiceMemo(const std::string& content) {
  return startsWithLegacy(content) || startsWithChunk(content);
}

const char* displayText() { return kDisplay; }

std::vector<std::string> packChunksFromFile(const char* c2Path) {
  std::vector<std::string> out;
  if (!c2Path || !c2Path[0]) return out;

  File f = SD.open(c2Path, FILE_READ);
  if (!f) {
    Serial.printf("[voice] packChunksFromFile: open %s failed\n", c2Path);
    return out;
  }
  size_t sz = (size_t)f.size();
  if (sz == 0 || sz > kMaxC2Bytes) {
    Serial.printf("[voice] packChunksFromFile: bad size %u for %s\n",
                  (unsigned)sz, c2Path);
    f.close();
    return out;
  }

  // Read the whole .c2 into PSRAM.
  uint8_t* raw = (uint8_t*)malloc(sz);
  if (!raw) {
    Serial.println("[voice] packChunksFromFile: malloc fail");
    f.close();
    return out;
  }
  size_t got = f.read(raw, sz);
  f.close();
  if (got != sz) {
    Serial.printf("[voice] packChunksFromFile: short read %u/%u\n",
                  (unsigned)got, (unsigned)sz);
    free(raw);
    return out;
  }

  // Compute number of slices. We pick raw slice size = kSliceRawBytes so
  // each chunk's b64 fits inside kChunkB64Max.
  size_t n = (sz + kSliceRawBytes - 1) / kSliceRawBytes;
  if (n == 0) n = 1;
  if (n > kMaxParts) {
    Serial.printf("[voice] packChunksFromFile: n=%u > kMaxParts\n", (unsigned)n);
    free(raw);
    return out;
  }

  // 8-hex id derived from the source filename hash for stability across
  // re-packs. Fall back to a millis()-derived tag.
  uint32_t tag = (uint32_t)esp_random() ^ millis();
  char id[9];
  snprintf(id, sizeof(id), "%08x", (unsigned)tag);

  char hdr[32];
  int hdrLen = snprintf(hdr, sizeof(hdr), "%s%s %u/%u ",
                        kPrefixChunk, id, (unsigned)1, (unsigned)n);

  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    size_t off = i * kSliceRawBytes;
    size_t thisLen = (i + 1 == n) ? (sz - off) : kSliceRawBytes;
    size_t b64Len = b64EncodedLen(thisLen);
    // Allocate hdr + b64 + 1 so the trailing '\0' from b64Encode lives
    // inside the std::string buffer (avoids writing 1 byte past size()
    // into capacity slack, which is UB even when the chunk happens to
    // round-trip correctly via mpPackBin's .size() read).
    std::string chunk;
    chunk.resize((size_t)hdrLen + b64Len + 1);
    memcpy(&chunk[0], hdr, (size_t)hdrLen);
    size_t wrote = 0;
    if (!b64Encode(raw + off, thisLen,
                   &chunk[(size_t)hdrLen], b64Len + 1, &wrote)) {
      Serial.printf("[voice] packChunksFromFile: b64 encode failed (chunk %u)\n",
                    (unsigned)i);
      free(raw);
      out.clear();
      return out;
    }
    // Trim the reserved-but-unused NUL byte off the string length so
    // mpPackBin(content) ships exactly the wire bytes (header + b64),
    // not the trailing NUL. c_str() will still NUL-terminate because
    // std::string always guarantees that at position size().
    chunk.resize((size_t)hdrLen + b64Len);
    out.push_back(std::move(chunk));
    // Rewrite header for subsequent chunks with their own i.
    if (i + 1 < n) {
      hdrLen = snprintf(hdr, sizeof(hdr), "%s%s %u/%u ",
                        kPrefixChunk, id, (unsigned)(i + 2), (unsigned)n);
    }
  }
  free(raw);
  Serial.printf("[voice] packChunksFromFile %s (%u b) -> %u chunks\n",
                c2Path, (unsigned)sz, (unsigned)out.size());
  return out;
}

// Helper used by both ingest() and the legacy unpackToFile path.
static bool unpackLegacy(const std::string& content, const char* outPath) {
  if (!outPath || !outPath[0]) return false;
  if (!startsWithLegacy(content)) {
    Serial.println("[voice] unpackLegacy: bad prefix");
    return false;
  }
  const std::string b64 = content.substr(strlen(kPrefixLegacy));
  size_t maxRaw = b64DecodedMaxLen(b64);
  if (maxRaw > kMaxC2Bytes) {
    Serial.printf("[voice] unpackLegacy: oversize %u\n", (unsigned)maxRaw);
    return false;
  }
  uint8_t* raw = (uint8_t*)malloc(maxRaw);
  if (!raw) return false;
  size_t rawLen = 0;
  if (!b64Decode(b64, raw, maxRaw, &rawLen)) {
    Serial.println("[voice] unpackLegacy: b64 decode failed");
    free(raw);
    return false;
  }
  if (rawLen == 0) {
    free(raw);
    return false;
  }
  bool ok = writeC2(outPath, raw, rawLen);
  free(raw);
  return ok;
}

IngestResult ingest(const std::string& content, const char* srcHashHex,
                    char* outPath, size_t outPathLen) {
  if (outPath && outPathLen) outPath[0] = '\0';
  if (!isVoiceMemo(content)) return IngestResult::NotVoice;

  // Legacy single-message path: decode and write immediately.
  if (startsWithLegacy(content)) {
    char tmpPath[64];
    if (!buildRxPath(srcHashHex, tmpPath, sizeof(tmpPath))) {
      Serial.println("[voice] ingest legacy: buildRxPath fail");
      return IngestResult::Fail;
    }
    if (unpackLegacy(content, tmpPath)) {
      if (outPath && outPathLen) {
        size_t n = strlen(tmpPath);
        if (n + 1 > outPathLen) return IngestResult::Fail;
        memcpy(outPath, tmpPath, n + 1);
      }
      // Reset any stale chunked inflight; legacy is a separate memo.
      resetInflight();
      Serial.printf("[voice] ingest legacy: complete path=%s\n", tmpPath);
      return IngestResult::Complete;
    }
    Serial.println("[voice] ingest legacy: unpackLegacy fail");
    return IngestResult::Fail;
  }

  // Chunked path: parse "C2P <id> <i>/<n> <b64>".
  char id[9] = {0};
  size_t iVal = 0, nVal = 0;
  std::string b64;
  if (!parseChunk(content, id, iVal, nVal, b64)) {
    Serial.println("[voice] ingest: parseChunk fail");
    return IngestResult::Fail;
  }

  // If a different memo is in flight, abandon it (caller logs).
  if (s_inflight.active && memcmp(s_inflight.idHex, id, 8) != 0) {
    Serial.printf("[voice] chunk: abandoning inflight id=%s for new id=%s\n",
                  s_inflight.idHex, id);
    resetInflight();
  }

  // New memo: set up the slot.
  if (!s_inflight.active || memcmp(s_inflight.idHex, id, 8) != 0) {
    if (nVal == 0 || nVal > kMaxParts) {
      Serial.printf("[voice] ingest: n=%u out of range\n", (unsigned)nVal);
      return IngestResult::Fail;
    }
    resetInflight();
    memcpy(s_inflight.idHex, id, 8);
    s_inflight.idHex[8] = '\0';
    s_inflight.totalParts = nVal;
    s_inflight.parts.assign(nVal, std::vector<uint8_t>());
    s_inflight.got.assign(nVal, false);
    s_inflight.gotCount = 0;
    s_inflight.active = true;
  } else {
    // Same memo: validate consistency.
    if (nVal != s_inflight.totalParts) {
      Serial.printf("[voice] chunk: n mismatch %u vs %u\n",
                    (unsigned)nVal, (unsigned)s_inflight.totalParts);
      resetInflight();
      return IngestResult::Fail;
    }
  }

  const size_t idx = iVal - 1;
  if (s_inflight.got[idx]) {
    // Duplicate chunk — LXMF may redeliver after a transient retry.
    // If we already have every part on hand, fall through and treat this
    // as the completion trigger (re-emit Complete so the caller can
    // queue playback even if the original Complete packet was dropped).
    if (s_inflight.gotCount >= s_inflight.totalParts) {
      char tmpPath[64];
      if (!buildRxPath(srcHashHex, tmpPath, sizeof(tmpPath))) {
        Serial.println("[voice] ingest dup-complete: buildRxPath fail");
        return IngestResult::Fail;
      }
      // Reassemble and rewrite (idempotent — writeC2 truncates-then-writes).
      size_t totalRaw = 0;
      for (const auto& p : s_inflight.parts) totalRaw += p.size();
      std::vector<uint8_t> full;
      full.reserve(totalRaw);
      for (const auto& p : s_inflight.parts) full.insert(full.end(), p.begin(), p.end());
      if (!writeC2(tmpPath, full.data(), full.size())) {
        Serial.println("[voice] ingest dup-complete: rewrite fail");
        return IngestResult::Fail;
      }
      if (outPath && outPathLen) {
        size_t n = strlen(tmpPath);
        if (n + 1 > outPathLen) return IngestResult::Fail;
        memcpy(outPath, tmpPath, n + 1);
      }
      Serial.printf("[voice] ingest dup-complete: id=%s -> %s\n", id, tmpPath);
      return IngestResult::Complete;
    }
    Serial.printf("[voice] ingest dup i=%u id=%s got=%u/%u\n",
                  (unsigned)iVal, id,
                  (unsigned)s_inflight.gotCount,
                  (unsigned)s_inflight.totalParts);
    return IngestResult::Incomplete;
  }

  // Decode the b64 slice into a fresh vector.
  size_t sliceMax = b64DecodedMaxLen(b64);
  std::vector<uint8_t> slice(sliceMax);
  size_t sliceLen = 0;
  if (!b64Decode(b64, slice.data(), sliceMax, &sliceLen)) {
    Serial.printf("[voice] chunk: b64 decode fail (chunk %u)\n", (unsigned)iVal);
    resetInflight();
    return IngestResult::Fail;
  }
  slice.resize(sliceLen);

  // Reject slice sizes that violate the per-slice cap (any value the
  // sender could produce that wouldn't fit kChunkB64Max means somebody is
  // sending out-of-spec bytes — don't accept them).
  if (sliceLen > kSliceRawBytes) {
    Serial.printf("[voice] chunk: slice too big %u > %u\n",
                  (unsigned)sliceLen, (unsigned)kSliceRawBytes);
    resetInflight();
    return IngestResult::Fail;
  }

  // Final slice carries the trailing raw tail; previous slices are fixed
  // size. Use this to enforce the file-size cap on the wire.
  s_inflight.parts[idx] = std::move(slice);
  s_inflight.got[idx] = true;
  s_inflight.gotCount++;

  // Compute the assembled length for cap checking.
  size_t totalRaw = 0;
  for (const auto& p : s_inflight.parts) totalRaw += p.size();
  if (totalRaw > kMaxC2Bytes) {
    Serial.printf("[voice] chunk: oversize total=%u > %u\n",
                  (unsigned)totalRaw, (unsigned)kMaxC2Bytes);
    resetInflight();
    return IngestResult::Fail;
  }

  Serial.printf("[voice] ingest i=%u/%u id=%s got=%u/%u bytes=%u\n",
                (unsigned)iVal, (unsigned)nVal, id,
                (unsigned)s_inflight.gotCount,
                (unsigned)s_inflight.totalParts,
                (unsigned)totalRaw);

  if (s_inflight.gotCount < s_inflight.totalParts) {
    return IngestResult::Incomplete;
  }

  // All parts in: assemble + write.
  char tmpPath[64];
  if (!buildRxPath(srcHashHex, tmpPath, sizeof(tmpPath))) {
    Serial.println("[voice] ingest complete: buildRxPath fail");
    resetInflight();
    return IngestResult::Fail;
  }

  // Concatenate into a single contiguous buffer.
  std::vector<uint8_t> full;
  full.reserve(totalRaw);
  for (const auto& p : s_inflight.parts) full.insert(full.end(), p.begin(), p.end());

  if (full.empty()) {
    Serial.println("[voice] chunked complete: empty reassembly");
    resetInflight();
    return IngestResult::Fail;
  }

  if (!writeC2(tmpPath, full.data(), full.size())) {
    Serial.printf("[voice] chunked complete: writeC2 %s fail\n", tmpPath);
    resetInflight();
    return IngestResult::Fail;
  }

  if (outPath && outPathLen) {
    size_t n = strlen(tmpPath);
    if (n + 1 > outPathLen) {
      resetInflight();
      return IngestResult::Fail;
    }
    memcpy(outPath, tmpPath, n + 1);
  }

  Serial.printf("[voice] chunked complete: id=%s n=%u bytes=%u -> %s\n",
                id, (unsigned)s_inflight.totalParts, (unsigned)totalRaw, tmpPath);

  resetInflight();
  return IngestResult::Complete;
}

bool unpackReceived(const std::string& content, const char* srcHashHex,
                    char* outPath, size_t outPathLen) {
  IngestResult r = ingest(content, srcHashHex, outPath, outPathLen);
  return r == IngestResult::Complete;
}

}  // namespace VoiceMemo