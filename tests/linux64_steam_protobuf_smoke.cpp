#ifdef NDEBUG
#undef NDEBUG
#endif
// Opt-in, offline smoke test against the user's exact Steam ELF. This loads a
// separate library instance and creates one protobuf message; it never attaches
// to Steam, creates a Steam pipe, logs in, or accesses any saves/cloud provider.
// Known vtable RVA is TEST ONLY and is gated by the complete input SHA-256.
#include "../src/common/sha256_hmac.cpp"
#include "../src/platform/linux/cloud_hooks.cpp"
#include <cassert>
#include <cstdarg>
#include <dlfcn.h>
#include <string_view>

namespace Log {
void Write(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}
} // namespace Log

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') {
        fprintf(stderr, "Usage: %s /absolute/path/to/steamclient.so\n", argv[0]);
        return 77;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file)
        return 1;
    std::string image((std::istreambuf_iterator<char>(file)), {});
    struct Fixture {
        std::string_view sha256;
        uintptr_t requestVtable;
    };
    static constexpr Fixture fixtures[] = {
        // 1792618285
        {"53bdabbbb63fff0146a459f1f40dc9208661f329810557c462404ba52318bf11", 0x31dc390},
        // 1794536281
        {"dab8df0daa27ffa6594ef5d6bc8cf707f37990220628f1f249fb30f4d88ade91", 0x31dc450},
    };
    const auto hash = crypto::Sha256Hex(image);
    uintptr_t requestVtable = 0;
    for (const auto& fixture : fixtures) {
        if (hash == fixture.sha256) {
            requestVtable = fixture.requestVtable;
            break;
        }
    }
    if (!requestVtable) {
        fprintf(stderr, "Unsupported fixture identity; known vtable RVA will not be used\n");
        return 77;
    }
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    Dl_info info{};
    assert(dladdr(dlsym(handle, "Steam_CreateSteamPipe"), &info));
    uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
    assert(CloudHooks::ResolveProtobufHelpers(info.dli_fbase, image.size()));

    // Real CCloud_GetAppFileChangelist_Request, identified by ELF RTTI relocs.
    auto *vtable = reinterpret_cast<uintptr_t *>(base + requestVtable);
    struct {
        uintptr_t *vtable;
    } prototype{vtable};
    auto *typeinfo = reinterpret_cast<uintptr_t *>(vtable[-1]);
    assert(strcmp(reinterpret_cast<const char *>(typeinfo[1]),
                  "35CCloud_GetAppFileChangelist_Request") == 0);
    using NewFn = void *(*)(void *);
    void *request = reinterpret_cast<NewFn>(vtable[3])(&prototype);
    assert(request);
    const uint8_t wire[] = {0x08, 0x96, 0x01}; // appid = 150
    assert(ParseIntoMessage(request, wire, sizeof(wire)));
    auto bytes = SerializeMessage(request);
    assert(bytes == std::vector<uint8_t>(wire, wire + sizeof(wire)));
    reinterpret_cast<void (*)(void *)>((*reinterpret_cast<uintptr_t **>(request))[1])(request);
    puts("Actual Steam x64 cloud request Parse/ByteSize/Serialize roundtrip passed");
    // Process exit owns library teardown; no live Steam process is affected.
    return 0;
}
