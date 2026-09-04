// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <unordered_map>
#include "common/logging/log.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"

namespace Core::AeroLib {

// Helper to provide stub implementations for missing functions
//
// This works by pre-compiling generic stub functions ("slots"), and then
// on lookup, setting up the nid_entry they are matched with
//
// If it runs out of stubs with name information, it will return
// a default implementation without function name details

constexpr u32 MAX_STUBS = 8192;

u64 UnresolvedStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    return 0;
}

static u64 UnknownStub() {
    LOG_ERROR(Core, "Returning zero to {}", __builtin_return_address(0));
    return 0;
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
