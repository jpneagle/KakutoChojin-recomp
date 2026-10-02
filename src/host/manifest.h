// Per-title data produced by tools/gen_manifest.py from the user's XBE.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct FsPatch {
    uint32_t va;
    uint8_t size, reg, offset, movzx_byte;
};

struct HleEntry {
    uint32_t va;
    std::string lib, name, callconv;
    unsigned stack_bytes = 0;  // bytes of stack arguments (callee pops them)
};

struct VarEntry {
    uint32_t va;
    std::string lib, name;
};

struct Manifest {
    std::vector<FsPatch> fs;
    std::vector<HleEntry> hle;
    std::vector<VarEntry> vars;
};

// Address of a library global found by XbSymbolDatabase, or 0.
uint32_t HleVar(const char* lib, const char* name);

Manifest LoadManifest(const std::filesystem::path& path);
void ApplyFsPatches(const Manifest& m);
