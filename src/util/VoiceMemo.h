#pragma once

// LXMF Codec2 voice memo wire format — Plus side.
//
// Wire layout (must match Pro exactly so cross-device playback works):
//
//   Legacy (one LXMF message carries the whole memo):
//     content = "C2 " + base64(entire .c2 file bytes, including C2V1 header)
//
//   Chunked (each LXMF message carries one slice, base64-encoded per slice):
//     content = "C2P " + <id8hex> + " " + <i>/<n> + " " + <base64-slice>
//     where  id8hex : 8 lowercase hex chars identifying the memo
//            i      : 1-based slice index
//            n      : total slices, n <= kMaxParts
//            base64-slice : RFC 4648 standard b64 of a raw sub-range of .c2
//
//   The split is applied to the RAW .c2 bytes — each slice is independently
//   base64-encoded. Decoders do not re-base64 the whole file before slicing.
//
//   Send side uses chunked; receive accepts both legacy and chunked.
//
// Display mapping (chats/previews/bubbles):
//   any content starting with kPrefixLegacy or kPrefixChunk renders as
//   kDisplay ("[voice memo]").
//
// File IO uses Arduino SD.h directly (Plus side has no SdCard facade), the
// way Codec2Voice.cpp already does.

#include <Arduino.h>
#include <string>
#include <vector>

namespace VoiceMemo {

// Wire prefixes. Anything starting with either is treated as a voice memo.
static constexpr const char* kPrefixLegacy = "C2 ";
static constexpr const char* kPrefixChunk  = "C2P ";

// Human-readable placeholder shown in chat lists / message bubbles.
static constexpr const char* kDisplay = "[voice memo]";

// Inbound .c2 destination directory.
static constexpr const char* kRxDir = "/Files/voice";

// Hard cap on inbound .c2 size (header + packed frames). Codec2 @ 1300 bps
// gives ~162 B/s, so 4096 B ≈ 25 s — plenty for a memo, small enough to
// keep the whole base64 round-trip in RAM on a chat bubble. The chunked
// packer additionally clamps to kMaxParts * kSliceRawBytes so all chunks
// stay under kChunkB64Max; legacy single-packet receives are limited to
// this same value.
static constexpr size_t kMaxC2Bytes = 4096;

// Maximum number of chunked parts a single memo can be split into.
// With kSliceRawBytes=90 that gives 16 * 90 = 1440 raw bytes total —
// comfortably under kMaxC2Bytes.
static constexpr size_t kMaxParts = 16;

// Maximum base64 chars produced per chunk (drives the raw slice size —
// sliceRaw = (kChunkB64Max * 3) / 4). 120 b64 == 90 raw bytes per slice.
// 90 is a multiple of 3 so b64 always emits clean groups (no mid-slice
// padding) and keeps a fully-encoded chunk inside the RNode single-frame
// budget after RNS+LXMF+msgpack wrapping.
static constexpr size_t kChunkB64Max = 120;

// Derived slice size (raw bytes per chunk). Kept as a constant so the
// ingest reassembler and the packer agree on layout.
static constexpr size_t kSliceRawBytes = (kChunkB64Max * 3) / 4;

// True if `content` starts with either the legacy or chunked voice-memo
// prefix. UI / previews use this to decide whether to render [voice memo].
bool isVoiceMemo(const std::string& content);

// The placeholder shown to the user. Constant today; lives behind a
// function so the UI layer doesn't need to know the literal.
const char* displayText();

// Read `c2Path` from SD, split the raw .c2 bytes into kSliceRawBytes-wide
// slices, base64-encode each slice independently, and prepend
// "C2P <id8hex> <i>/<n> " per chunk. Returns an empty vector on any
// failure (missing, oversize, SD read error, malloc fail, etc.). The
// returned vector's size == number of chunks (1..kMaxParts).
std::vector<std::string> packChunksFromFile(const char* c2Path);

// Result of feeding one inbound LXMF message into the reassembler.
enum class IngestResult {
  NotVoice,    // content does not look like a voice memo
  Incomplete,  // chunk accepted but not all parts received yet
  Complete,    // all parts now received; outPath holds the .c2 file path
  Fail,        // malformed chunk, decode error, oversize, write failure, etc.
};

// Feed one inbound LXMF `content` into the single-flight reassembler.
//   - Legacy "C2 ..." payloads are written directly and return Complete.
//   - Chunked "C2P ..." payloads are accumulated; on the final chunk the
//     fully reassembled .c2 is written and Complete is returned with
//     outPath populated.
//   - Incomplete is returned when a chunk has been accepted but more
//     parts are still pending (outPath is not touched in this case).
//   - Fail is returned for any hard error (bad prefix, malformed chunk,
//     b64 decode error, oversize, write failure, abandoned inflight).
// `srcHashHex` is msg.sourceHash.toHex() and is used to derive the
// output filename (rx_<first8>.c2) when Complete.
IngestResult ingest(const std::string& content, const char* srcHashHex,
                    char* outPath, size_t outPathLen);

// Convenience wrapper for legacy callers: returns true iff ingest()
// returns Complete. Equivalent to `ingest(...) == IngestResult::Complete`.
bool unpackReceived(const std::string& content, const char* srcHashHex,
                    char* outPath, size_t outPathLen);

}  // namespace VoiceMemo