#include "../src/platform/linux/init.cpp"
#include <sys/mman.h>
#include <stdexcept>

static void* transport[10];
static void* remote[25];
enum InitStage {
    NoTransport,
    AfterTransport,
    AfterRemoteStorage,
    Exception,
    ObserverFailure,
    Success,
    RemovalFailure,
    UnknownException,
    Cancelled
};
static InitStage stage = NoTransport;
static bool stopped = false;
static bool enabled = false;
static bool failRemoval = false;
static bool rollbackIncomplete = false;

namespace Log {
void Init() {}
void Info(const char*, ...) {}
void Warn(const char*, ...) {}
void Error(const char* message, ...) {
    if (strstr(message, "rollback failed"))
        rollbackIncomplete = true;
}
} // namespace Log
namespace VtableHook {
uintptr_t FindSteamclient(size_t& n) {
    n = 1;
    return 1;
}
void** FindTransportVtable(uintptr_t, size_t) {
    return stage == NoTransport ? nullptr : transport;
}
bool InstallHooks(void** t, VtableInfo& i) {
    i.vtable = t;
    i.origSlot5 = t[5];
    i.origSlot7 = t[7];
    i.origSlot8 = t[8];
    t[5] = t[7] = t[8] = reinterpret_cast<void*>(2);
    return true;
}
bool RemoveHooks(VtableInfo& i) {
    if (failRemoval)
        return false;
    if (i.vtable) {
        i.vtable[5] = i.origSlot5;
        i.vtable[7] = i.origSlot7;
        i.vtable[8] = i.origSlot8;
    }
    i = {};
    return true;
}
void** FindRemoteStorageVtable(uintptr_t, size_t) {
    if (stage == AfterTransport)
        raise(SIGSEGV);
    return remote;
}
bool InstallCloudEnabledHook(void** t, CloudEnabledHookInfo& i) {
    i.vtable = t;
    i.origSlot = t[24];
    i.slotIndex = 24;
    t[24] = reinterpret_cast<void*>(2);
    return true;
}
bool RemoveCloudEnabledHook(CloudEnabledHookInfo& i) {
    if (i.vtable)
        i.vtable[i.slotIndex] = i.origSlot;
    i = {};
    return true;
}
} // namespace VtableHook
namespace CloudHooks {
void SetOriginals(void*, void*, void*) {}
void SetOriginalIsCloudEnabled(void*) {}
bool ResolveProtobufHelpers(void*, size_t) {
    return true;
}
bool InstallGamesPlayedObserver(uintptr_t, size_t) {
    return stage != ObserverFailure;
}
void Enable() {
    enabled = true;
}
bool BeginShutdown(bool) {
    stopped = true;
    enabled = false;
    return true;
}
} // namespace CloudHooks
namespace SteamKvInjector {
bool Init() {
    return true;
}
} // namespace SteamKvInjector
namespace CloudIntercept {
std::string GetSteamPath() {
    return "/fixture";
}
void FlushPendingSyncStates() {}
} // namespace CloudIntercept
namespace CloudStorage {
void Shutdown() {}
} // namespace CloudStorage
namespace HttpServer {
void Stop() {}
} // namespace HttpServer
namespace LegacyMetadataCleanup {
SweepStats PruneSteamUserdata(const std::string&) {
    if (stage == Cancelled)
        g_stopInit.store(true);
    if (stage == AfterRemoteStorage)
        raise(SIGBUS);
    if (stage == UnknownException)
        throw 7;
    if (stage == Exception || stage == RemovalFailure)
        throw std::runtime_error("injected init failure");
    return {};
}
} // namespace LegacyMetadataCleanup

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    stage = static_cast<InitStage>(atoi(argv[1]));
    failRemoval = stage == RemovalFailure;
    std::string pattern = "/tmp/cr-init-XXXXXX";
    char* dir = mkdtemp(pattern.data());
    if (!dir)
        return 3;
    setenv("XDG_CONFIG_HOME", dir, 1);
    std::filesystem::create_directory(std::string(dir) + "/CloudRedirect");
    std::ofstream(std::string(dir) + "/CloudRedirect/config.json") << "{\"notifications_enabled\":false}";
    // Trigger the real deferred-init path without loading Steam.
    int fd = memfd_create("steamclient.so", 0);
    if (fd < 0 || ftruncate(fd, 4096))
        return 4;
    void* mapping = mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED)
        return 4;
    for (auto& slot : transport)
        slot = reinterpret_cast<void*>(1);
    for (auto& slot : remote)
        slot = reinterpret_cast<void*>(1);
    struct sigaction previous{};
    previous.sa_handler = SIG_IGN;
    sigemptyset(&previous.sa_mask);
    sigaction(SIGSEGV, &previous, nullptr);
    sigaction(SIGBUS, &previous, nullptr);
    DeferredInitThread(nullptr);
    struct sigaction restoredSegv{}, restoredBus{};
    sigaction(SIGSEGV, nullptr, &restoredSegv);
    sigaction(SIGBUS, nullptr, &restoredBus);
    if (stage != Success && (restoredSegv.sa_handler != SIG_IGN || restoredBus.sa_handler != SIG_IGN))
        return 5;
    if (stage == Success) {
        if (!g_initialized.load() || !enabled || transport[5] != reinterpret_cast<void*>(2))
            return 6;
        OnUnload();
    }
    if (stage == RemovalFailure) {
        if (enabled || !g_hookCleanupPending || !rollbackIncomplete ||
            transport[5] != reinterpret_cast<void*>(2))
            return 7;
        failRemoval = false;
        if (!RemoveInstalledHooks(false))
            return 8;
    }
    const bool ok = !g_initialized.load() && !g_hookCleanupPending && !enabled && stopped &&
                    transport[5] == reinterpret_cast<void*>(1) &&
                    transport[7] == reinterpret_cast<void*>(1) &&
                    transport[8] == reinterpret_cast<void*>(1) && remote[24] == reinterpret_cast<void*>(1);
    if (!ok)
        fprintf(stderr, "partial init retained hooks: transport=%p remote=%p stopped=%d\n", transport[5],
                remote[24], stopped);
    munmap(mapping, 4096);
    close(fd);
    std::filesystem::remove_all(dir);
    return ok ? 0 : 1;
}
