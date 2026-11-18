#include "vtable_hook.h"
#include "log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <mutex>
#include <vector>
#include <elf.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

// RTTI typestring for CClientUnifiedServiceTransport (Itanium ABI mangled)
static constexpr const char RTTI_NAME[] = "30CClientUnifiedServiceTransport";

// Steam's loader hides libraries from dl_iterate_phdr; parse /proc/self/maps.

struct MapRange { uintptr_t start; uintptr_t end; };
static std::vector<MapRange> g_readableRanges;
static std::vector<MapRange> g_writableRanges;
#if defined(__x86_64__)
static std::vector<MapRange> g_executableRanges;
#endif

// Used before the Steam mapping inventory has been populated.
static bool CanReadMemory(const void* addr, size_t len);

// Walk backward page-by-page to find ELF magic (true load base).
// Labeled-min may be offset into the file due to split PT_LOAD mappings.
static uintptr_t FindElfBaseBackward(uintptr_t hint)
{
    const long pageSize = sysconf(_SC_PAGESIZE);
    const uintptr_t pageMask = ~(uintptr_t)(pageSize - 1);
    uintptr_t page = hint & pageMask;
    // Cap at 64 MiB backward to avoid sweeping into adjacent modules.
    const uintptr_t limit = (page > (64ULL * 1024 * 1024)) ? page - (64ULL * 1024 * 1024) : 0;
    while (page >= limit && page != 0)
    {
        if (CanReadMemory(reinterpret_cast<void*>(page), 4))
        {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(page);
            if (p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F')
                return page;
        }
        if (page < (uintptr_t)pageSize) break;
        page -= pageSize;
    }
    return 0;
}

uintptr_t VtableHook::FindSteamclient(size_t& outSize)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f)
    {
        Log::Error("cannot open /proc/self/maps");
        return 0;
    }

    uintptr_t labeledMin = 0;
    uintptr_t labeledMax = 0;
    g_readableRanges.clear();
    g_writableRanges.clear();
#if defined(__x86_64__)
    g_executableRanges.clear();
#endif
    char line[512];

    while (fgets(line, sizeof(line), f))
    {
        uintptr_t start_addr, end_addr;
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start_addr, &end_addr) < 2)
            continue;
        if (strstr(line, "steamclient.so"))
        {
            if (labeledMin == 0 || start_addr < labeledMin)
                labeledMin = start_addr;
            if (end_addr > labeledMax)
                labeledMax = end_addr;
        }
    }

    if (labeledMin == 0)
    {
        fclose(f);
        Log::Error("steamclient.so not found in /proc/self/maps");
        return 0;
    }

    // Anchor base on ELF magic to correct for labeled-min offset.
    uintptr_t base = FindElfBaseBackward(labeledMin);
    if (base == 0)
    {
        Log::Warn("ELF magic not found backward from labeled steamclient.so start %p, using labeled min",
                  (void*)labeledMin);
        base = labeledMin;
    }
    else if (base != labeledMin)
    {
        Log::Info("corrected steamclient.so base from labeled %p to ELF-magic %p (gap=0x%zx)",
                  (void*)labeledMin, (void*)base, labeledMin - base);
    }

#if defined(__x86_64__)
    // The final PT_LOAD contains anonymous .bss past the last file-labelled
    // mapping. Protobuf default instances and the engine global live there.
    if (CanReadMemory(reinterpret_cast<void*>(base), sizeof(Elf64_Ehdr))) {
        const auto* eh = reinterpret_cast<const Elf64_Ehdr*>(base);
        if (eh->e_ident[EI_CLASS] == ELFCLASS64 && eh->e_phentsize == sizeof(Elf64_Phdr) &&
            eh->e_phnum < 128 && eh->e_phoff < 0x10000 &&
            CanReadMemory(reinterpret_cast<void*>(base + eh->e_phoff), eh->e_phnum * sizeof(Elf64_Phdr))) {
            const auto* ph = reinterpret_cast<const Elf64_Phdr*>(base + eh->e_phoff);
            for (size_t i = 0; i < eh->e_phnum; ++i)
                if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < 0x40000000 && ph[i].p_memsz < 0x40000000 &&
                    base + ph[i].p_vaddr + ph[i].p_memsz > labeledMax)
                    labeledMax = base + ph[i].p_vaddr + ph[i].p_memsz;
        }
    }
#endif

    // Capture readable mappings within +/-16 MiB of labeled span.
    // .data.rel.ro may be in anonymous mappings outside the labeled range.
    // Slot-pointer validation still uses the tight labeled span.
    const uintptr_t adjacency = 16ULL * 1024 * 1024;
    const uintptr_t scanLo = (base > adjacency) ? base - adjacency : 0;
    const uintptr_t scanHi = labeledMax + adjacency;

    rewind(f);
    while (fgets(line, sizeof(line), f))
    {
        uintptr_t start_addr, end_addr;
        char perms[5] = {};
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start_addr, &end_addr, perms) < 3)
            continue;

        if (end_addr <= scanLo || start_addr >= scanHi) continue;

#if defined(__x86_64__)
        if (perms[2] == 'x' && start_addr < labeledMax && end_addr > base)
            g_executableRanges.push_back({std::max(start_addr, base), std::min(end_addr, labeledMax)});
#endif
        if (perms[0] == 'r')
        {
            g_readableRanges.push_back({start_addr, end_addr});
        }
        if (perms[0] == 'r' && perms[1] == 'w')
        {
            g_writableRanges.push_back({start_addr, end_addr});
        }
    }
    fclose(f);

    outSize = labeledMax - base;
    Log::Info("steamclient.so base=%p end=%p size=0x%zx (%zu readable, %zu writable ranges)",
              (void*)base, (void*)labeledMax, outSize, g_readableRanges.size(), g_writableRanges.size());
    Log::Debug("labeledMin=%p labeledMax=%p scanWindow=[%p, %p]",
               (void*)labeledMin, (void*)labeledMax, (void*)scanLo, (void*)scanHi);
    return base;
}

// /dev/null does not dereference the write buffer, so write() is not a memory
// probe. Ask the kernel to copy from our address space and reject partial reads.
bool VtableHook::ReadMemory(const void* address, void* output, size_t size)
{
    if (size > UINTPTR_MAX - reinterpret_cast<uintptr_t>(address))
        return false;
    iovec local{output, size};
    iovec remote{const_cast<void*>(address), size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(size);
}

static bool CanReadMemory(const void* addr, size_t len)
{
    if (len > UINTPTR_MAX - reinterpret_cast<uintptr_t>(addr))
        return false;
    char scratch[4096];
    size_t offset = 0;
    while (offset < len) {
        const size_t chunk = (len - offset > sizeof(scratch)) ? sizeof(scratch) : len - offset;
        iovec local{scratch, chunk};
        iovec remote{reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) + offset), chunk};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(chunk))
            return false;
        offset += chunk;
    }
    return true;
}

// Scan readable ranges within steamclient for a byte sequence.
static const uint8_t* FindBytes(const void* needle, size_t needleLen,
                                uintptr_t steamBase, size_t steamSize)
{
    for (size_t r = 0; r < g_readableRanges.size(); r++)
    {
        if (g_readableRanges[r].end <= steamBase ||
            g_readableRanges[r].start >= steamBase + steamSize)
            continue;
        size_t rangeSize = g_readableRanges[r].end - g_readableRanges[r].start;
        if (rangeSize < needleLen) continue;

        const uint8_t* rStart = reinterpret_cast<const uint8_t*>(g_readableRanges[r].start);
        const uint8_t* rEnd = reinterpret_cast<const uint8_t*>(g_readableRanges[r].end);
        for (const uint8_t* p = rStart; p <= rEnd - needleLen; p++)
        {
            if (memcmp(p, needle, needleLen) == 0)
                return p;
        }
    }
    return nullptr;
}

// Search steamclient ranges for a pointer value; tries `primary` first,
// falls back to `fallback` to avoid false positives on small values.
static const uintptr_t* FindPointerValue(uintptr_t primary, uintptr_t fallback,
                                         uintptr_t steamBase, size_t steamSize)
{
    for (int pass = 0; pass < 2; pass++)
    {
        uintptr_t target = (pass == 0) ? primary : fallback;
        if (pass == 1 && primary == fallback) break;  // same value, no second pass

        for (size_t r = 0; r < g_readableRanges.size(); r++)
        {
            if (g_readableRanges[r].end <= steamBase ||
                g_readableRanges[r].start >= steamBase + steamSize)
                continue;

            const uintptr_t* scanStart = reinterpret_cast<const uintptr_t*>(
                (g_readableRanges[r].start + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
            const uintptr_t* scanEnd = reinterpret_cast<const uintptr_t*>(
                g_readableRanges[r].end & ~(sizeof(uintptr_t) - 1));

            for (const uintptr_t* p = scanStart; p < scanEnd; p++)
            {
                if (*p == target)
                    return p;
            }
        }
    }
    return nullptr;
}

#if defined(__x86_64__)
static void** FindTransportTable(uintptr_t typeinfo, uintptr_t base, size_t size)
{
    void** match = nullptr;
    for (const auto& range : g_readableRanges) {
        const auto begin = std::max(range.start, base);
        const auto end = std::min(range.end, base + size);
        if (end <= begin || end - begin < 11 * sizeof(uintptr_t))
            continue;
        const auto first = (begin + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
        for (auto address = first; address <= end - 11 * sizeof(uintptr_t); address += sizeof(uintptr_t)) {
            const auto* header = reinterpret_cast<const uintptr_t*>(address);
            if (header[0] != 0 || header[1] != typeinfo)
                continue;

            auto** table = reinterpret_cast<void**>(address + 2 * sizeof(uintptr_t));
            uintptr_t slots[9];
            if (!VtableHook::ReadMemory(table, slots, sizeof(slots)))
                continue;
            const auto executable = [](uintptr_t entry) {
                return std::any_of(g_executableRanges.begin(), g_executableRanges.end(),
                    [entry](const MapRange& code) { return entry >= code.start && entry < code.end; });
            };
            if (!executable(slots[5]) || !executable(slots[7]) || !executable(slots[8]))
                continue;
            if (match)
                return nullptr;
            match = table;
        }
    }
    return match;
}
#endif

void** VtableHook::FindTransportVtable(uintptr_t steamBase, size_t steamSize)
{
    const size_t nameLen = strlen(RTTI_NAME);

    // Find RTTI string in .rodata (never relocated)
    const uint8_t* rttiStr = FindBytes(RTTI_NAME, nameLen + 1, steamBase, steamSize);
    if (!rttiStr)
    {
        Log::Error("RTTI string '%s' not found in steamclient.so", RTTI_NAME);
        return nullptr;
    }
    uintptr_t rttiStrAddr = reinterpret_cast<uintptr_t>(rttiStr);
    uintptr_t rttiStrVaddr = rttiStrAddr - steamBase;  // file vaddr (pre-relocation value)
    Log::Debug("RTTI typestring at %p (vaddr 0x%zx)", rttiStr, (size_t)rttiStrVaddr);

    // Find typeinfo: name_ptr is raw vaddr (unrelocated) or absolute (relocated)
    const uintptr_t* nameField = FindPointerValue(rttiStrAddr, rttiStrVaddr,
                                                  steamBase, steamSize);
    if (!nameField)
    {
        Log::Error("typeinfo for CClientUnifiedServiceTransport not found");
        Log::Debug("  searched for 0x%zx (relocated) and 0x%zx (unrelocated) across %zu ranges",
                   (size_t)rttiStrAddr, (size_t)rttiStrVaddr, g_readableRanges.size());
        return nullptr;
    }
    const uintptr_t* typeinfo = nameField - 1;  // typeinfo starts one slot before name
    uintptr_t typeinfoAddr = reinterpret_cast<uintptr_t>(typeinfo);
    bool relocated = (*nameField == rttiStrAddr);
    Log::Debug("typeinfo at %p (vaddr 0x%zx, %s)",
               typeinfo, typeinfoAddr - steamBase, relocated ? "relocated" : "unrelocated");

    // Poll until .data.rel.ro relocations are applied
    if (!relocated)
    {
        Log::Info("Relocations pending for .data.rel.ro, waiting...");
        volatile uintptr_t* nameSlot = const_cast<volatile uintptr_t*>(nameField);
        int waitMs = 0;
        const int maxWaitMs = 30000;  // 30 seconds max
        while (*nameSlot != rttiStrAddr && waitMs < maxWaitMs)
        {
            usleep(50000);  // 50ms
            waitMs += 50;
        }
        if (*nameSlot != rttiStrAddr)
        {
            Log::Error("Relocations did not complete after %dms (name_ptr=%p, expected %p)",
                       waitMs, (void*)*nameSlot, (void*)rttiStrAddr);
            return nullptr;
        }
        Log::Info("Relocations completed after %dms", waitMs);
    }

    // Find vtable: scan for [offset_to_top=0, typeinfo_ptr] header
#if defined(__x86_64__)
    void** vtableFuncs = FindTransportTable(typeinfoAddr, steamBase, steamSize);
    if (!vtableFuncs) {
        Log::Error("No unique CClientUnifiedServiceTransport vtable with executable slots");
        return nullptr;
    }
#else
    void** vtableFuncs = nullptr;
    for (size_t r = 0; r < g_readableRanges.size() && !vtableFuncs; r++)
    {
        if (g_readableRanges[r].end <= steamBase ||
            g_readableRanges[r].start >= steamBase + steamSize)
            continue;

        const uintptr_t* scanStart = reinterpret_cast<const uintptr_t*>(
            (g_readableRanges[r].start + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
        const uintptr_t* scanEnd = reinterpret_cast<const uintptr_t*>(
            g_readableRanges[r].end & ~(sizeof(uintptr_t) - 1));

        for (const uintptr_t* p = scanStart; p + 1 < scanEnd; p++)
        {
            if (*p == 0 && *(p + 1) == typeinfoAddr)
            {
                vtableFuncs = reinterpret_cast<void**>(const_cast<uintptr_t*>(p + 2));
                Log::Debug("Found vtable at %p (header at %p) in range %zu",
                           vtableFuncs, p, r);
                break;
            }
        }
    }

    if (!vtableFuncs)
    {
        Log::Error("vtable for CClientUnifiedServiceTransport not found (typeinfo=%p)", typeinfo);
        return nullptr;
    }

    // Validate function pointer slots
    if (!CanReadMemory(vtableFuncs, 9 * sizeof(void*)))
    {
        Log::Error("CClientUnifiedServiceTransport vtable at %p but slots not readable", vtableFuncs);
        return nullptr;
    }

    for (int slot : {5, 7, 8}) {
        uintptr_t fn = reinterpret_cast<uintptr_t>(vtableFuncs[slot]);
        if (fn < steamBase || fn >= steamBase + steamSize) {
            Log::Error("vtable slot %d (%p) outside steamclient range [%p, %p)",
                       slot, (void*)fn, (void*)steamBase, (void*)(steamBase + steamSize));
            return nullptr;
        }
    }
#endif

    Log::Info("CClientUnifiedServiceTransport vtable at %p (offset 0x%zx)",
              vtableFuncs, reinterpret_cast<uintptr_t>(vtableFuncs) - steamBase);
    Log::Debug("  slot5=%p  slot7=%p  slot8=%p",
               vtableFuncs[5], vtableFuncs[7], vtableFuncs[8]);
    return vtableFuncs;
}

static bool MakeWritable(void* addr, size_t len)
{
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(pageSize - 1);
    uintptr_t endAddr = reinterpret_cast<uintptr_t>(addr) + len;
    size_t pageLen = ((endAddr - page) + (pageSize - 1)) & ~(pageSize - 1);
    return mprotect(reinterpret_cast<void*>(page), pageLen, PROT_READ | PROT_WRITE) == 0;
}

static bool MakeReadOnly(void* addr, size_t len)
{
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t page = reinterpret_cast<uintptr_t>(addr) & ~(pageSize - 1);
    uintptr_t endAddr = reinterpret_cast<uintptr_t>(addr) + len;
    size_t pageLen = ((endAddr - page) + (pageSize - 1)) & ~(pageSize - 1);
    return mprotect(reinterpret_cast<void*>(page), pageLen, PROT_READ) == 0;
}

// Defined in cloud_hooks.cpp
extern "C" {
    int hook_BYieldingSend(void* pThis, const char* methodName, void* request, void* response, int* flags);
    int hook_NotificationDirect(void* pThis, const char* methodName, void* body, int* flags);
    int hook_SyncSend2(void* pThis, const char* methodName, void* buf, unsigned int bufLen, void* response, int* flags);
    bool hook_IsCloudEnabledForApp(void* pThis, unsigned int appId);
}

bool VtableHook::InstallHooks(void** vtable, VtableInfo& info)
{
    info.vtable = vtable;
    info.origSlot5 = vtable[5];
    info.origSlot7 = vtable[7];
    info.origSlot8 = vtable[8];

    // The vtable lives in .data.rel.ro -- need to make it writable
    void* slotBase = &vtable[5];
    size_t span = reinterpret_cast<uintptr_t>(&vtable[8]) - reinterpret_cast<uintptr_t>(&vtable[5]) + 8;

    if (!MakeWritable(slotBase, span))
    {
        Log::Error("mprotect RW failed on vtable");
        return false;
    }

    vtable[5] = reinterpret_cast<void*>(&hook_BYieldingSend);
    vtable[7] = reinterpret_cast<void*>(&hook_NotificationDirect);
    vtable[8] = reinterpret_cast<void*>(&hook_SyncSend2);

    // Restore read-only (non-fatal if it fails).
    MakeReadOnly(slotBase, span);

    Log::Info("Vtable hooks installed: slot5=%p slot7=%p slot8=%p",
              vtable[5], vtable[7], vtable[8]);
    return true;
}

void VtableHook::RemoveHooks(const VtableInfo& info)
{
    if (!info.vtable) return;

    void* slotBase = &info.vtable[5];
    size_t span = reinterpret_cast<uintptr_t>(&info.vtable[8]) - reinterpret_cast<uintptr_t>(&info.vtable[5]) + 8;

    if (MakeWritable(slotBase, span))
    {
        info.vtable[5] = info.origSlot5;
        info.vtable[7] = info.origSlot7;
        info.vtable[8] = info.origSlot8;
        MakeReadOnly(slotBase, span);
        Log::Info("Vtable hooks removed");
    }
    else
    {
        Log::Warn("mprotect RW failed during hook removal");
    }
}

static constexpr const char RTTI_REMOTE_STORAGE[] = "18CUserRemoteStorage";

static bool IsWithinSteamclient(uintptr_t ptr, uintptr_t steamBase, size_t steamSize)
{
    return ptr >= steamBase && ptr < steamBase + steamSize;
}

static bool IsLikelyAppCloudEnabledSlot(void** vtable, size_t slotIndex, uintptr_t steamBase, size_t steamSize)
{
    void* function = nullptr;
    if (!VtableHook::ReadMemory(vtable + slotIndex, &function, sizeof(function)))
        return false;
    uintptr_t fn = reinterpret_cast<uintptr_t>(function);
    if (!IsWithinSteamclient(fn, steamBase, steamSize) || !CanReadMemory(reinterpret_cast<void*>(fn), 30))
        return false;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(fn);

#if defined(__x86_64__)
    // Steam 1792618285, SysV ABI: appId is ESI. This method checks the per-app
    // override tree at this+0x300 and then UFS external-file policy. Match both
    // argument handling and that tree access, not a slot number alone.
    static constexpr uint8_t prefix[] = {
        0x85,0xf6,0x41,0x54,0x55,0x48,0x89,0xfd,0x53,0x89,0xf3,0x0f,0x84
    };
    static constexpr uint8_t tree[] = {0x48,0x63,0x85,0x00,0x03,0x00,0x00,0x83,0xf8,0xff};
    // SLSsteam may already own a five-byte entry detour. Its untouched
    // instruction suffix still identifies this method; our saved original
    // pointer preserves that existing detour for non-namespace apps.
    const bool entryMatches = memcmp(p, prefix, sizeof(prefix)) == 0 ||
        (p[0] == 0xe9 && memcmp(p + 5, prefix + 5, sizeof(prefix) - 5) == 0);
    return entryMatches && memcmp(p + 17, tree, sizeof(tree)) == 0;
#endif

    // Pattern A (older builds): args passed via esp offsets
    //   +0:  55 57 56 53 E8 ?? ?? ?? ??  (push ebp/edi/esi/ebx + call PIC thunk)
    //   +9:  81 C3 ?? ?? ?? ??          (add ebx, imm32)
    //   +15: 83 EC 0C                   (sub esp, 0x0C)
    //   +18: 8B 74 24 24                (mov esi, [esp+0x24] - appId)
    //   +22: 8B 7C 24 20                (mov edi, [esp+0x20] - this)
    if (p[0] == 0x55 && p[1] == 0x57 && p[2] == 0x56 && p[3] == 0x53 && p[4] == 0xE8 &&
        p[15] == 0x83 && p[16] == 0xEC && p[17] == 0x0C &&
        p[18] == 0x8B && p[19] == 0x74 && p[20] == 0x24 && p[21] == 0x24 &&
        p[22] == 0x8B && p[23] == 0x7C && p[24] == 0x24 && p[25] == 0x20)
        return true;

    // Pattern B (May 2026+): esi/ebx pushed after PIC fixup, args via ebp
    //   +0:  55 89 E5 57 E8 ?? ?? ?? ??  (push ebp; mov ebp,esp; push edi; call PIC)
    //   +9:  81 C7/C3 ?? ?? ?? ??       (add edi/ebx, imm32)
    //   +15: 56 53                      (push esi; push ebx)
    //   +17: 83 EC                      (sub esp, imm8)
    //   +20: 8B 45 0C                   (mov eax, [ebp+0Ch] - appId arg)
    //   +23: 85 C0                      (test eax, eax - appId null check)
    //   +25: 0F 84                      (jz near - distinct from jnz in other methods)
    if (p[0] == 0x55 && p[1] == 0x89 && p[2] == 0xE5 && p[3] == 0x57 && p[4] == 0xE8 &&
        p[9] == 0x81 &&
        p[15] == 0x56 && p[16] == 0x53 &&
        p[17] == 0x83 && p[18] == 0xEC &&
        p[20] == 0x8B && p[21] == 0x45 && p[22] == 0x0C &&
        p[23] == 0x85 && p[24] == 0xC0 &&
        p[25] == 0x0F && p[26] == 0x84)
        return true;

    return false;
}

static size_t ResolveCloudEnabledSlot(void** vtable, uintptr_t steamBase, size_t steamSize)
{
    size_t match = SIZE_MAX;
    for (size_t slot = 0; slot < 32; ++slot) {
        if (!IsLikelyAppCloudEnabledSlot(vtable, slot, steamBase, steamSize))
            continue;
        if (match != SIZE_MAX)
            return SIZE_MAX;
        match = slot;
    }
    return match;
}

void** VtableHook::FindRemoteStorageVtable(uintptr_t steamBase, size_t steamSize)
{
    const size_t nameLen = strlen(RTTI_REMOTE_STORAGE);

    // Find RTTI string
    const uint8_t* rttiStr = FindBytes(RTTI_REMOTE_STORAGE, nameLen + 1, steamBase, steamSize);
    if (!rttiStr)
    {
        Log::Error("RTTI string '%s' not found in steamclient.so", RTTI_REMOTE_STORAGE);
        return nullptr;
    }
    uintptr_t rttiStrAddr = reinterpret_cast<uintptr_t>(rttiStr);
    uintptr_t rttiStrVaddr = rttiStrAddr - steamBase;
    Log::Debug("CUserRemoteStorage RTTI at %p (vaddr 0x%zx)", rttiStr, (size_t)rttiStrVaddr);

    // Find typeinfo (relocations already waited on by Transport)
    const uintptr_t* nameField = FindPointerValue(rttiStrAddr, rttiStrVaddr,
                                                  steamBase, steamSize);
    if (!nameField)
    {
        Log::Error("typeinfo for CUserRemoteStorage not found");
        return nullptr;
    }
    const uintptr_t* typeinfo = nameField - 1;
    uintptr_t typeinfoAddr = reinterpret_cast<uintptr_t>(typeinfo);
    Log::Debug("CUserRemoteStorage typeinfo at %p", typeinfo);

    // Find vtable: [offset_to_top=0, typeinfo_ptr]
    void** vtableFuncs = nullptr;
    for (size_t r = 0; r < g_readableRanges.size() && !vtableFuncs; r++)
    {
        if (g_readableRanges[r].end <= steamBase ||
            g_readableRanges[r].start >= steamBase + steamSize)
            continue;

        const uintptr_t* scanStart = reinterpret_cast<const uintptr_t*>(
            (g_readableRanges[r].start + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
        const uintptr_t* scanEnd = reinterpret_cast<const uintptr_t*>(
            g_readableRanges[r].end & ~(sizeof(uintptr_t) - 1));

        for (const uintptr_t* p = scanStart; p + 1 < scanEnd; p++)
        {
            if (*p == 0 && *(p + 1) == typeinfoAddr)
            {
                vtableFuncs = reinterpret_cast<void**>(const_cast<uintptr_t*>(p + 2));
                break;
            }
        }
    }

    if (!vtableFuncs)
    {
        Log::Error("vtable for CUserRemoteStorage not found");
        return nullptr;
    }

    if (!CanReadMemory(vtableFuncs, 25 * sizeof(void*)))
    {
        Log::Error("CUserRemoteStorage vtable at %p but slots not readable", vtableFuncs);
        return nullptr;
    }

    size_t cloudEnabledSlot = ResolveCloudEnabledSlot(vtableFuncs, steamBase, steamSize);
    if (cloudEnabledSlot == SIZE_MAX)
    {
        Log::Error("CUserRemoteStorage vtable at %p but could not resolve cloud-enabled slot", vtableFuncs);
        return nullptr;
    }

    Log::Info("CUserRemoteStorage vtable at %p (cloud-enabled slot%zu=%p)",
              vtableFuncs, cloudEnabledSlot, vtableFuncs[cloudEnabledSlot]);
    return vtableFuncs;
}

void** VtableHook::FindVtableByRTTIName(const char* mangledName,
                                       uintptr_t steamBase, size_t steamSize)
{
    const size_t nameLen = strlen(mangledName);

    // 1) RTTI type-name string (.rodata, never relocated).
    const uint8_t* rttiStr = FindBytes(mangledName, nameLen + 1, steamBase, steamSize);
    if (!rttiStr)
    {
        Log::Error("RTTI string '%s' not found in steamclient.so", mangledName);
        return nullptr;
    }
    uintptr_t rttiStrAddr = reinterpret_cast<uintptr_t>(rttiStr);
    uintptr_t rttiStrVaddr = rttiStrAddr - steamBase;  // file vaddr (pre-relocation)
    Log::Debug("RTTI '%s' at %p (vaddr 0x%zx)", mangledName, rttiStr, (size_t)rttiStrVaddr);

    // 2) typeinfo: name_ptr holds relocated absolute or unrelocated file vaddr.
    const uintptr_t* nameField = FindPointerValue(rttiStrAddr, rttiStrVaddr,
                                                  steamBase, steamSize);
    if (!nameField)
    {
        Log::Error("typeinfo for '%s' not found (rel=0x%zx unrel=0x%zx)",
                   mangledName, (size_t)rttiStrAddr, (size_t)rttiStrVaddr);
        return nullptr;
    }
    const uintptr_t* typeinfo = nameField - 1;  // typeinfo starts one slot before name
    uintptr_t typeinfoAddr = reinterpret_cast<uintptr_t>(typeinfo);
    bool relocated = (*nameField == rttiStrAddr);
    Log::Debug("typeinfo for '%s' at %p (%s)", mangledName, typeinfo,
               relocated ? "relocated" : "unrelocated");

    // 3) Wait for .data.rel.ro relocations if pending (vtable typeinfo ptr is
    // the runtime absolute typeinfoAddr only once relocated).
    if (!relocated)
    {
        volatile uintptr_t* nameSlot = const_cast<volatile uintptr_t*>(nameField);
        int waitMs = 0;
        const int maxWaitMs = 30000;
        while (*nameSlot != rttiStrAddr && waitMs < maxWaitMs)
        {
            usleep(50000);
            waitMs += 50;
        }
        if (*nameSlot != rttiStrAddr)
        {
            Log::Error("relocations did not complete for '%s' after %dms", mangledName, waitMs);
            return nullptr;
        }
    }

    // 4) vtable: scan for [offset_to_top=0, typeinfo_ptr] header.
    for (size_t r = 0; r < g_readableRanges.size(); r++)
    {
        if (g_readableRanges[r].end <= steamBase ||
            g_readableRanges[r].start >= steamBase + steamSize)
            continue;

        const uintptr_t* scanStart = reinterpret_cast<const uintptr_t*>(
            (g_readableRanges[r].start + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
        const uintptr_t* scanEnd = reinterpret_cast<const uintptr_t*>(
            g_readableRanges[r].end & ~(sizeof(uintptr_t) - 1));

        for (const uintptr_t* p = scanStart; p + 1 < scanEnd; p++)
        {
            if (*p == 0 && *(p + 1) == typeinfoAddr)
            {
                void** vtableFuncs = reinterpret_cast<void**>(const_cast<uintptr_t*>(p + 2));
                if (!CanReadMemory(vtableFuncs, sizeof(void*)))
                    continue;
                Log::Info("vtable for '%s' at %p (offset 0x%zx)", mangledName,
                          vtableFuncs, reinterpret_cast<uintptr_t>(vtableFuncs) - steamBase);
                return vtableFuncs;
            }
        }
    }

    Log::Error("vtable for '%s' not found (typeinfo=%p)", mangledName, typeinfo);
    return nullptr;
}

void* VtableHook::FindGlobalWithVtable(void* vtablePtr,
                                       uintptr_t steamBase, size_t steamSize)
{
    uintptr_t target = reinterpret_cast<uintptr_t>(vtablePtr);

    // Default instances live in writable .data/.bss. Scan writable ranges.
    for (size_t r = 0; r < g_writableRanges.size(); r++)
    {
        if (g_writableRanges[r].end <= steamBase ||
            g_writableRanges[r].start >= steamBase + steamSize)
            continue;

        const uintptr_t* scanStart = reinterpret_cast<const uintptr_t*>(
            (g_writableRanges[r].start + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
        const uintptr_t* scanEnd = reinterpret_cast<const uintptr_t*>(
            g_writableRanges[r].end & ~(sizeof(uintptr_t) - 1));

        for (const uintptr_t* p = scanStart; p + 2 < scanEnd; p++)
        {
            // First word == vtable, next two words == 0 (arena / has_bits).
            if (*p == target && *(p + 1) == 0 && *(p + 2) == 0)
            {
                void* inst = reinterpret_cast<void*>(const_cast<uintptr_t*>(p));
                Log::Info("default instance for vtable %p at %p (offset 0x%zx)",
                          vtablePtr, inst, reinterpret_cast<uintptr_t>(inst) - steamBase);
                return inst;
            }
        }
    }

    Log::Error("default instance for vtable %p not found", vtablePtr);
    return nullptr;
}

bool VtableHook::InstallCloudEnabledHook(void** vtable, CloudEnabledHookInfo& info)
{
    size_t slotIndex = ResolveCloudEnabledSlot(vtable, 0, UINTPTR_MAX);
    if (slotIndex == SIZE_MAX)
    {
        Log::Error("Could not resolve CUserRemoteStorage cloud-enabled slot");
        return false;
    }

    info.vtable = vtable;
    info.slotIndex = slotIndex;
    info.origSlot = vtable[slotIndex];

    void* slotAddr = &vtable[slotIndex];
    if (!MakeWritable(slotAddr, sizeof(void*)))
    {
        Log::Error("mprotect RW failed on RemoteStorage vtable slot %zu", slotIndex);
        return false;
    }

    vtable[slotIndex] = reinterpret_cast<void*>(&hook_IsCloudEnabledForApp);
    MakeReadOnly(slotAddr, sizeof(void*));

    Log::Info("IsCloudEnabledForApp hook installed at slot %zu (orig=%p)", slotIndex, info.origSlot);
    return true;
}

void VtableHook::RemoveCloudEnabledHook(const CloudEnabledHookInfo& info)
{
    if (!info.vtable) return;

    void* slotAddr = &info.vtable[info.slotIndex];
    if (MakeWritable(slotAddr, sizeof(void*)))
    {
        info.vtable[info.slotIndex] = info.origSlot;
        MakeReadOnly(slotAddr, sizeof(void*));
        Log::Info("IsCloudEnabledForApp hook removed");
    }
}
