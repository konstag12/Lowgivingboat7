// pcport_inventory - read-only inventory of the materials in a folder.
// Classifies files by signature, hashes them, and reports which porting
// operations are possible or blocked. It never modifies or executes anything.
//
//   pcport_inventory <root> [--exclude DIR]... [--expect SHA256]... [--report out.md]
//                    [--max-hash-gb N]
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../core/sha256.hpp"

namespace fs = std::filesystem;
using namespace pcport;

namespace {

struct Entry {
    std::string rel, kind, detail, sha;
    uint64_t size = 0;
};

std::string classify(const fs::path& p, uint64_t size, std::string& detail) {
    std::ifstream f(p, std::ios::binary);
    unsigned char h[0x400] = {0};
    f.read((char*)h, sizeof h);
    const size_t n = (size_t)f.gcount();
    std::string ext = p.extension().string();
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);

    if (n >= 0x208 && !std::memcmp(h + 0x200, "msft-xvd", 8)) {
        detail = "Xbox XVD/XVC container signature; contents are encrypted and signed";
        return "xbox-package";
    }
    if (n >= 0x40 && h[0] == 'M' && h[1] == 'Z') {
        uint32_t lfanew = h[0x3c] | h[0x3d] << 8 | h[0x3e] << 16 | (uint32_t)h[0x3f] << 24;
        if (lfanew + 0x5e < n && !std::memcmp(h + lfanew, "PE\0\0", 4)) {
            const uint16_t mach = h[lfanew + 4] | h[lfanew + 5] << 8;
            const uint16_t ch = h[lfanew + 22] | h[lfanew + 23] << 8;
            const uint16_t sub = h[lfanew + 24 + 68] | h[lfanew + 24 + 69] << 8;
            const char* m = mach == 0x8664 ? "x64" : mach == 0x14c ? "x86" : mach == 0xaa64 ? "arm64" : "other";
            char buf[160];
            std::snprintf(buf, sizeof buf, "PE machine=%s(0x%04x) %s subsystem=%u", m, mach,
                          (ch & 0x2000) ? "DLL" : "EXE", sub);
            detail = buf;
            return "pe-binary";
        }
        detail = "MZ header without PE signature in first 1 KiB";
        return "binary";
    }
    if (n >= 4 && !std::memcmp(h, "PK\x03\x04", 4)) { detail = "ZIP archive"; return "archive"; }
    if (n >= 4 && !std::memcmp(h, "DXBC", 4)) { detail = "DXBC shader bytecode"; return "shader"; }
    if (n >= 4 && !std::memcmp(h, "DXIL", 4)) { detail = "DXIL shader container"; return "shader"; }
    if (n >= 4 && !std::memcmp(h, "DDS ", 4)) { detail = "DDS texture"; return "texture"; }
    if (n >= 4 && (!std::memcmp(h, "RPF", 3))) { detail = "RPF-style archive signature (not parsed)"; return "archive"; }
    if (ext == ".hlsl" || ext == ".fx" || ext == ".glsl") { detail = "shader source"; return "shader"; }
    if (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cs") { detail = "source"; return "source"; }
    if (ext == ".ini" || ext == ".cfg" || ext == ".xml" || ext == ".json" || ext == ".yaml" || ext == ".toml") {
        detail = "configuration/text";
        return "config";
    }
    if (ext == ".sav" || p.filename().string().find("SGTA") == 0) { detail = "save-like name"; return "save"; }
    if (size == 0) { detail = "empty"; return "empty"; }
    detail = "unrecognized signature";
    return "unknown";
}

bool hashFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    Sha256 h;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), (std::streamsize)buf.size());
        if (f.gcount() > 0) h.update(buf.data(), (size_t)f.gcount());
    }
    out = h.hexDigest();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pcport_inventory <root> [--exclude DIR]... [--expect SHA256]... "
                             "[--report out.md] [--max-hash-gb N]\n");
        return 2;
    }
    fs::path root = argv[1];
    std::vector<std::string> excludes = {".git"}, expects;
    std::string report;
    double maxHashGb = 8;
    for (int i = 2; i < argc; i++) {
        if (!std::strcmp(argv[i], "--exclude") && i + 1 < argc) excludes.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--expect") && i + 1 < argc) expects.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--report") && i + 1 < argc) report = argv[++i];
        else if (!std::strcmp(argv[i], "--max-hash-gb") && i + 1 < argc) maxHashGb = std::atof(argv[++i]);
    }
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { std::fprintf(stderr, "not a directory: %s\n", root.string().c_str()); return 1; }

    std::vector<Entry> entries;
    std::map<std::string, uint64_t> bytesByKind;
    std::map<std::string, int> countByKind;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path rel = fs::relative(it->path(), root, ec);
        const std::string top = rel.begin() != rel.end() ? rel.begin()->string() : "";
        bool skip = false;
        for (auto& x : excludes) skip = skip || top == x;
        if (skip) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
        if (!it->is_regular_file(ec)) continue;
        Entry e;
        e.rel = rel.generic_string();
        e.size = it->file_size(ec);
        e.kind = classify(it->path(), e.size, e.detail);
        if ((double)e.size <= maxHashGb * 1e9) hashFile(it->path(), e.sha);
        bytesByKind[e.kind] += e.size;
        countByKind[e.kind]++;
        entries.push_back(e);
    }

    std::string md;
    char line[1024];
    md += "# Inventory of `" + fs::weakly_canonical(fs::absolute(root)).generic_string() + "`\n\n";
    std::snprintf(line, sizeof line, "Files scanned: %zu (excluded top-level:", entries.size());
    md += line;
    for (auto& x : excludes) md += " `" + x + "`";
    md += ")\n\n| kind | files | bytes |\n|---|---:|---:|\n";
    for (auto& kv : countByKind) {
        std::snprintf(line, sizeof line, "| %s | %d | %llu |\n", kv.first.c_str(), kv.second,
                      (unsigned long long)bytesByKind[kv.first]);
        md += line;
    }
    if (entries.empty()) md += "| (none) | 0 | 0 |\n";
    md += "\n## Files\n\n| path | size | kind | detail | sha256 |\n|---|---:|---|---|---|\n";
    for (auto& e : entries) {
        std::snprintf(line, sizeof line, "| `%s` | %llu | %s | %s | `%s` |\n", e.rel.c_str(),
                      (unsigned long long)e.size, e.kind.c_str(), e.detail.c_str(),
                      e.sha.empty() ? "(not hashed)" : e.sha.c_str());
        md += line;
    }
    if (!expects.empty()) {
        md += "\n## Expected checksums\n\n| sha256 | found |\n|---|---|\n";
        for (auto& x : expects) {
            std::string where = "**not present**";
            for (auto& e : entries) if (e.sha == x) where = "`" + e.rel + "`";
            md += "| `" + x + "` | " + where + " |\n";
        }
    }
    auto has = [&](const char* k) { return countByKind.count(k) > 0; };
    md += "\n## Component checklist\n\n| component | present | operations blocked without it |\n|---|---|---|\n";
    md += std::string("| game executable (PE or Xbox package) | ") + (has("pe-binary") || has("xbox-package") ? "yes" : "no") +
          " | import/API analysis, any compatibility layer, first frame of game render |\n";
    md += std::string("| game data archives | ") + (has("archive") ? "yes" : "no") +
          " | asset/world loading, main menu, world load, character control |\n";
    md += std::string("| shaders (bytecode or source) | ") + (has("shader") ? "yes" : "no") +
          " | shader translation, shader-compile stutter profiling |\n";
    md += std::string("| configuration | ") + (has("config") ? "yes" : "no") + " | settings mapping |\n";
    md += std::string("| save data | ") + (has("save") ? "yes" : "no") + " | save-load test |\n";
    md += std::string("| source code | ") + (has("source") ? "yes" : "no") + " | native Windows build target |\n";
    if (has("xbox-package"))
        md += "\nNote: an XVD/XVC container is encrypted and signed; its contents are not readable "
              "with public tools and this tool does not attempt to decrypt it.\n";

    std::fputs(md.c_str(), stdout);
    if (!report.empty()) {
        std::ofstream o(report, std::ios::binary);
        o << md;
    }
    return 0;
}
