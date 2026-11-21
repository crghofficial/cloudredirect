#include "../src/common/steam_kv_injector.cpp"
#include <array>
#include <cstdio>
#include <cstdlib>

static void require(bool condition) {
    if (!condition) {
        std::fputs("KV tree lookup failed\n", stderr);
        std::exit(1);
    }
}

int main() {
    using namespace SteamKvInjector;
    alignas(8) std::array<uint8_t, 0x48> cache{};
    alignas(8) std::array<uint8_t, 64> nodes{};
    const auto setIndex = [&](size_t offset, int32_t value) {
        std::memcpy(nodes.data() + offset, &value, sizeof(value));
    };
    const auto setRoot = [&](int32_t value) {
        std::memcpy(cache.data() + 0x28, &value, sizeof(value));
    };
    const auto setNodes = [&](uintptr_t value) {
        std::memcpy(cache.data() + 0x40, &value, sizeof(value));
    };
    int appInfo = 0;
    void* expected = &appInfo;
    uint32_t appId = 100;
    std::memcpy(nodes.data() + 16, &appId, sizeof(appId));
    std::memcpy(nodes.data() + 24, &expected, sizeof(expected));
    setIndex(0, -1);
    setIndex(4, -1);
    setRoot(0);
    setNodes(reinterpret_cast<uintptr_t>(nodes.data()));
    require(ResolveAppInfo(cache.data(), 100) == expected);
    require(ResolveAppInfo(cache.data(), 99) == nullptr);
    require(ResolveAppInfo(cache.data(), 101) == nullptr);
    setIndex(0, 0); // A corrupt cycle must terminate.
    require(ResolveAppInfo(cache.data(), 99) == nullptr);
    setRoot(-1);
    require(ResolveAppInfo(cache.data(), 100) == nullptr);
    setRoot(0);
    setNodes(1);
    require(ResolveAppInfo(cache.data(), 100) == nullptr);
    setRoot(1);
    setNodes(UINTPTR_MAX - 16);
    require(ResolveAppInfo(cache.data(), 100) == nullptr);
    require(ResolveAppInfo(nullptr, 100) == nullptr);
    std::puts("KV lookup and corrupt-pointer checks passed");
}
