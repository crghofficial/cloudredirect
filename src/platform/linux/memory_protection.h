#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cinttypes>
#include <sys/mman.h>
#include <unistd.h>

// Small hook patches can span two pages with different permissions.
struct MemoryProtection {
    uintptr_t pages[2]{};
    int permissions[2]{};
    size_t count = 0;

    bool Capture(void* address, size_t size) {
        const long pageSize = sysconf(_SC_PAGESIZE);
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        if (count || pageSize <= 0 || !size || start > UINTPTR_MAX - (size - 1))
            return false;
        const uintptr_t first = start & ~(uintptr_t(pageSize) - 1);
        const uintptr_t last = (start + size - 1) & ~(uintptr_t(pageSize) - 1);
        if (last - first > uintptr_t(pageSize))
            return false;
        FILE* maps = fopen("/proc/self/maps", "r");
        if (!maps)
            return false;
        const size_t needed = first == last ? 1 : 2;
        char line[512], flags[5];
        uintptr_t begin, end;
        while (fgets(line, sizeof(line), maps)) {
            if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &begin, &end, flags) != 3)
                continue;
            for (size_t i = count; i < needed; ++i) {
                const uintptr_t page = first + i * pageSize;
                if (page < begin || page >= end)
                    break;
                pages[i] = page;
                permissions[i] = (flags[0] == 'r' ? PROT_READ : 0) | (flags[1] == 'w' ? PROT_WRITE : 0) |
                                 (flags[2] == 'x' ? PROT_EXEC : 0);
                ++count;
            }
            if (count == needed)
                break;
        }
        fclose(maps);
        if (count == needed)
            return true;
        count = 0;
        return false;
    }

    bool Restore() const {
        bool ok = true;
        const long pageSize = sysconf(_SC_PAGESIZE);
        for (size_t i = 0; i < count; ++i)
            if (mprotect(reinterpret_cast<void*>(pages[i]), pageSize, permissions[i]) != 0)
                ok = false;
        return ok;
    }

    bool MakeWritable() const {
        const long pageSize = sysconf(_SC_PAGESIZE);
        for (size_t i = 0; i < count; ++i) {
            if (mprotect(reinterpret_cast<void*>(pages[i]), pageSize, permissions[i] | PROT_WRITE) != 0) {
                Restore();
                return false;
            }
        }
        return count != 0;
    }
};
