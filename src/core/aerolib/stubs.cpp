// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include "common/logging/log.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/libraries/pad/pad_errors.h"

namespace Core::AeroLib {

// Helper to provide stub implementations for missing functions
//
// This works by pre-compiling generic stub functions ("slots"), and then
// on lookup, setting up the nid_entry they are matched with
//
// If it runs out of stubs with name information, it will return
// a default implementation without function name details

constexpr u32 MAX_STUBS = 8192;

// A stub is by definition unimplemented functionality returning a fixed value (0) - if the
// game polls it in a loop waiting for that value to change (a real pattern seen with GT7:
// hundreds of thousands of consecutive sceDeviceServiceGetEventState calls/sec, pinning a
// CPU core at 100% while nothing else progresses), there is no way to "fix" that generically
// without knowing the real semantics of each specific function. What we CAN do safely,
// without touching behavior at all: cap how fast any one thread can hammer stubs, so a
// runaway poll loop degrades into a bounded, sustainable spin instead of a 100%-CPU burn
// that has repeatedly pushed this machine toward OOM this session. Only kicks in once a
// thread has already made many rapid consecutive stub calls, so a normal one-off call to an
// unimplemented function never pays for it.
static thread_local u32 stub_call_counter = 0;

static void ThrottleIfHot() {
    if ((++stub_call_counter & 0x3F) == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

u64 UnresolvedStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    ThrottleIfHot();
    return 0;
}

static u64 UnknownStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    ThrottleIfHot();
    return 0;
}

// sceDeviceServiceGetEventState (nid 9ddRUOV8Q5A): the generic stub above always returns 0
// (ORBIS_OK), which every Get*Event-style PS4 API treats as "an event was dequeued, go
// process it" (confirmed by the real implementation of sceUserServiceGetEvent, which
// returns ORBIS_OK only when it actually popped an event and ORBIS_USER_SERVICE_ERROR_NO_EVENT
// otherwise). Nothing in shadPS4 ever generates a device-service event, so a caller that
// polls this in a loop expecting *some* terminal "no event" reply spins forever - observed
// with GT7 as hundreds of thousands of consecutive calls/sec on one thread. Special-cased
// here (instead of building out a whole DeviceService library module for a single function)
// to consistently report "no event", matching the idiom without guessing at what "an event"
// would even contain since nothing produces one. See GT7-NOTES.md.
static u64 DeviceServiceGetEventStateStub() {
    LOG_ERROR(Core, "Stub: sceDeviceServiceGetEventState (special-cased), returning "
                    "ORBIS_DEVICE_SERVICE_ERROR_NO_EVENT to {}",
              __builtin_return_address(0));
    return static_cast<u64>(static_cast<u32>(ORBIS_DEVICE_SERVICE_ERROR_NO_EVENT));
}

static const NidEntry* stub_nids[MAX_STUBS];
static std::string stub_nids_unknown[MAX_STUBS];

static u64 CommonStub(int stub_index, void* addr) {
    auto entry = stub_nids[stub_index];
    if (entry) {
        LOG_ERROR(Core, "Stub: {} (nid: {}) called, returning zero to {}", entry->name, entry->nid,
                  addr);
    } else {
        LOG_ERROR(Core, "Stub: Unknown (nid: {}) called, returning zero to {}",
                  stub_nids_unknown[stub_index], addr);
    }
    ThrottleIfHot();
    return 0;
}

template <int stub_index>
static u64 CommonStubTemplate() {
    return CommonStub(stub_index, __builtin_return_address(0));
}

template <size_t... Is>
consteval auto MakeStubArray(std::index_sequence<Is...>) {
    return std::array<u64 (*)(), sizeof...(Is)>{&CommonStubTemplate<Is>...};
}

constexpr auto stub_handlers = MakeStubArray(std::make_index_sequence<MAX_STUBS>{});
static u32 UsedStubEntries;

// GetStub used to hand out a fresh slot from the fixed-size stub_handlers pool on EVERY
// call, even for a nid it had already resolved before. Games that dynamically load/relink
// the same modules more than once (GT7 does this heavily) burn through MAX_STUBS on
// duplicate nids alone, so real, previously-unseen imports start silently falling into the
// nameless UnknownStub() fallback (no nid, no name - just "Returning zero to <addr>") long
// before the pool is actually full of *distinct* missing functions. Cache by nid so a
// repeat request reuses the same slot/address instead of consuming a new one.
static std::unordered_map<std::string, u64> nid_to_stub;

u64 GetStub(const char* nid) {
    if (const auto it = nid_to_stub.find(nid); it != nid_to_stub.end()) {
        return it->second;
    }

    // See DeviceServiceGetEventStateStub() above for why this one nid is special-cased.
    if (std::string_view(nid) == "9ddRUOV8Q5A") {
        const u64 stub_addr = (u64)&DeviceServiceGetEventStateStub;
        nid_to_stub.emplace(nid, stub_addr);
        return stub_addr;
    }

    if (UsedStubEntries >= MAX_STUBS) {
        return (u64)&UnknownStub;
    }

    const auto entry = FindByNid(nid);
    if (!entry) {
        stub_nids_unknown[UsedStubEntries] = nid;
    } else {
        stub_nids[UsedStubEntries] = entry;
    }

    const u64 stub_addr = (u64)stub_handlers[UsedStubEntries++];
    nid_to_stub.emplace(nid, stub_addr);
    return stub_addr;
}

} // namespace Core::AeroLib
