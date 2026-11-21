#ifdef NDEBUG
#undef NDEBUG
#endif
// Standalone regression test for the Linux transport's internal ABI boundary.
// Compile with function/data sections and --gc-sections; unrelated RPC/provider
// paths from cloud_hooks.cpp are deliberately not linked into this fixture.
#include "../src/platform/linux/cloud_hooks.cpp"
#include <cassert>
#include <sys/mman.h>

namespace Log {
void Write(const char *, ...) {}
} // namespace Log

struct FakeMessage {
    uintptr_t *vtable;
    size_t claimedSize;
    int endAdjustment;
};
static size_t FakeByteSize(void *obj) {
    return static_cast<FakeMessage *>(obj)->claimedSize;
}
static void *FakeSerialize(void *obj, void *out) {
    auto *msg = static_cast<FakeMessage *>(obj);
    memset(out, 0x2A, msg->claimedSize);
    return static_cast<uint8_t *>(out) + msg->claimedSize + msg->endAdjustment;
}
static bool FakeParseFailure(void *, const void *, int) {
    return false;
}
static bool FakeParseSuccess(void *, const void *, int) {
    return true;
}

int main() {
#if !defined(__x86_64__)
    return 77;
#else
    uintptr_t vtable[14] = {};
    vtable[9] = reinterpret_cast<uintptr_t>(&FakeByteSize);
    FakeMessage msg{vtable, 4, 0};
    g_serializeToArray = &FakeSerialize;
    auto bytes = SerializeMessage(&msg);
    assert(bytes.size() == 4 && bytes[0] == 0x2A);
    // Get the pointer width and actual written length right, including a
    // legitimate shorter serialization and a rejected out-of-bounds result.
    msg.endAdjustment = -1;
    assert(SerializeMessage(&msg).size() == 3);
    msg.endAdjustment = 1;
    assert(SerializeMessage(&msg).empty());
    msg.claimedSize = size_t{1} << 32;
    assert(SerializeMessage(&msg).empty());
    assert(SerializeMessage(nullptr).empty());

    g_parseFromArray = &ParseFromArray64Adapter;
    g_parseFromArray64 = &FakeParseFailure;
    const uint8_t wire[] = {8, 1};
    assert(!ParseIntoMessage(&msg, wire, sizeof(wire)));
    g_parseFromArray64 = &FakeParseSuccess;
    assert(ParseIntoMessage(&msg, wire, sizeof(wire)));

    // RPC helpers can run on multiple Steam threads concurrently.
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            FakeMessage local{vtable, 4, 0};
            for (int call = 0; call < 1000; ++call) {
                assert(SerializeMessage(&local).size() == 4);
                assert(ParseIntoMessage(&local, wire, sizeof(wire)));
            }
        });
    }
    for (auto& worker : workers)
        worker.join();

    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto *pages = static_cast<uint8_t *>(
        mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(pages != MAP_FAILED);
    const uint8_t signature[] = {0x41, 0x55, 0x59, 0xF3, 0x29, 0x81, 0x17, 0x61};
    memcpy(pages + page + 16, signature, sizeof(signature));
    assert(mprotect(pages, page, PROT_NONE) == 0);
    assert(mprotect(pages + page, page, PROT_READ | PROT_EXEC) == 0);
    // An unmapped/unreadable gap inside the module span must not be scanned.
    assert(FindUniqueMappedPattern(pages, 2 * page, signature, sizeof(signature)) ==
           pages + page + 16);
    assert(!FindUniqueMappedPattern(pages + page + 16, sizeof(signature) - 1, signature,
                                    sizeof(signature)));
    assert(mprotect(pages + page, page, PROT_READ | PROT_WRITE) == 0);
    memcpy(pages + page + 64, signature, sizeof(signature));
    assert(mprotect(pages + page, page, PROT_READ | PROT_EXEC) == 0);
    assert(!FindUniqueMappedPattern(pages, 2 * page, signature, sizeof(signature)));
    assert(munmap(pages, 2 * page) == 0);
    puts("Linux x86-64 protobuf ABI and unique-mapping scans passed");
#endif
}
