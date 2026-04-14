// fsb_extract.cpp
// Extract audio samples from a LEGO Universe FSB (FMOD Sound Bank) file.
//
// Usage: fsb_extract <input.fsb> [output_dir]
//   input.fsb  — encrypted or plaintext FSB4 file
//   output_dir — directory to write samples into (default: current directory)
//
// Each sample is written as "<output_dir>/<sample_name>.<ext>" where ext is
// determined by the per-sample FMOD_MODE flags:
//   FMOD_MPEG (0x200)     → .mp3  (all LU PC samples use this codec)
//   FMOD_IMAADPCM (0x400) → .adpcm
//   FMOD_XMA  (0x8000)    → .xma
//   FMOD_GCADPCM (0x200000) → .dsp
//   Other                 → .pcm
//
// The MP3 data stored in LU FSBs is raw back-to-back MP3 frames — no
// container — and plays directly in any MP3-aware player or ffmpeg.
//
// To convert all extracted .mp3 files to .wav for FMOD Designer use:
//   for f in *.mp3; do ffmpeg -y -i "$f" "${f%.mp3}.wav"; done

#include "fmod/fsb/fsb_reader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace lu::assets;

// FMOD_MODE codec flag constants (only the bits we care about for extension selection).
// Values from FMOD 3.x/4.x SDK fmod.h.
static constexpr uint32_t FMOD_MPEG       = 0x00000200;
static constexpr uint32_t FMOD_IMAADPCM   = 0x00000400;
static constexpr uint32_t FMOD_XMA        = 0x00008000;
static constexpr uint32_t FMOD_GCADPCM    = 0x00200000;

static const char* sample_extension(uint32_t mode) {
    if (mode & FMOD_MPEG)     return ".mp3";
    if (mode & FMOD_IMAADPCM) return ".adpcm";
    if (mode & FMOD_XMA)      return ".xma";
    if (mode & FMOD_GCADPCM)  return ".dsp";
    return ".pcm";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: fsb_extract <input.fsb> [output_dir]\n"
            "  Extracts all audio samples from an FSB4 file.\n"
            "  LU FSBs are auto-decrypted with the built-in LU key.\n"
            "  Output: <output_dir>/<sample_name>.mp3 (or .pcm/.xma etc.)\n");
        return 1;
    }

    const char* fsb_path = argv[1];
    fs::path out_dir = (argc >= 3) ? fs::path(argv[2]) : fs::current_path();

    // Read input
    std::vector<uint8_t> data;
    {
        std::ifstream f(fsb_path, std::ios::binary | std::ios::ate);
        if (!f) {
            fprintf(stderr, "Error: cannot open %s\n", fsb_path);
            return 1;
        }
        auto sz = f.tellg(); f.seekg(0);
        data.resize(static_cast<size_t>(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }

    // Decrypt in-place (no-op if plaintext, returns false without modifying)
    bool was_encrypted = fsb_decrypt(data);
    if (was_encrypted)
        fprintf(stderr, "Decrypted FSB with LU key.\n");

    // Parse
    FsbFile fsb;
    try {
        fsb = fsb_parse(data);
    } catch (const std::exception& e) {
        fprintf(stderr, "FSB parse error: %s\n", e.what());
        return 1;
    }

    fprintf(stderr, "FSB: %u samples, data offset 0x%zx, data size %u bytes\n",
            fsb.num_samples, fsb.data_offset, fsb.data_size);

    fs::create_directories(out_dir);

    // Extract each sample.
    // Audio data layout (FSB4):
    //   bytes [data_offset .. data_offset + data_size) = concatenated sample data
    //   sample[i] occupies [data_offset + cumulative_offset, + compressed_size)
    size_t cumulative_offset = 0;
    int written = 0;
    int skipped = 0;

    for (const auto& s : fsb.samples) {
        const char* ext = sample_extension(s.mode);

        // Sanitize sample name: replace characters illegal in filenames
        std::string safe_name = s.name;
        for (char& c : safe_name) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' ||
                c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
                c = '_';
        }
        if (safe_name.empty()) safe_name = "sample";

        fs::path out_path = out_dir / (safe_name + ext);

        size_t sample_start = fsb.data_offset + cumulative_offset;
        size_t sample_size  = s.compressed_size;

        if (sample_start + sample_size > data.size()) {
            fprintf(stderr, "Warning: sample '%s' data out of bounds (offset %zu + size %zu > file %zu) — skipping\n",
                    s.name.c_str(), sample_start, sample_size, data.size());
            ++skipped;
            cumulative_offset += sample_size;
            continue;
        }

        {
            std::ofstream out(out_path, std::ios::binary);
            if (!out) {
                fprintf(stderr, "Error: cannot write %s\n", out_path.string().c_str());
                ++skipped;
                cumulative_offset += sample_size;
                continue;
            }
            out.write(reinterpret_cast<const char*>(data.data() + sample_start),
                      static_cast<std::streamsize>(sample_size));
        }

        fprintf(stderr, "  [%3d] %-30s %6u Hz  %u ch  %7zu bytes → %s\n",
                written,
                s.name.c_str(),
                s.default_freq,
                s.num_channels,
                sample_size,
                out_path.filename().string().c_str());

        cumulative_offset += sample_size;
        ++written;
    }

    fprintf(stderr, "\nExtracted %d samples", written);
    if (skipped) fprintf(stderr, " (%d skipped)", skipped);
    fprintf(stderr, " to %s\n", out_dir.string().c_str());
    return skipped > 0 ? 2 : 0;
}
