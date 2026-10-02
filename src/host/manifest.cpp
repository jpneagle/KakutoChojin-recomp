#include "manifest.h"

#include <fstream>
#include <sstream>

#include "host.h"

namespace {
std::vector<VarEntry> g_vars;
}

Manifest LoadManifest(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) Fatal("cannot open manifest %s", path.string().c_str());
    Manifest m;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "fs") {
            unsigned va, size, reg, off, mz;
            ss >> std::hex >> va >> std::dec >> size >> reg >> std::hex >> off >> std::dec >> mz;
            m.fs.push_back({va, uint8_t(size), uint8_t(reg), uint8_t(off), uint8_t(mz)});
        } else if (kind == "hle") {
            HleEntry e;
            ss >> std::hex >> e.va >> e.lib >> e.name >> e.callconv >> std::dec >> e.stack_bytes;
            m.hle.push_back(e);
        } else if (kind == "var") {
            VarEntry v;
            ss >> std::hex >> v.va >> v.lib >> v.name;
            m.vars.push_back(v);
        } else {
            Fatal("manifest line %d: unknown record '%s'", lineno, kind.c_str());
        }
        if (ss.fail()) Fatal("manifest line %d: malformed", lineno);
    }
    Log("manifest: %zu fs patches, %zu hle symbols, %zu variables", m.fs.size(), m.hle.size(), m.vars.size());
    g_vars = m.vars;
    return m;
}

uint32_t HleVar(const char* lib, const char* name) {
    for (const VarEntry& v : g_vars)
        if (v.lib == lib && v.name == name) return v.va;
    return 0;
}
