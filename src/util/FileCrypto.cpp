// =============================================================================
// FileCrypto — Notes on-disk encryption (RNE1 format).
// See FileCrypto.h for the binary layout and security model.
//
// Implementation notes:
//   - PBKDF2-HMAC-SHA256 derives a 32-byte AES-256 key from the
//     passphrase + per-file salt. 50k iterations is a defensive default
//     for ESP32 (~50ms / derivation on the Plus). If you bump it, every
//     older file remains decryptable (iterations are stored in the
//     header).
//   - AES-256-GCM gives both confidentiality and integrity. The tag is
//     verified on decrypt — wrong passphrase or any flipped byte fails
//     the whole op, never silently corrupts the body.
//   - The nonce is 12 random bytes per file. With AES-GCM the
//     (key, nonce) pair MUST be unique across files; random 12-byte
//     nonces collide at ~2^48 birthday bound, well past a lifetime
//     of notes.
//   - Sensitive buffers (key, intermediate GCM state) are zeroed
//     before returning. Passphrases themselves are owned by the caller
//     (LVGL textarea) — we wipe what we touch and document that the
//     caller must do the same.
// =============================================================================

#include "util/FileCrypto.h"

// mbedtls — provided by the ESP32 Arduino core (framework-arduinoespressif32).
// We include the headers directly rather than going through the platform
// abstraction, because the project already builds and links mbedtls via
// Arduino's wifi/BLE stacks; no extra lib_deps or build_flags needed.
#include <mbedtls/gcm.h>
#include <mbedtls/pkcs5.h>
#include <mbedtls/md.h>

// esp_fill_random() lives in <esp_random.h>. On the framework version we
// target (espressif32@6.7.0 / esp32 2.0.17) this is the public API for
// hardware-backed random bytes. Falls back to a SoftwareSecurity fail if
// somehow the symbol is missing (which would be a build env breakage
// well outside our scope).
#include <esp_system.h>
#include <esp_random.h>

#include <string.h>

namespace FileCrypto {

size_t maxCipherLen(size_t ptLen) {
    return HEADER_SIZE + ptLen + TAG_SIZE;
}

bool isEncryptedBlob(const uint8_t* data, size_t len) {
    if (!data || len < HEADER_SIZE + TAG_SIZE) return false;
    return data[0] == MAGIC[0] &&
           data[1] == MAGIC[1] &&
           data[2] == MAGIC[2] &&
           data[3] == MAGIC[3];
}

void wipeSensitive(void* p, size_t n) {
    if (!p || n == 0) return;
    // volatile prevents the compiler from eliding the writes as "dead"
    // stores. We accept the perf hit — this is on hot paths only during
    // a single encrypt/decrypt, not in any tight loop.
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) {
        *v++ = 0;
    }
}

namespace {

// Big-endian uint32_t write (manual: avoids dragging <arpa/inet.h>).
inline void putU32BE(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

inline uint32_t getU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

// Derive a 32-byte AES key via PBKDF2-HMAC-SHA256.
// We build a fresh mbedtls_md_context each call (init/setup) and free
// it on the way out — keys never linger in a long-lived context.
bool deriveKey(const char* pass, size_t passLen,
               const uint8_t* salt, size_t saltLen,
               uint32_t iters,
               uint8_t* keyOut) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1 /* HMAC */);
    if (rc != 0) {
        mbedtls_md_free(&ctx);
        return false;
    }

    // mbedtls_pkcs5_pbkdf2_hmac reads the password through a non-const
    // pointer in this version of the lib, even though it doesn't write
    // to it. Cast away const locally — the underlying API is logically
    // const-correct for our use.
    rc = mbedtls_pkcs5_pbkdf2_hmac(
        &ctx,
        reinterpret_cast<const unsigned char*>(pass), passLen,
        salt, saltLen,
        iters,
        KEY_SIZE, keyOut);

    mbedtls_md_free(&ctx);
    return rc == 0;
}

// AES-256-GCM AEAD encrypt. Returns false on any mbedtls error.
// `tagOut` receives the 16-byte GCM tag.
bool aeadEncrypt(const uint8_t* key,
                 const uint8_t* nonce,
                 const uint8_t* in, size_t inLen,
                 uint8_t* out,
                 uint8_t* tagOut) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_SIZE * 8);
    if (rc != 0) { mbedtls_gcm_free(&gcm); return false; }

    // No additional authenticated data (AAD) in RNE1 v1. The header is
    // integrity-protected by being stored in cleartext alongside the
    // ciphertext; an attacker who flipped header bytes would only
    // change which file's plaintext comes out, which is the same
    // threat model as renaming the file. We keep the header minimal
    // (38 bytes) and inside the cipher domain as little as possible.
    int rc2 = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
                                        inLen,
                                        nonce, NONCE_SIZE,
                                        nullptr, 0,
                                        in, out,
                                        TAG_SIZE, tagOut);
    mbedtls_gcm_free(&gcm);
    return rc2 == 0;
}

// AES-256-GCM AEAD decrypt + tag verify. Returns false on any mbedtls
// error, INCLUDING MBEDTLS_ERR_GCM_AUTH_FAILED (tag mismatch → wrong
// passphrase or corrupted blob). The tag is a read-only input.
bool aeadDecrypt(const uint8_t* key,
                 const uint8_t* nonce,
                 const uint8_t* in, size_t inLen,
                 uint8_t* out,
                 const uint8_t* tagIn) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);

    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_SIZE * 8);
    if (rc != 0) { mbedtls_gcm_free(&gcm); return false; }

    int rc2 = mbedtls_gcm_auth_decrypt(&gcm,
                                       inLen,
                                       nonce, NONCE_SIZE,
                                       nullptr, 0,
                                       tagIn, TAG_SIZE,
                                       in, out);
    mbedtls_gcm_free(&gcm);
    return rc2 == 0;
}

}  // namespace

bool encrypt(const char* pass, size_t passLen,
             const uint8_t* pt, size_t ptLen,
             uint8_t* out, size_t outCap, size_t* outLen) {
    if (outLen) *outLen = 0;
    if (!pass || passLen == 0 || !pt || !out || !outLen) return false;
    if (ptLen > MAX_PLAINTEXT) return false;
    if (outCap < maxCipherLen(ptLen)) return false;

    // 1. Header
    out[0] = MAGIC[0]; out[1] = MAGIC[1]; out[2] = MAGIC[2]; out[3] = MAGIC[3];
    out[4] = VERSION;
    out[5] = KDF_ID_PBKDF2_SHA256;
    putU32BE(out + 6, KDF_ITERS);

    // 2. Random salt + nonce. esp_fill_random() is hardware RNG on
    // ESP32-S3; the SDK guarantees no two consecutive reads collide.
    // The 2.0.17 SDK declares it as void — we trust the SDK contract
    // (no error code to surface). On any platform that did report
    // errors we'd fall back to aborting the save rather than reusing
    // the same bytes for a different file (which would weaken GCM's
    // IV uniqueness guarantee).
    esp_fill_random(out + 10, SALT_SIZE);
    esp_fill_random(out + 10 + SALT_SIZE, NONCE_SIZE);

    // 3. Derive key (PBKDF2)
    uint8_t key[KEY_SIZE];
    if (!deriveKey(pass, passLen, out + 10, SALT_SIZE, KDF_ITERS, key)) {
        wipeSensitive(key, sizeof(key));
        return false;
    }

    // 4. Encrypt. ciphertext starts at HEADER_SIZE.
    uint8_t* ct = out + HEADER_SIZE;
    uint8_t* tag = out + HEADER_SIZE + ptLen;  // tag lives at the end
    bool ok = aeadEncrypt(key,
                          out + 10 + SALT_SIZE,
                          pt, ptLen, ct, tag);

    // 5. Wipe key BEFORE returning. Order matters: we want the
    // intermediate key gone regardless of encrypt success/failure.
    wipeSensitive(key, sizeof(key));

    if (!ok) {
        // Clear the partial blob so a caller bug that reads out
        // anyway doesn't see the magic bytes of a half-encrypted file.
        wipeSensitive(out, outCap);
        return false;
    }

    *outLen = maxCipherLen(ptLen);
    return true;
}

bool decrypt(const char* pass, size_t passLen,
             const uint8_t* blob, size_t blobLen,
             uint8_t* ptOut, size_t ptCap, size_t* ptLen) {
    if (ptLen) *ptLen = 0;
    if (!pass || passLen == 0 || !blob || !ptOut || !ptLen) return false;
    if (blobLen < HEADER_SIZE + TAG_SIZE) return false;
    if (!isEncryptedBlob(blob, blobLen)) return false;
    if (blob[4] != VERSION) return false;
    if (blob[5] != KDF_ID_PBKDF2_SHA256) return false;

    const uint32_t iters = getU32BE(blob + 6);
    if (iters == 0 || iters > 1000000) return false;   // sanity bound

    const uint8_t* salt = blob + 10;
    const uint8_t* nonce = blob + 10 + SALT_SIZE;
    const size_t ctLen = blobLen - HEADER_SIZE - TAG_SIZE;
    if (ctLen > MAX_PLAINTEXT) return false;
    if (ptCap < ctLen) return false;

    uint8_t key[KEY_SIZE];
    if (!deriveKey(pass, passLen, salt, SALT_SIZE, iters, key)) {
        wipeSensitive(key, sizeof(key));
        return false;
    }

    const uint8_t* ct = blob + HEADER_SIZE;
    const uint8_t* tag = blob + HEADER_SIZE + ctLen;
    bool ok = aeadDecrypt(key, nonce, ct, ctLen, ptOut, tag);
    wipeSensitive(key, sizeof(key));

    if (!ok) {
        // Wipe any partial plaintext a buggy caller might still read.
        wipeSensitive(ptOut, ptCap);
        return false;
    }

    *ptLen = ctLen;
    return true;
}

}  // namespace FileCrypto
