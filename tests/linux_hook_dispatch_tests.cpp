#include "../src/platform/linux/cloud_hooks.cpp"
#include <cstdlib>
namespace Log {
void Write(const char*, ...) {}
} // namespace Log
extern "C" void CR_SetCrashContext(const char*, const char*, uint32_t) {}
extern "C" void CR_ClearCrashContext() {}
static int originalCalls = 0;
static int interceptCalls = 0;
namespace CloudIntercept {
bool IsNamespaceApp(uint32_t) {
    ++interceptCalls;
    return true;
}
} // namespace CloudIntercept
static bool Original(void*, unsigned) {
    ++originalCalls;
    return false;
}
int main() {
    CloudHooks::SetOriginalIsCloudEnabled(reinterpret_cast<void*>(&Original));
    if (hook_IsCloudEnabledForApp(nullptr, 42) || originalCalls != 1 || interceptCalls)
        return 1;
    CloudHooks::Enable();
    if (!hook_IsCloudEnabledForApp(nullptr, 42) || originalCalls != 1 || interceptCalls != 1)
        return 2;
    g_shuttingDown.store(true);
    if (hook_IsCloudEnabledForApp(nullptr, 42) || originalCalls != 2 || interceptCalls != 1)
        return 3;
}
