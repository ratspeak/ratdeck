#include "CoreSync.h"
#include <string.h>

namespace CoreSync {

NetStatus netStatus;

// Toast bridge: a single-slot mailbox. Producers (any core) write the message
// under a short critical section and bump toastSeq; the UI consumer reads it.
namespace {
    char     s_toastMsg[64] = {0};
    uint32_t s_toastDur = 0;
#if RSDECK_UI_CORE_SPLIT
    portMUX_TYPE s_toastMux = portMUX_INITIALIZER_UNLOCKED;
#endif
    uint32_t s_lastToastSeen = 0;
}

void requestToast(const char* msg, uint32_t durationMs) {
    if (!msg) return;
#if RSDECK_UI_CORE_SPLIT
    portENTER_CRITICAL(&s_toastMux);
#endif
    strlcpy(s_toastMsg, msg, sizeof(s_toastMsg));
    s_toastDur = durationMs;
    netStatus.toastSeq.fetch_add(1);
#if RSDECK_UI_CORE_SPLIT
    portEXIT_CRITICAL(&s_toastMux);
#endif
}

uint32_t takePendingToast(char* out, uint32_t outSize) {
    uint32_t dur = 0;
#if RSDECK_UI_CORE_SPLIT
    portENTER_CRITICAL(&s_toastMux);
#endif
    uint32_t seq = netStatus.toastSeq.load();
    if (seq != s_lastToastSeen) {
        s_lastToastSeen = seq;
        strlcpy(out, s_toastMsg, outSize);
        dur = s_toastDur == 0 ? 1 : s_toastDur;
    }
#if RSDECK_UI_CORE_SPLIT
    portEXIT_CRITICAL(&s_toastMux);
#endif
    return dur;
}

#if RSDECK_UI_CORE_SPLIT

SemaphoreHandle_t spiBusMutex = nullptr;
SemaphoreHandle_t rnsMutex = nullptr;

bool begin() {
    if (!spiBusMutex) spiBusMutex = xSemaphoreCreateRecursiveMutex();
    if (!rnsMutex)    rnsMutex = xSemaphoreCreateRecursiveMutex();
    return spiBusMutex && rnsMutex;
}

#else

bool begin() { return true; }

#endif

}  // namespace CoreSync
