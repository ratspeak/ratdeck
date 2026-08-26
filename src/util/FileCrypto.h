#pragma once

// =============================================================================
// FileCrypto — Notes on-disk encryption (RNE1 format).
//
// File layout (binary, big-endian except where noted):
//   magic[4]   = 'R','N','E','1'       4 bytes
//   version    = 1                     1 byte
//   kdf_id     = 1  (PBKDF2-HMAC-SHA256)  1 byte
//   iterations = 50000                 4 bytes (big-endian)
//   salt[16]   random (esp_fill_random)  16 bytes
//   nonce[12]  random (esp_fill_random)  12 bytes
//   ciphertext[pt_len]                 pt_len bytes
//   tag[16]    AES-256-GCM auth tag    16 bytes
//
// HEADER_SIZE = 38. Total file size = 38 + pt_len + 16.
//
// The plaintext is NEVER written to disk in any intermediate form. The
// ciphertext blob is built entirely in RAM, then handed to
// SDStore::writeAtomic() which performs a .tmp -> rename dance. The
// .tmp / .bak files on the SD only ever contain the encrypted blob
// (no cleartext), so a partial save or rollback cannot leak the body.
//
// Passphrase and plaintext buffers are wiped with FileCrypto::wipeSensitive()
// on the way out of encrypt()/decrypt(). No passphrase or body bytes are
// ever logged via Serial.
//
// Implementation uses mbedtls (bundled with ESP32 Arduino core): GCM,
// PBKDF2-HMAC-SHA256. This keeps Pro/rsDeck parity: both devices can
// open each other's .note.enc files via a shared key/passphrase.
// =============================================================================

#include <stddef.h>
#include <stdint.h>

namespace FileCrypto {

// Magic header bytes, in order. Public for tests / diagnostics.
constexpr uint8_t MAGIC[4]    = {'R', 'N', 'E', '1'};
constexpr uint8_t VERSION     = 1;
constexpr uint8_t KDF_ID_PBKDF2_SHA256 = 1;

// Sizes
static constexpr size_t HEADER_SIZE = 38;   // magic(4)+ver(1)+kdf(1)+iters(4)+salt(16)+nonce(12)
static constexpr size_t TAG_SIZE    = 16;
static constexpr size_t SALT_SIZE   = 16;
static constexpr size_t NONCE_SIZE  = 12;
static constexpr size_t KEY_SIZE    = 32;   // AES-256
static constexpr uint32_t KDF_ITERS = 50000;

// Plaintext body cap (matches LvNotesEditScreen::MAX_BODY_LEN). The
// ciphertext blob for a note is therefore bounded by
// HEADER_SIZE + MAX_BODY_LEN + TAG_SIZE = 4150 bytes.
static constexpr size_t MAX_PLAINTEXT = 4096;

// Returns the exact ciphertext blob size for a plaintext of `ptLen`.
//   = HEADER_SIZE + ptLen + TAG_SIZE
size_t maxCipherLen(size_t ptLen);

// True if `data` looks like an RNE1 blob (magic + version match and
// length is at least HEADER_SIZE + TAG_SIZE). Used by the list screen
// to skip "looks like a binary" files that aren't ours.
bool isEncryptedBlob(const uint8_t* data, size_t len);

// Encrypt `ptLen` bytes of plaintext into `out`. `outCap` must be at
// least maxCipherLen(ptLen). Writes the exact blob length to *outLen.
//
// Returns false on:
//   - null pointers, empty passphrase, ptLen > MAX_PLAINTEXT
//   - outCap insufficient
//   - random / KDF / GCM failure
//
// On failure, sensitive buffers (key, intermediate GCM state) are wiped.
// On success, `out` holds the RNE1 blob and *outLen is set.
bool encrypt(const char* pass, size_t passLen,
             const uint8_t* pt, size_t ptLen,
             uint8_t* out, size_t outCap, size_t* outLen);

// Decrypt an RNE1 blob. `ptCap` must be at least MAX_PLAINTEXT.
//
// Returns false on:
//   - header / magic / version mismatch (not our format)
//   - blob shorter than HEADER_SIZE + TAG_SIZE
//   - ptCap insufficient for the (unknown) plaintext length
//   - GCM auth tag mismatch (wrong passphrase or corrupted file)
//
// On success, plaintext (decrypted bytes) is written into `ptOut`
// and *ptLen is set. The buffer is NOT NUL-terminated.
bool decrypt(const char* pass, size_t passLen,
             const uint8_t* blob, size_t blobLen,
             uint8_t* ptOut, size_t ptCap, size_t* ptLen);

// Best-effort memory wipe. Used to clear passphrases and intermediate
// key buffers. Implementation does NOT depend on volatile semantics —
// it just walks the buffer and zeroes every byte. Callers should
// still aim to keep passphrases in stack buffers (not heap) and out
// of long-lived globals.
void wipeSensitive(void* p, size_t n);

}  // namespace FileCrypto
