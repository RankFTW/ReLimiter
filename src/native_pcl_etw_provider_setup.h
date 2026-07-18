#pragma once

#include <Windows.h>

#include <cstdint>
#include <vector>

struct PMTraceConsumer;
struct PMTraceSession;

ULONG NativePclEtw_StartModernFilteredProviders(
    PMTraceSession& session,
    PMTraceConsumer& consumer) noexcept;

ULONG NativePclEtw_EnableFilteredProvider(
    PMTraceSession& session,
    const GUID& provider_guid,
    const std::vector<uint16_t>& event_ids,
    uint64_t any_keyword_mask,
    uint64_t all_keyword_mask,
    uint8_t maximum_level) noexcept;

ULONG NativePclEtw_ConfigureNvidiaPclProcessScope(
    PMTraceSession& session,
    uint32_t process_id) noexcept;
