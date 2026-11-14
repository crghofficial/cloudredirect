#include "kv_resolver64.h"
#include <cstdlib>
#include <elf.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sys/mman.h>
#include <vector>

static void Require(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
int main(int argc, char **argv) {
    // Missing and ambiguous signatures must never select a write target.
    std::vector<uint8_t> fixture = {1, 2, 3, 0, 1, 2, 3};
    auto at = reinterpret_cast<uintptr_t>(fixture.data());
    std::vector<LinuxKv64::Range> small = {{at, at + fixture.size(), true, false}};
    Require(LinuxKv64::Unique(small, {1, 2, 3}) == 0, "ambiguous signature accepted");
    Require(LinuxKv64::Unique(small, {9}) == 0, "missing signature accepted");
    Require(LinuxKv64::Unique(small, {1, 2, 3, 0}) == at, "unique signature rejected");
    Require(!LinuxKv64::Contains(small, UINTPTR_MAX - 1, 4), "overflow range accepted");
    LinuxKv64::Range clipped;
    Require(LinuxKv64::ClipRange(0x332d000, 0x337a000, 0, 0x3379cc0, false, true, clipped),
            "page-aligned BSS map dropped");
    Require(clipped.end == 0x3379cc0 && clipped.writable &&
                LinuxKv64::Contains({clipped}, 0x332d0b0, sizeof(void *), false, true),
            "engine global lost in clipped BSS map");
    Require(!LinuxKv64::ClipRange(0x337a000, 0x3380000, 0, 0x3379cc0, false, true, clipped),
            "unrelated mapping accepted");
    LinuxKv64::Addresses a;
    Require(!LinuxKv64::Resolve(small, a), "partial image accepted");
    if (argc != 2) {
        std::cout << "synthetic resolver checks passed\n";
        return 0;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    Require(bytes.size() >= sizeof(Elf64_Ehdr), "missing ELF");
    auto *eh = reinterpret_cast<const Elf64_Ehdr *>(bytes.data());
    Require(!std::memcmp(eh->e_ident, ELFMAG, SELFMAG) && eh->e_machine == EM_X86_64,
            "requires x86-64 ELF");
    Require(eh->e_phoff + eh->e_phnum * sizeof(Elf64_Phdr) <= bytes.size(), "invalid phdrs");
    auto *ph = reinterpret_cast<const Elf64_Phdr *>(bytes.data() + eh->e_phoff);
    size_t imageSize = 0;
    for (size_t i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD)
            imageSize = std::max(imageSize, size_t(ph[i].p_vaddr + ph[i].p_memsz));
    auto *image = static_cast<uint8_t *>(
        mmap(nullptr, imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Require(image != MAP_FAILED, "mmap failed");
    auto base = reinterpret_cast<uintptr_t>(image);
    std::vector<LinuxKv64::Range> ranges;
    for (size_t i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            Require(ph[i].p_offset + ph[i].p_filesz <= bytes.size(), "invalid segment");
            std::memcpy(image + ph[i].p_vaddr, bytes.data() + ph[i].p_offset, ph[i].p_filesz);
            ranges.push_back({base + ph[i].p_vaddr, base + ph[i].p_vaddr + ph[i].p_memsz,
                              bool(ph[i].p_flags & PF_X), bool(ph[i].p_flags & PF_W)});
        }
    Require(LinuxKv64::Resolve(ranges, a), "actual Steam ELF resolver failed");
    std::cout << std::hex << "engine=" << a.engineGlobal - base << " cache=" << a.cacheOffset
              << " read=" << a.readConfig - base << " section=" << a.getSection - base
              << " find=" << a.findKey - base << " u64=" << a.setU64 - base
              << " int=" << a.setInt - base << " string=" << a.setString - base << '\n';
    uint8_t original = *reinterpret_cast<uint8_t *>(a.setU64);
    *reinterpret_cast<uint8_t *>(a.setU64) = 0xcc;
    LinuxKv64::Addresses corrupted;
    Require(!LinuxKv64::Resolve(ranges, corrupted), "corrupted setter accepted");
    *reinterpret_cast<uint8_t *>(a.setU64) = original;
    original = *reinterpret_cast<uint8_t *>(a.readConfig + 0x80);
    *reinterpret_cast<uint8_t *>(a.readConfig + 0x80) = 0xcc;
    Require(!LinuxKv64::Resolve(ranges, corrupted), "corrupted helper call accepted");
    *reinterpret_cast<uint8_t *>(a.readConfig + 0x80) = original;
    munmap(image, imageSize);
    std::cout << "Steam image resolution and corrupted-setter rejection passed\n";
}
