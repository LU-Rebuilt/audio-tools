// fev_to_fdp.cpp
// Convert a LEGO Universe .fev (FMOD Event) file into a FMOD Designer Project
// (.fdp) XML file usable with FMOD Designer.
//
// Usage: fev_to_fdp <input.fev> [input1.fsb input2.fsb ...] [output.fdp]
//   fev   — required: parsed for event/group/category/sounddef structure
//   fsb   — optional: decrypted for per-sample metadata (freq, channels, etc.)
//   fdp   — optional output path; defaults to <input_stem>.fdp
//
// GUID sourcing:
//   FevEvent::guid[16]  — the canonical GUID for each event, read from the FEV
//                         binary (FMOD Designer assigns these per-event).
//   All other objects   — banks, categories, groups, sounddefs, sounddeffolders:
//                         their GUIDs live only in the FMOD Designer .fdp project
//                         file, NOT in the compiled FEV/FSB or CDClient.
//                         We generate them deterministically from the object name
//                         using FNV-1a so re-running on the same FEV yields the
//                         same output.

#include <string>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <unordered_map>
#include <vector>

#include "fdp_write.h"

namespace fs = std::filesystem;
using namespace lu::assets;
using namespace fdp_write;

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: fev_to_fdp <input.fev> [input1.fsb input2.fsb ...] [output.fdp]\n"
            "  FSB arguments are identified by .fsb extension (multiple allowed).\n"
            "  The output path is the last non-.fsb argument, or defaults to <stem>.fdp.\n");
        return 1;
    }

    const char* fev_path = argv[1];
    std::vector<std::string> fsb_paths;
    const char* out_path = nullptr;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a.size() >= 4) {
            std::string ext = a.substr(a.size() - 4);
            for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (ext == ".fsb") { fsb_paths.push_back(a); continue; }
        }
        out_path = argv[i];
    }

    std::string default_out;
    if (!out_path) {
        fs::path p(fev_path);
        default_out = p.stem().string() + ".fdp";
        out_path = default_out.c_str();
    }

    // Read FEV
    std::vector<uint8_t> fev_data;
    {
        std::ifstream f(fev_path, std::ios::binary | std::ios::ate);
        if (!f) { fprintf(stderr, "Error: cannot open %s\n", fev_path); return 1; }
        auto sz = f.tellg(); f.seekg(0);
        fev_data.resize(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(fev_data.data()), sz);
    }

    FevFile fev;
    try {
        fev = fev_parse(fev_data);
    } catch (const std::exception& e) {
        fprintf(stderr, "FEV parse error: %s\n", e.what()); return 1;
    }

    // Read all FSBs and merge their sample indices.
    std::vector<FsbFile> fsb_files;
    std::unordered_map<std::string, FsbSampleHeader> fsb_idx;
    for (const auto& path : fsb_paths) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { fprintf(stderr, "Warning: cannot open FSB %s\n", path.c_str()); continue; }
        auto sz = f.tellg(); f.seekg(0);
        std::vector<uint8_t> fsb_data(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(fsb_data.data()), sz);
        fsb_decrypt(fsb_data);
        try {
            fsb_files.push_back(fsb_parse(fsb_data));
            auto idx = build_fsb_index(fsb_files.back());
            for (auto& [k, v] : idx) fsb_idx[k] = v;
        } catch (const std::exception& e) {
            fprintf(stderr, "Warning: FSB parse error %s: %s\n", path.c_str(), e.what());
        }
    }

    // Open output and write FDP
    FILE* out = fopen(out_path, "w");
    if (!out) { fprintf(stderr, "Error: cannot write %s\n", out_path); return 1; }

    write_fdp(out, fev, fsb_idx);
    fclose(out);

    fprintf(stderr, "Wrote %s\n", out_path);
    size_t total_events = 0;
    std::function<void(const FevEventGroup&)> count_events;
    count_events = [&](const FevEventGroup& g) {
        total_events += g.events.size();
        for (const auto& sg : g.subgroups) count_events(sg);
    };
    for (const auto& g : fev.event_groups) count_events(g);

    fprintf(stderr, "  %zu banks, %zu event groups, %zu sound definitions, %zu events\n",
            fev.banks.size(), fev.event_groups.size(),
            fev.sound_definitions.size(), total_events);
    return 0;
}
