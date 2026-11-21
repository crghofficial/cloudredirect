#pragma once

// Linux SysV x86-64 CAppInfoCache / KeyValues resolver. These byte signatures
// describe instructions, never fixed RVAs. Reject missing/ambiguous matches.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace LinuxKv64 {
struct Range {
    uintptr_t begin, end;
    bool executable, writable;
};
// /proc/maps ends are page aligned; PT_LOAD p_memsz need not be. Keep
// the overlapping portion instead of dropping the final anonymous BSS map.
inline bool ClipRange(uintptr_t lo, uintptr_t hi, uintptr_t imageBegin, uintptr_t imageEnd,
                      bool executable, bool writable, Range &result) {
    lo = std::max(lo, imageBegin);
    hi = std::min(hi, imageEnd);
    if (lo >= hi)
        return false;
    result = {lo, hi, executable, writable};
    return true;
}
struct Addresses {
    uintptr_t engineGlobal = 0, readConfig = 0, getSection = 0;
    uintptr_t findKey = 0, setU64 = 0, setInt = 0, setString = 0;
    uint32_t cacheOffset = 0;
};
inline bool Contains(const std::vector<Range> &ranges, uintptr_t at, size_t len,
                     bool executable = false, bool writable = false) {
    if (at + len < at)
        return false;
    for (const auto &r : ranges)
        if (at >= r.begin && at + len <= r.end && (!executable || r.executable) &&
            (!writable || r.writable))
            return true;
    return false;
}
inline uintptr_t Unique(const std::vector<Range> &ranges, std::initializer_list<int> pattern) {
    uintptr_t result = 0;
    for (const auto &r : ranges) {
        if (!r.executable || r.end < r.begin || r.end - r.begin < pattern.size())
            continue;
        const auto *p = reinterpret_cast<const uint8_t *>(r.begin);
        for (size_t i = 0; i <= r.end - r.begin - pattern.size(); ++i) {
            size_t j = 0;
            for (int byte : pattern) {
                if (byte >= 0 && p[i + j] != byte)
                    break;
                ++j;
            }
            if (j == pattern.size()) {
                if (result)
                    return 0;
                result = r.begin + i;
            }
        }
    }
    return result;
}
inline uintptr_t Rel32(uintptr_t instruction, size_t displacement, size_t length) {
    int32_t d;
    std::memcpy(&d, reinterpret_cast<const void *>(instruction + displacement), 4);
    return instruction + length + d;
}
inline bool Resolve(const std::vector<Range> &ranges, Addresses &out) {
    Addresses a;
    // ReadConfigU64's tree walk fixes root/nodes/stride to +0x28/+0x40/32.
    a.findKey = Unique(ranges, {0x41, 0x57, 0x49, 0x89, 0xf7, 0x41, 0x56, 0x49, 0x89, 0xf6,
                                0x41, 0x55, 0x41, 0x54, 0x41, 0x89, 0xd4, 0x55, 0x53, 0x48,
                                0x89, 0xfb, 0x48, 0x81, 0xec, 0x48, 0x04, 0,    0});
    a.setU64 = Unique(ranges,
                      {0x48, 0x85, 0xff, 0x74, 0x2b, 0x55, 0x48, 0x89, 0xf5, 0x53, 0x48, 0x89, 0xfb,
                       0x48, 0x83, 0xec, 0x08, 0xe8, -1,   -1,   -1,   -1,   0x0f, 0xb6, 0x43, 0x13,
                       0x48, 0x89, 0x2b, 0x83, 0xe0, 0xe1, 0x83, 0xc8, 0x0e, 0x88, 0x43, 0x13});
    a.setInt =
        Unique(ranges, {0x48, 0x85, 0xff, 0x74, 0x2b, 0x55, 0x89, 0xf5, 0x53, 0x48, 0x89, 0xfb,
                        0x48, 0x83, 0xec, 0x08, 0xe8, -1,   -1,   -1,   -1,   0x0f, 0xb6, 0x43,
                        0x13, 0x89, 0x2b, 0x83, 0xe0, 0xe1, 0x83, 0xc8, 0x04, 0x88, 0x43, 0x13});
    a.setString = Unique(ranges, {0x48, 0x85, 0xff, 0x74, 0x7b, 0x41, 0x54, 0x55, 0x48, 0x89,
                                  0xf5, 0x53, 0x48, 0x89, 0xfb, 0xe8, -1,   -1,   -1,   -1,
                                  0x48, 0x85, 0xed, 0x74, 0x6f, 0x48, 0x89, 0xef});
    // Quota and maxnumfiles are read by the same CUserRemoteStorage method.
    // Its global load and cache LEA tie engine/layout to the resolved reader.
    uintptr_t engine = Unique(
        ranges, {0x4c, 0x8d, 0x3d, -1,   -1,   -1,   -1,   0x49, 0x8b, 0x07, 0x4c, 0x8d, 0xb0, -1,
                 -1,   -1,   -1,   0x4c, 0x89, 0xf7, 0xe8, -1,   -1,   -1,   -1,   0x45, 0x31, 0xc0,
                 0x89, 0xde, 0xba, 0x0a, 0,    0,    0,    0x48, 0x8d, 0x0d, -1,   -1,   -1,   -1,
                 0x4c, 0x89, 0xf7, 0x89, 0x5c, 0x24, 0x0c, 0xe8, -1,   -1,   -1,   -1});
    if (!a.findKey || !a.setU64 || !a.setInt || !a.setString || !engine)
        return false;
    a.readConfig = Rel32(engine + 49, 1, 5);
    const uint8_t readPrefix[] = {0x55, 0x53, 0x4c, 0x89, 0xc3, 0x48, 0x83, 0xec, 0x08, 0x48,
                                  0x63, 0x47, 0x28, 0x83, 0xf8, 0xff, 0x74, 0x32, 0x48, 0x8b,
                                  0x7f, 0x40, 0x41, 0x89, 0xd1, 0x48, 0x89, 0xcd};
    if (!Contains(ranges, a.readConfig, sizeof(readPrefix), true) ||
        std::memcmp(reinterpret_cast<void *>(a.readConfig), readPrefix, sizeof(readPrefix)))
        return false;
    a.engineGlobal = Rel32(engine, 3, 7);
    std::memcpy(&a.cacheOffset, reinterpret_cast<const void *>(engine + 13), 4);
    if (a.cacheOffset != 0x13d8 || !Contains(ranges, a.engineGlobal, sizeof(void *), false, true) ||
        Rel32(engine + 49, 1, 5) != a.readConfig)
        return false;
    uintptr_t quotaName = Rel32(engine + 35, 3, 7);
    if (!Contains(ranges, quotaName, 6) ||
        std::memcmp(reinterpret_cast<void *>(quotaName), "quota", 6))
        return false;
    // ReadConfigU64's first direct call is GetSection. The key reader's first
    // subsequent call reaches the read-only FindKey wrapper, which tail-jumps
    // into the same 4-argument FindKey used for mutations.
    if (!Contains(ranges, a.readConfig, 0x99, true) ||
        *reinterpret_cast<uint8_t *>(a.readConfig + 0x70) != 0xe8 ||
        *reinterpret_cast<uint8_t *>(a.readConfig + 0x80) != 0xe8)
        return false;
    a.getSection = Rel32(a.readConfig + 0x70, 1, 5);
    if (!Contains(ranges, a.getSection, 30, true))
        return false;
    const uint8_t sectionPrefix[] = {0x83, 0xfe, 0x07, 0x7f, 0x2b, 0x83, 0xfe, 0x01};
    if (std::memcmp(reinterpret_cast<void *>(a.getSection), sectionPrefix, sizeof(sectionPrefix)))
        return false;
    uintptr_t findWrapper = Rel32(a.readConfig + 0x80, 1, 5);
    if (!Contains(ranges, findWrapper, 9, true))
        return false;
    const uint8_t wrapperPrefix[] = {0x31, 0xc9, 0x31, 0xd2, 0xe9};
    if (std::memcmp(reinterpret_cast<void *>(findWrapper), wrapperPrefix, sizeof(wrapperPrefix)) ||
        Rel32(findWrapper + 4, 1, 5) != a.findKey)
        return false;
    out = a;
    return true;
}
} // namespace LinuxKv64
