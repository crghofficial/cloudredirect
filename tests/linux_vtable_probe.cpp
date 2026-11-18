// Read-only ABI probe. dlopen Steam's actual library in a separate process;
// discover its relocated RTTI/vtables without installing hooks or opening an account.
#include "../src/platform/linux/vtable_hook.cpp"
#include <cstdio>
#include <dlfcn.h>
#include <sys/mman.h>
extern "C" int hook_BYieldingSend(void *, const char *, void *, void *, int *) {
    return 0;
}
extern "C" int hook_NotificationDirect(void *, const char *, void *, int *) {
    return 0;
}
extern "C" int hook_SyncSend2(void *, const char *, void *, unsigned, void *, int *) {
    return 0;
}
extern "C" bool hook_IsCloudEnabledForApp(void *, unsigned) {
    return false;
}
int main(int argc, char **argv) {
    if (argc > 2)
        return 2;
    // Reject unreadable pointers and boundary-spanning reads without a signal.
    auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *area =
        mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (area == MAP_FAILED)
        return 3;
    mprotect(static_cast<char *>(area) + page, page, PROT_NONE);
    if (!CanReadMemory(area, page) || CanReadMemory(static_cast<char *>(area) + page, 1) ||
        CanReadMemory(static_cast<char *>(area) + page - 1, 2) || CanReadMemory((void *)1, 4))
        return 4;
    munmap(area, page * 2);

    // A header alone is insufficient: the three hook slots must point into
    // executable Steam mappings and exactly one valid table must match.
    uintptr_t tables[22] = {};
    const auto begin = reinterpret_cast<uintptr_t>(tables);
    constexpr uintptr_t typeinfo = 1;
    constexpr uintptr_t executable = 0x123400;
    g_readableRanges = {{begin, begin + sizeof(tables)}};
    g_executableRanges = {{executable, executable + 16}};
    tables[1] = typeinfo;
    tables[7] = tables[9] = tables[10] = executable;
    if (FindTransportTable(typeinfo, begin, sizeof(tables)) != reinterpret_cast<void**>(tables + 2))
        return 10;
    tables[10] = begin;
    if (FindTransportTable(typeinfo, begin, sizeof(tables)))
        return 11;
    tables[10] = executable;
    memcpy(tables + 11, tables, 11 * sizeof(uintptr_t));
    if (FindTransportTable(typeinfo, begin, sizeof(tables)))
        return 12;
    if (argc == 1)
        return 0;

    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 5;
    }
    size_t size = 0;
    auto base = VtableHook::FindSteamclient(size);
    if (!base)
        return 6;
    auto vt = VtableHook::FindTransportVtable(base, size);
    auto rs = VtableHook::FindRemoteStorageVtable(base, size);
    if (!vt || !rs)
        return 7;
    auto slot = ResolveCloudEnabledSlot(rs, base, size);
    if (slot == SIZE_MAX)
        return 13;
    // Cooperate with an existing SLSsteam five-byte entry detour, but reject
    // changed body instructions rather than trusting the vtable slot number.
    uint8_t method[40];
    memcpy(method, rs[slot], sizeof(method));
    method[0] = 0xe9;
    memset(method + 1, 0, 4);
    void *synthetic[1] = {method};
    if (!IsLikelyAppCloudEnabledSlot(synthetic, 0, 0, UINTPTR_MAX))
        return 8;
    method[20] ^= 1;
    if (IsLikelyAppCloudEnabledSlot(synthetic, 0, 0, UINTPTR_MAX))
        return 9;
    printf("PASS readable ranges, transport RVA=%zx, RemoteStorage RVA=%zx, cloud slot=%zu\n",
           (uintptr_t)vt - base, (uintptr_t)rs - base, slot);
    // Leave process teardown to the loader; no Steam interfaces were created.
    return 0;
}
