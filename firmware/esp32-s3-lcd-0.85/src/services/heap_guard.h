#pragma once

#include <cstdint>

// Heap corruption locator.
//
// The Arduino ESP32 libraries are built with CONFIG_HEAP_POISONING_LIGHT, so every allocation
// carries head/tail canaries. Those canaries are only verified when a block is freed, which is
// why an overflowing write surfaces far away from its origin — in our case as
// "assert failed: pbuf_free ... (pbuf_free: p->ref > 0)" inside the lwIP tcpip task.
//
// This guard verifies the whole heap on a timer and at explicit checkpoints, and aborts on the
// first failure, so the panic lands close to the write that caused it. Phase() records what the
// firmware was doing, and the failure report names that phase.
#define CONSOLE_HEAP_GUARD 0  // diagnostic; flip to 1 to re-arm the heap verifier

// Diagnostic kill switches for the lwIP "pbuf_free: p->ref > 0" panic. Both must be 0 for a
// normal build; each one silences one source of TCP/UDP traffic so the trigger can be isolated.
#define CONSOLE_DIAG_NO_POLL 0   // Skip the console sync poll (keeps the pairing token).
#define CONSOLE_DIAG_NO_MDNS 0   // Skip mDNS announce/response.

namespace HeapGuard {

#if CONSOLE_HEAP_GUARD
// Starts the background verifier. Safe to call once, after Serial is up.
void Begin(uint32_t interval_ms = 200);
// Records the current activity. `tag` must outlive the call (use string literals).
void Phase(const char* tag);
// Verifies the heap now. Returns true when intact; aborts the firmware when not.
bool Check(const char* where);
#else
inline void Begin(uint32_t = 200) {}
inline void Phase(const char*) {}
inline bool Check(const char*) { return true; }
#endif

}  // namespace HeapGuard
