// Exercise legacy detours on synthetic RX pages. The generated i386 code is
// never executed, and no Steam process or file is used.
#include "../src/platform/linux/gamesplayed_hook.cpp"
#include "../src/platform/linux/live_playtime.cpp"
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <signal.h>
#include <sys/resource.h>

#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c);                                                  \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (false)
namespace Log {
void Write(const char*, ...) {}
} // namespace Log
namespace StatsHandlers {
std::vector<uint32_t> ObserveGamesPlayed(const uint8_t*, size_t) {
    return {};
}
void ObserveStoreUserStats(const uint8_t*, size_t) {}
} // namespace StatsHandlers
namespace MetadataSync {
std::atomic<bool> syncPlaytime{true};
std::atomic<bool> syncAchievements{true};
} // namespace MetadataSync
namespace StatsHandlers {
PB::Writer BuildLastPlayedNotificationBody(const std::vector<uint32_t>&) {
    return {};
}
} // namespace StatsHandlers

static bool RejectsWrite(uint8_t* code) {
    const pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        code[0] = 0;
        _exit(0);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV;
}

int main() {
    const rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    const size_t page = sysconf(_SC_PAGESIZE);
    auto code = static_cast<uint8_t*>(
        mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(code != MAP_FAILED);
#if !defined(__x86_64__)
    memcpy(code, GamesPlayedHook::kSigBytes, GamesPlayedHook::kSigLen);
    std::vector<uint8_t> saved(code, code + GamesPlayedHook::kSigLen);
    CHECK(mprotect(code, page, PROT_READ | PROT_EXEC) == 0);
    CHECK(GamesPlayedHook::Install(reinterpret_cast<uintptr_t>(code), page));
    CHECK(code[GamesPlayedHook::PROLOGUE_LEN] == 0xe9);
    CHECK(GamesPlayedHook::Remove());
    CHECK(!memcmp(code, saved.data(), saved.size()));
    CHECK(GamesPlayedHook::Remove());
    CHECK(RejectsWrite(code));
    CHECK(mprotect(code, page, PROT_READ | PROT_WRITE) == 0);
#endif
    memset(code, 0x90, page);
    const std::vector<uint8_t> original(code, code + page);
    LivePlaytime::g_writerStart = code;
    CHECK(mprotect(code, page, PROT_READ | PROT_EXEC) == 0);
    CHECK(LivePlaytime::InstallUserCapture());
    CHECK(code[LivePlaytime::WRITER_PROLOGUE] == 0xe9);
    CHECK(LivePlaytime::RemoveUserCapture());
    CHECK(!memcmp(code, original.data(), original.size()));
    CHECK(LivePlaytime::RemoveUserCapture());
    CHECK(RejectsWrite(code));
    munmap(code, page);
}
