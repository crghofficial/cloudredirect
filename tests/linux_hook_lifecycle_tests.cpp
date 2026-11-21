#include "../src/platform/linux/vtable_hook.cpp"
#include <cerrno>
#include <cstdlib>

#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c);                                             \
            std::exit(1);                                                                                    \
        }                                                                                                    \
    } while (false)

static int protectCalls = 0;
static int failCall = 0;
extern "C" int __real_mprotect(void*, size_t, int);
extern "C" int __wrap_mprotect(void* p, size_t n, int prot) {
    if (++protectCalls == failCall) {
        errno = EACCES;
        return -1;
    }
    return __real_mprotect(p, n, prot);
}

extern "C" int hook_BYieldingSend(void*, const char*, void*, void*, int*) {
    return 0;
}
extern "C" int hook_NotificationDirect(void*, const char*, void*, int*) {
    return 0;
}
extern "C" int hook_SyncSend2(void*, const char*, void*, unsigned, void*, int*) {
    return 0;
}
extern "C" bool hook_IsCloudEnabledForApp(void*, unsigned) {
    return false;
}

static int Protection(void* address) {
    FILE* f = fopen("/proc/self/maps", "r");
    CHECK(f);
    char line[512], perms[5];
    uintptr_t begin, end, at = reinterpret_cast<uintptr_t>(address);
    int result = -1;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &begin, &end, perms) == 3 && at >= begin &&
            at < end) {
            result = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                     (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    fclose(f);
    return result;
}

namespace CloudHooks {
void SetOriginalIsCloudEnabled(void*) {}
} // namespace CloudHooks

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const size_t page = sysconf(_SC_PAGESIZE);
    auto area = static_cast<char*>(
        mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(area != MAP_FAILED);
    // Slots straddle pages with different original permissions.
    auto table = reinterpret_cast<void**>(area + page - 7 * sizeof(void*));
    for (int i = 0; i < 10; ++i)
        table[i] = reinterpret_cast<void*>(uintptr_t(100 + i));
    CHECK(__real_mprotect(area, page, PROT_READ) == 0);
    CHECK(__real_mprotect(area + page, page, PROT_READ | PROT_EXEC) == 0);
    uint8_t method[40]{};
#if defined(__x86_64__)
    const uint8_t prefix[] = {0x85, 0xf6, 0x41, 0x54, 0x55, 0x48, 0x89, 0xfd, 0x53, 0x89, 0xf3, 0x0f, 0x84};
    const uint8_t tree[] = {0x48, 0x63, 0x85, 0x00, 0x03, 0x00, 0x00, 0x83, 0xf8, 0xff};
    memcpy(method, prefix, sizeof(prefix));
    memcpy(method + 17, tree, sizeof(tree));
#else
    const uint8_t prefix[] = {0x55, 0x89, 0xe5, 0x57, 0xe8, 0, 0,    0,    0,    0x81, 0,    0,    0,   0,
                              0,    0x56, 0x53, 0x83, 0xec, 0, 0x8b, 0x45, 0x0c, 0x85, 0xc0, 0x0f, 0x84};
    memcpy(method, prefix, sizeof(prefix));
#endif
    auto remote =
        static_cast<void**>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(remote != MAP_FAILED);
    remote[24] = method;
    VtableHook::CloudEnabledHookInfo cloud{};
    CHECK(VtableHook::InstallCloudEnabledHook(remote, cloud));
    CHECK(remote[24] != method);
    CHECK(VtableHook::RemoveCloudEnabledHook(cloud));
    CHECK(remote[24] == method);
    CHECK(VtableHook::RemoveCloudEnabledHook(cloud));
    failCall = protectCalls + 2;
    CHECK(!VtableHook::InstallCloudEnabledHook(remote, cloud));
    CHECK(remote[24] == method);
    CHECK(Protection(remote) == (PROT_READ | PROT_WRITE));
    failCall = 0;
    CHECK(VtableHook::RemoveCloudEnabledHook(cloud));
    munmap(remote, page);
    protectCalls = 0;
    VtableHook::VtableInfo info{};
    if (!strcmp(argv[1], "failed-install"))
        failCall = 1;
    if (!strcmp(argv[1], "failed-seal"))
        failCall = 3;
    bool installed = VtableHook::InstallHooks(table, info);
    if (!strcmp(argv[1], "failed-install") || !strcmp(argv[1], "failed-seal")) {
        CHECK(!installed);
    } else {
        CHECK(installed);
        CHECK(Protection(area) == PROT_READ);
        CHECK(Protection(area + page) == (PROT_READ | PROT_EXEC));
    }
    if (!strcmp(argv[1], "failed-remove") || !strcmp(argv[1], "failed-remove-seal")) {
        failCall = protectCalls + (!strcmp(argv[1], "failed-remove") ? 1 : 3);
        CHECK(!VtableHook::RemoveHooks(info));
        CHECK(info.vtable);
    }
    if (!strcmp(argv[1], "other-hook")) {
        CHECK(__real_mprotect(area, 2 * page, PROT_READ | PROT_WRITE) == 0);
        table[7] = reinterpret_cast<void*>(999);
    }
    failCall = 0;
    CHECK(VtableHook::RemoveHooks(info));
    CHECK(!info.vtable);
    CHECK(VtableHook::RemoveHooks(info));
    CHECK(table[5] == reinterpret_cast<void*>(105));
    CHECK(table[7] == reinterpret_cast<void*>(!strcmp(argv[1], "other-hook") ? 999 : 107));
    CHECK(table[8] == reinterpret_cast<void*>(108));
    CHECK(Protection(area) == PROT_READ);
    CHECK(Protection(area + page) == (PROT_READ | PROT_EXEC));
    munmap(area, 2 * page);
}
