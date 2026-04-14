// fev_project_setup.cpp
// Bulk-convert a LEGO Universe NDAudio directory tree into a FMOD Designer
// project tree: generate .fdp files and extract + convert audio to WAV.
//
// Usage:
//   fev_project_setup <input_audio_dir> <output_dir> [--no-convert]
//
//   input_audio_dir — root of an NDAudio directory tree (contains subdirs with
//                     .fev / .fsb files, matching the layout shipped in client/)
//   output_dir      — destination root for .fdp files and audio (created if absent)
//   --no-convert    — extract MP3 files only; skip ffmpeg WAV conversion
//
// Output layout mirrors the input subdirectory structure:
//   <output_dir>/<RelativeFevDir>/<stem>.fdp
//   <output_dir>/<RelativeFevDir>/<waveform_filename>   (WAV, or MP3 if --no-convert)
//
// Music FEV detection:
//   FEVs with non-empty music_data (FevFile::music_data.items.size() > 0) are
//   music projects whose structure differs from the standard waveform model.
//   Currently skipped pending a reference for static analysis.
//   Known music FEVs: Music_Front_End.fev, music_global.fev / Music_Global.fev.
//
// Audio extraction:
//   All LU PC FSB samples use FMOD_MPEG (mode bit 0x200), so audio data is raw
//   MP3 frames. FMOD Designer expects uncompressed WAV source files, so by
//   default we call `ffmpeg -y -i <sample>.mp3 <waveform>.wav` for each sample.
//   Pass --no-convert to skip ffmpeg and leave .mp3 files in place instead.
//
// FSB matching:
//   For each FEV bank (named e.g. "Ambience_Non-Streaming"), we look for an FSB
//   file in the same directory as the FEV whose stem matches the bank name
//   (case-insensitive). Multiple FSBs per FEV are supported.
//
// Waveform → sample matching:
//   FDP waveform filenames (e.g. "ambience/monument_effects/Fan_01.wav") have
//   a stem that should match the FSB sample name (truncated at 29 chars).
//   Unmatched waveforms are reported but do not abort the run.

#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
// Helpers
// ---------------------------------------------------------------------------

static std::vector<uint8_t> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> d(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(d.data()), sz);
    return d;
}

// Case-insensitive string comparison
static bool icase_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// Lowercase a string
static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// Run a shell command; returns the exit code.
static int run_cmd(const std::string& cmd) {
    return std::system(cmd.c_str());
}

// Quote a path for shell use (single-quotes, escaping any embedded single-quotes).
static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

// FMOD_MPEG codec flag — all LU PC samples use this.
static constexpr uint32_t FMOD_MPEG = 0x00000200;

static const char* sample_extension(uint32_t mode) {
    if (mode & FMOD_MPEG)     return ".mp3";
    if (mode & 0x00000400)    return ".adpcm"; // FMOD_IMAADPCM
    if (mode & 0x00008000)    return ".xma";   // FMOD_XMA
    if (mode & 0x00200000)    return ".dsp";   // FMOD_GCADPCM
    return ".pcm";
}

// ---------------------------------------------------------------------------
// Per-FEV processing
// ---------------------------------------------------------------------------

struct ProcessStats {
    int fdps_written    = 0;
    int samples_extracted = 0;
    int wavs_converted  = 0;
    int music_skipped   = 0;
    int errors          = 0;
};

static void process_fev(
    const fs::path& fev_path,
    const fs::path& input_root,
    const fs::path& output_root,
    bool no_convert,
    ProcessStats& stats)
{
    // Determine relative directory (mirrors input structure in output)
    fs::path rel_dir = fs::relative(fev_path.parent_path(), input_root);
    fs::path out_dir = output_root / rel_dir;
    std::string stem = fev_path.stem().string();

    // Read + parse FEV
    auto fev_data = read_file(fev_path);
    if (fev_data.empty()) {
        fprintf(stderr, "Error: cannot read %s\n", fev_path.string().c_str());
        ++stats.errors;
        return;
    }

    FevFile fev;
    try {
        fev = fev_parse(fev_data);
    } catch (const std::exception& e) {
        fprintf(stderr, "Error parsing %s: %s\n", fev_path.string().c_str(), e.what());
        ++stats.errors;
        return;
    }

    // Skip music FEVs — every FEV has a top-level music_data with a single item
    // containing a "comp" container chunk.  Non-music FEVs have an empty comp
    // (0 sub-items), while real music FEVs (e.g. music_global.fev) have themes,
    // segments, cues, etc. inside the comp.  Detect by checking whether the
    // comp container's sub-items are non-empty.
    bool is_music_fev = false;
    for (const auto& item : fev.music_data.items) {
        for (const auto& chunk : item.chunks) {
            if (chunk.type == "comp") {
                if (auto* sub = std::get_if<std::vector<FevMusicDataItem>>(&chunk.body)) {
                    if (!sub->empty()) is_music_fev = true;
                }
            }
        }
    }
    if (is_music_fev) {
        fprintf(stderr, "Processing (music FEV): %s\n", fev_path.string().c_str());
        ++stats.music_skipped; // still count for reporting
    } else {
        fprintf(stderr, "Processing: %s\n", fev_path.string().c_str());
    }

    // Find sibling FSB files — match by bank name (case-insensitive stem comparison).
    // Gather all .fsb files in the same directory first.
    std::vector<fs::path> sibling_fsbs;
    for (const auto& entry : fs::directory_iterator(fev_path.parent_path())) {
        if (entry.is_regular_file() &&
            lower(entry.path().extension().string()) == ".fsb")
            sibling_fsbs.push_back(entry.path());
    }

    // For each FEV bank, find the matching FSB(s).
    // Bank names may look like "Ambience_Non-Streaming"; FSB stems look like
    // "Ambience_Non-Streaming" — exact match after lower-casing.
    std::unordered_map<std::string /*bank_name*/, std::vector<fs::path>> bank_fsb_map;
    for (const auto& bank : fev.banks) {
        std::string bname_low = lower(bank.name);
        for (const auto& fsb_path : sibling_fsbs) {
            if (lower(fsb_path.stem().string()) == bname_low)
                bank_fsb_map[bank.name].push_back(fsb_path);
        }
    }

    // Load all FSBs and build a unified sample index + raw data map.
    // We need both the parsed header (for FDP metadata) and the raw data
    // (for audio extraction).
    struct SampleData {
        FsbSampleHeader header;
        std::vector<uint8_t> audio; // raw compressed bytes
    };
    std::unordered_map<std::string, SampleData> sample_index; // key: lowercase stem

    for (const auto& bank : fev.banks) {
        auto it = bank_fsb_map.find(bank.name);
        if (it == bank_fsb_map.end()) continue;

        for (const auto& fsb_path : it->second) {
            auto fsb_data = read_file(fsb_path);
            if (fsb_data.empty()) {
                fprintf(stderr, "  Warning: cannot read FSB %s\n",
                        fsb_path.string().c_str());
                continue;
            }
            fsb_decrypt(fsb_data);

            FsbFile fsb;
            try {
                fsb = fsb_parse(fsb_data);
            } catch (const std::exception& e) {
                fprintf(stderr, "  Warning: FSB parse error %s: %s\n",
                        fsb_path.string().c_str(), e.what());
                continue;
            }

            size_t cum_offset = 0;
            for (const auto& s : fsb.samples) {
                size_t sample_start = fsb.data_offset + cum_offset;
                size_t sample_size  = s.compressed_size;

                std::string key = lower(s.name);
                if (!sample_index.count(key)) {
                    SampleData sd;
                    sd.header = s;
                    if (sample_start + sample_size <= fsb_data.size()) {
                        sd.audio.assign(
                            fsb_data.data() + sample_start,
                            fsb_data.data() + sample_start + sample_size);
                    }
                    sample_index[key] = std::move(sd);
                }
                cum_offset += sample_size;
            }
        }
    }

    // Build FSB sample-header index for FDP metadata (reuse fdp_write helpers)
    std::unordered_map<std::string, FsbSampleHeader> fsb_hdr_idx;
    for (const auto& [k, sd] : sample_index)
        fsb_hdr_idx[k] = sd.header;

    // Create output directory and write FDP
    fs::create_directories(out_dir);
    fs::path fdp_path = out_dir / (stem + ".fdp");
    {
        FILE* f = fopen(fdp_path.string().c_str(), "w");
        if (!f) {
            fprintf(stderr, "  Error: cannot write %s\n", fdp_path.string().c_str());
            ++stats.errors;
            return;
        }
        write_fdp(f, fev, fsb_hdr_idx);
        fclose(f);
    }
    fprintf(stderr, "  Wrote %s\n", fdp_path.string().c_str());
    ++stats.fdps_written;

    // Extract audio for each waveform referenced in the FDP.
    // FDP waveform filenames are relative to the FDP location.
    // We match the filename stem against FSB sample names.
    for (const auto& sd_def : fev.sound_definitions) {
        for (const auto& wf : sd_def.waveforms) {
            if (wf.type != FevWaveformType::WAVETABLE) continue;
            const auto& wp = std::get<FevWavetableParams>(wf.params);
            if (wp.filename.empty()) continue;

            // Sanitize waveform filename: some FEV files embed absolute
            // Windows paths (e.g. "Z:/lwo/3_working/Audio/Source/...").
            // Strip any drive letter prefix and convert backslashes.
            std::string sanitized = wp.filename;
            for (char& c : sanitized) if (c == '\\') c = '/';
            if (sanitized.size() >= 2 && sanitized[1] == ':')
                sanitized = sanitized.substr(2);
            while (!sanitized.empty() && sanitized[0] == '/')
                sanitized = sanitized.substr(1);

            // Final destination for this waveform (relative to out_dir)
            fs::path wav_path = out_dir / sanitized;

            // Skip if already present (re-run safety)
            if (fs::exists(wav_path)) continue;

            // Match stem against sample_index
            std::string key = lower(stem_lower(wp.filename));
            auto sit = sample_index.find(key);
            // FSB truncates names at 29 chars — try prefix if full name not found
            if (sit == sample_index.end() && key.size() > 29)
                sit = sample_index.find(key.substr(0, 29));

            if (sit == sample_index.end() || sit->second.audio.empty()) {
                fprintf(stderr, "  Warning: no FSB sample for waveform '%s'\n",
                        wp.filename.c_str());
                continue;
            }

            // Ensure parent directory exists
            fs::create_directories(wav_path.parent_path());

            const char* src_ext = sample_extension(sit->second.header.mode);
            bool is_mp3 = (std::string(src_ext) == ".mp3");

            if (no_convert || !is_mp3) {
                // Write raw audio (MP3 or other) with appropriate extension.
                fs::path raw_path = wav_path;
                raw_path.replace_extension(src_ext);
                {
                    std::ofstream out(raw_path, std::ios::binary);
                    if (!out) {
                        fprintf(stderr, "  Error: cannot write %s\n",
                                raw_path.string().c_str());
                        ++stats.errors;
                        continue;
                    }
                    const auto& audio = sit->second.audio;
                    out.write(reinterpret_cast<const char*>(audio.data()),
                              static_cast<std::streamsize>(audio.size()));
                }
                ++stats.samples_extracted;
            } else {
                // Write MP3 to a temp file, convert to WAV via ffmpeg, delete temp.
                fs::path tmp_mp3 = wav_path;
                tmp_mp3.replace_extension(".mp3");

                {
                    std::ofstream out(tmp_mp3, std::ios::binary);
                    if (!out) {
                        fprintf(stderr, "  Error: cannot write temp %s\n",
                                tmp_mp3.string().c_str());
                        ++stats.errors;
                        continue;
                    }
                    const auto& audio = sit->second.audio;
                    out.write(reinterpret_cast<const char*>(audio.data()),
                              static_cast<std::streamsize>(audio.size()));
                }
                ++stats.samples_extracted;

                // ffmpeg: decode MP3 to 16-bit PCM WAV
                std::string cmd = "ffmpeg -y -hide_banner -loglevel warning -i " +
                                  shell_quote(tmp_mp3.string()) + " " +
                                  shell_quote(wav_path.string());
                int rc = run_cmd(cmd);
                if (rc != 0) {
                    fprintf(stderr, "  Warning: ffmpeg failed (rc=%d) for %s\n",
                            rc, tmp_mp3.string().c_str());
                    // Leave the .mp3 in place so data isn't lost
                } else {
                    fs::remove(tmp_mp3);
                    ++stats.wavs_converted;
                }
            }
        }
    }

    // Bulk extraction fallback: extract ALL samples from ALL matched FSBs when
    // waveform-based extraction didn't find anything. This covers:
    //   - RIFF FEVs where the parser couldn't reach sound definitions (envelope format)
    bool needs_bulk_extract = !is_music_fev &&
        fev.sound_definitions.empty() && !sample_index.empty();
    if (needs_bulk_extract) {
        for (const auto& [key, sd] : sample_index) {
            if (sd.audio.empty()) continue;

            // Place under a flat "samples/" dir, using sample name
            fs::path wav_path = out_dir / "samples" / (std::string(sd.header.name) + ".wav");
            if (fs::exists(wav_path)) continue;
            fs::create_directories(wav_path.parent_path());

            const char* src_ext = sample_extension(sd.header.mode);
            bool is_mp3 = (std::string(src_ext) == ".mp3");

            if (no_convert || !is_mp3) {
                fs::path raw_path = wav_path;
                raw_path.replace_extension(src_ext);
                std::ofstream out(raw_path, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(sd.audio.data()),
                              static_cast<std::streamsize>(sd.audio.size()));
                    ++stats.samples_extracted;
                }
            } else {
                fs::path tmp_mp3 = wav_path;
                tmp_mp3.replace_extension(".mp3");
                {
                    std::ofstream out(tmp_mp3, std::ios::binary);
                    if (!out) continue;
                    out.write(reinterpret_cast<const char*>(sd.audio.data()),
                              static_cast<std::streamsize>(sd.audio.size()));
                }
                ++stats.samples_extracted;

                std::string cmd = "ffmpeg -y -hide_banner -loglevel warning -i " +
                                  shell_quote(tmp_mp3.string()) + " " +
                                  shell_quote(wav_path.string());
                int rc = run_cmd(cmd);
                if (rc != 0) {
                    fprintf(stderr, "  Warning: ffmpeg failed (rc=%d) for %s\n",
                            rc, tmp_mp3.string().c_str());
                } else {
                    fs::remove(tmp_mp3);
                    ++stats.wavs_converted;
                }
            }
        }
    }

    // Music FEV extraction: extract samples referenced by the smpf > str chunk
    // filenames, placing them at paths that match the FDP <Filename> tags.
    // This allows FMOD Designer to find the audio when clicking segments.
    if (is_music_fev) {
        // Parse ALL sibling FSBs (music FEVs may reference samples across
        // multiple FSBs that don't necessarily match bank names).
        std::unordered_map<std::string, SampleData> music_sample_index;
        for (const auto& fsb_path : sibling_fsbs) {
            auto fsb_data = read_file(fsb_path);
            if (fsb_data.empty()) continue;
            fsb_decrypt(fsb_data);

            FsbFile fsb;
            try {
                fsb = fsb_parse(fsb_data);
            } catch (const std::exception& e) {
                fprintf(stderr, "  Warning: FSB parse error %s: %s\n",
                        fsb_path.string().c_str(), e.what());
                continue;
            }

            size_t cum_offset = 0;
            for (const auto& s : fsb.samples) {
                size_t sample_start = fsb.data_offset + cum_offset;
                size_t sample_size  = s.compressed_size;

                std::string key = lower(s.name);
                if (!music_sample_index.count(key)) {
                    SampleData sd;
                    sd.header = s;
                    if (sample_start + sample_size <= fsb_data.size()) {
                        sd.audio.assign(
                            fsb_data.data() + sample_start,
                            fsb_data.data() + sample_start + sample_size);
                    }
                    music_sample_index[key] = std::move(sd);
                }
                cum_offset += sample_size;
            }
        }

        // Walk the FEV music data tree to collect sample filenames from smpf > str
        MusicCollected mc;
        for (const auto& item : fev.music_data.items) {
            std::vector<FevMusicDataChunk> all_chunks;
            for (const auto& ch : item.chunks) all_chunks.push_back(ch);
            collect_music_chunks(all_chunks, mc);
        }

        fprintf(stderr, "  Music sample filenames: %zu, FSB samples available: %zu\n",
                mc.sample_filenames.size(), music_sample_index.size());

        int music_extracted = 0;
        for (const auto& filename : mc.sample_filenames) {
            // Extract stem from the path (e.g. "music/avant gardens/Foo.wav" -> "foo")
            std::string file_stem = stem_lower(filename);

            // Find matching FSB sample
            auto sit = music_sample_index.find(file_stem);
            // FSB truncates names at 29 chars — try prefix match
            if (sit == music_sample_index.end() && file_stem.size() > 29)
                sit = music_sample_index.find(file_stem.substr(0, 29));

            if (sit == music_sample_index.end() || sit->second.audio.empty()) {
                fprintf(stderr, "  Warning: no FSB sample for music filename '%s'\n",
                        filename.c_str());
                continue;
            }

            // Determine output path. Replace .aif with .wav since we extract as WAV.
            std::string out_filename = filename;
            // Normalize backslashes
            for (char& c : out_filename) if (c == '\\') c = '/';
            // Replace .aif extension with .wav
            if (out_filename.size() >= 4) {
                std::string ext = lower(out_filename.substr(out_filename.size() - 4));
                if (ext == ".aif")
                    out_filename = out_filename.substr(0, out_filename.size() - 4) + ".wav";
            }

            fs::path wav_path = out_dir / stem / out_filename;

            // Skip if already present (re-run safety)
            if (fs::exists(wav_path)) {
                ++music_extracted;
                continue;
            }

            fs::create_directories(wav_path.parent_path());

            const char* src_ext = sample_extension(sit->second.header.mode);
            bool is_mp3 = (std::string(src_ext) == ".mp3");

            if (no_convert || !is_mp3) {
                fs::path raw_path = wav_path;
                raw_path.replace_extension(src_ext);
                std::ofstream out(raw_path, std::ios::binary);
                if (out) {
                    out.write(reinterpret_cast<const char*>(sit->second.audio.data()),
                              static_cast<std::streamsize>(sit->second.audio.size()));
                    ++stats.samples_extracted;
                    ++music_extracted;
                }
            } else {
                fs::path tmp_mp3 = wav_path;
                tmp_mp3.replace_extension(".mp3");
                {
                    std::ofstream out(tmp_mp3, std::ios::binary);
                    if (!out) {
                        fprintf(stderr, "  Error: cannot write temp %s\n",
                                tmp_mp3.string().c_str());
                        ++stats.errors;
                        continue;
                    }
                    out.write(reinterpret_cast<const char*>(sit->second.audio.data()),
                              static_cast<std::streamsize>(sit->second.audio.size()));
                }
                ++stats.samples_extracted;

                std::string cmd = "ffmpeg -y -hide_banner -loglevel warning -i " +
                                  shell_quote(tmp_mp3.string()) + " " +
                                  shell_quote(wav_path.string());
                int rc = run_cmd(cmd);
                if (rc != 0) {
                    fprintf(stderr, "  Warning: ffmpeg failed (rc=%d) for %s\n",
                            rc, tmp_mp3.string().c_str());
                } else {
                    fs::remove(tmp_mp3);
                    ++stats.wavs_converted;
                }
                ++music_extracted;
            }
        }
        fprintf(stderr, "  Music samples extracted: %d / %zu\n",
                music_extracted, mc.sample_filenames.size());
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: fev_project_setup <input_audio_dir> <output_dir> [--no-convert]\n"
            "\n"
            "  Walks all .fev files in input_audio_dir, generates .fdp project files\n"
            "  in output_dir (mirroring the input directory structure), extracts audio\n"
            "  from sibling .fsb files, and converts MP3 samples to WAV via ffmpeg.\n"
            "\n"
            "  --no-convert  Extract raw MP3 files instead of converting to WAV.\n"
            "\n"
            "  Music FEVs (with non-empty music_data) are skipped — pending reference.\n"
            "  Known: Music_Front_End.fev, Music_Global.fev / music_global.fev.\n");
        return 1;
    }

    fs::path input_root = argv[1];
    fs::path output_root = argv[2];
    bool no_convert = false;

    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-convert")
            no_convert = true;
    }

    if (!fs::is_directory(input_root)) {
        fprintf(stderr, "Error: input directory does not exist: %s\n",
                input_root.string().c_str());
        return 1;
    }

    fs::create_directories(output_root);

    fprintf(stderr, "Input:  %s\n", input_root.string().c_str());
    fprintf(stderr, "Output: %s\n", output_root.string().c_str());
    if (no_convert)
        fprintf(stderr, "Mode:   extract MP3 (no WAV conversion)\n");
    else
        fprintf(stderr, "Mode:   extract MP3 + convert to WAV via ffmpeg\n");
    fprintf(stderr, "\n");

    ProcessStats stats;

    // Walk all .fev files recursively
    for (const auto& entry : fs::recursive_directory_iterator(input_root)) {
        if (!entry.is_regular_file()) continue;
        if (lower(entry.path().extension().string()) != ".fev") continue;

        process_fev(entry.path(), input_root, output_root, no_convert, stats);
    }

    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "  FDPs written:      %d\n", stats.fdps_written);
    fprintf(stderr, "  Samples extracted: %d\n", stats.samples_extracted);
    fprintf(stderr, "  WAVs converted:    %d\n", stats.wavs_converted);
    fprintf(stderr, "  Music FEVs:        %d\n", stats.music_skipped);
    fprintf(stderr, "  Errors:            %d\n", stats.errors);

    return stats.errors > 0 ? 1 : 0;
}
