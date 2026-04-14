#pragma once
// fdp_write.h — shared FDP XML generation helpers.
// Shared by fev_to_fdp (single-file converter) and fev_project_setup (bulk runner).
// All functions are static so each TU gets its own copy — no ODR concerns.

#include "fmod/fev/fev_reader.h"
#include "fmod/fsb/fsb_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fdp_write {

using namespace lu::assets;

// ---------------------------------------------------------------------------
// GUID helpers
// ---------------------------------------------------------------------------

// Format a raw 16-byte FMOD GUID as {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}.
// FMOD stores GUIDs as four packed little-endian fields (Data1 u32, Data2 u16,
// Data3 u16, Data4 u8[8]), matching the Windows GUID layout.
static std::string format_fmod_guid(const uint8_t g[16]) {
    char buf[40];
    snprintf(buf, sizeof(buf),
        "{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        g[3], g[2], g[1], g[0],
        g[5], g[4],
        g[7], g[6],
        g[8], g[9],
        g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

// Generate a deterministic pseudo-UUID from a name string via FNV-1a.
static std::string guid_from_name(const std::string& name) {
    constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
    uint64_t h = FNV_OFFSET;
    for (unsigned char c : name) { h ^= c; h *= FNV_PRIME; }
    uint64_t h2 = (h ^ 0xc0ffeedead'beefULL) * FNV_PRIME;

    uint8_t g[16];
    memcpy(g,     &h,  8);
    memcpy(g + 8, &h2, 8);
    g[6] = static_cast<uint8_t>((g[6] & 0x0f) | 0x40);
    g[8] = static_cast<uint8_t>((g[8] & 0x3f) | 0x80);

    char buf[40];
    snprintf(buf, sizeof(buf),
        "{%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
        g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return buf;
}

// ---------------------------------------------------------------------------
// XML string escaping
// ---------------------------------------------------------------------------

static std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Volume unit conversion
// ---------------------------------------------------------------------------

static double linear_to_db(float linear) {
    if (linear <= 0.0f) return -100.0;
    double db = 20.0 * std::log10(static_cast<double>(linear));
    return std::round(db * 100.0) / 100.0;
}

// ---------------------------------------------------------------------------
// Enum-to-string helpers
// ---------------------------------------------------------------------------

static const char* bank_type_str(FevBankLoadMode m) {
    switch (m) {
        case FevBankLoadMode::STREAM_FROM_DISK:       return "Stream";
        case FevBankLoadMode::DECOMPRESS_INTO_MEMORY: return "DecompressedSample";
        case FevBankLoadMode::LOAD_INTO_MEMORY:       return "Sample";
    }
    return "Sample";
}

static const char* max_pb_str(FevMaxPlaybackBehavior b) {
    switch (b) {
        case FevMaxPlaybackBehavior::STEAL_OLDEST:          return "Steal_oldest";
        case FevMaxPlaybackBehavior::STEAL_NEWEST:          return "Steal_newest";
        case FevMaxPlaybackBehavior::STEAL_QUIETEST:        return "Steal_quietest";
        case FevMaxPlaybackBehavior::JUST_FAIL:             return "Just_fail";
        case FevMaxPlaybackBehavior::JUST_FAIL_IF_QUIETEST: return "Just_fail_if_quietest";
    }
    return "Steal_oldest";
}

static const char* max_pb_event_str(FevEventMaxPlaybackBehavior b) {
    switch (b) {
        case FevEventMaxPlaybackBehavior::STEAL_OLDEST:          return "Steal_oldest";
        case FevEventMaxPlaybackBehavior::STEAL_NEWEST:          return "Steal_newest";
        case FevEventMaxPlaybackBehavior::STEAL_QUIETEST:        return "Steal_quietest";
        case FevEventMaxPlaybackBehavior::JUST_FAIL:             return "Just_fail";
        case FevEventMaxPlaybackBehavior::JUST_FAIL_IF_QUIETEST: return "Just_fail_if_quietest";
    }
    return "Steal_oldest";
}

static const char* play_mode_str(FevPlayMode m) {
    switch (m) {
        case FevPlayMode::SEQUENTIAL:              return "sequential";
        case FevPlayMode::RANDOM:                  return "random";
        case FevPlayMode::RANDOM_NO_REPEAT:        return "randomnorepeat";
        case FevPlayMode::SEQUENTIAL_EVENT_RESTART:return "sequentialeventrestart";
        case FevPlayMode::SHUFFLE:                 return "shuffle";
        case FevPlayMode::PROGRAMMER_SELECTED:     return "programmerselected";
        case FevPlayMode::SHUFFLE_GLOBAL:          return "shuffleglobal";
        case FevPlayMode::SEQUENTIAL_GLOBAL:       return "sequentialglobal";
    }
    return "sequential";
}

// ---------------------------------------------------------------------------
// Write helpers
// ---------------------------------------------------------------------------

// FDP files use no indentation (matching FMOD Designer's output format).
static void tag(FILE* f, int /*d*/, const char* t, const std::string& v) {
    fprintf(f, "<%s>%s</%s>\n", t, xml_escape(v).c_str(), t);
}
static void tagi(FILE* f, int /*d*/, const char* t, long long v) {
    fprintf(f, "<%s>%lld</%s>\n", t, v, t);
}
static void tagf(FILE* f, int /*d*/, const char* t, double v) {
    fprintf(f, "<%s>%g</%s>\n", t, v, t);
}
static void open(FILE* f, int /*d*/, const char* t) {
    fprintf(f, "<%s>\n", t);
}
static void close(FILE* f, int /*d*/, const char* t) {
    fprintf(f, "</%s>\n", t);
}

// ---------------------------------------------------------------------------
// Build bank→waveform filename index from sounddefs
// ---------------------------------------------------------------------------

static std::unordered_map<std::string, std::vector<std::string>>
build_bank_waveform_index(const FevFile& fev) {
    std::unordered_map<std::string, std::vector<std::string>> idx;
    for (const auto& sd : fev.sound_definitions) {
        for (const auto& wf : sd.waveforms) {
            if (wf.type == FevWaveformType::WAVETABLE) {
                const auto& wp = std::get<FevWavetableParams>(wf.params);
                if (!wp.bank_name.empty() && !wp.filename.empty()) {
                    auto& vec = idx[wp.bank_name];
                    if (std::find(vec.begin(), vec.end(), wp.filename) == vec.end())
                        vec.push_back(wp.filename);
                }
            }
        }
    }
    return idx;
}

// ---------------------------------------------------------------------------
// Build FSB sample index: stem-lowercase → FsbSampleHeader
// ---------------------------------------------------------------------------

static std::string stem_lower(const std::string& path) {
    auto p = path.find_last_of("/\\");
    std::string base = (p == std::string::npos) ? path : path.substr(p + 1);
    auto dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    for (char& c : base) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return base;
}

static std::unordered_map<std::string, FsbSampleHeader>
build_fsb_index(const FsbFile& fsb) {
    std::unordered_map<std::string, FsbSampleHeader> idx;
    for (const auto& s : fsb.samples) {
        std::string k = s.name;
        for (char& c : k) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        idx[k] = s;
    }
    return idx;
}

// ---------------------------------------------------------------------------
// Write: event category tree (recursive)
// ---------------------------------------------------------------------------

static void write_event_category(FILE* f, int d, const FevEventCategory& cat) {
    open(f, d, "eventcategory");
    tag(f, d+1, "name", cat.name);
    tag(f, d+1, "guid", guid_from_name("cat:" + cat.name));
    tagf(f, d+1, "volume_db", linear_to_db(cat.volume));
    tagf(f, d+1, "pitch", cat.pitch);
    tagi(f, d+1, "maxplaybacks", cat.max_streams);
    tag(f, d+1, "maxplaybacks_behavior", max_pb_str(cat.max_playback_behavior));
    tag(f, d+1, "notes", "");
    tagi(f, d+1, "open", 0);
    for (const auto& sub : cat.subcategories)
        write_event_category(f, d+1, sub);
    close(f, d, "eventcategory");
}

// ---------------------------------------------------------------------------
// Write: layers and events
// ---------------------------------------------------------------------------

static void write_layer(FILE* f, int d, const FevLayer& layer, int idx,
                        const FevFile& fev, bool is_simple) {
    open(f, d, "layer");
    char lname[16]; snprintf(lname, sizeof(lname), "layer%02d", idx);
    tag(f, d+1, "name", lname);
    tagi(f, d+1, "height", 100);
    tagi(f, d+1, "envelope_nextid", static_cast<int>(layer.effect_envelopes.size()));
    // layer_flags[0] bit 0 = mute, layer_flags[0] bit 1 = solo
    tagi(f, d+1, "mute", (!is_simple && (layer.layer_flags[0] & 0x01)) ? 1 : 0);
    tagi(f, d+1, "solo", (!is_simple && (layer.layer_flags[0] & 0x02)) ? 1 : 0);
    tagi(f, d+1, "soundlock", 0);
    tagi(f, d+1, "envlock", 0);
    tagi(f, d+1, "priority", is_simple ? -1 : layer.priority);

    for (const auto& si : layer.sound_instances) {
        std::string sname;
        if (si.sound_definition_index < fev.sound_definitions.size())
            sname = fev.sound_definitions[si.sound_definition_index].name;

        open(f, d+1, "sound");
        tag(f, d+2, "name", sname);
        tagi(f, d+2, "x", static_cast<long long>(si.start_position));
        tagf(f, d+2, "width", si.length > 0 ? si.length : 1.0);
        tagi(f, d+2, "startmode", static_cast<int>(si.start_mode));
        tagi(f, d+2, "loopmode", static_cast<int>(si.loop_mode));
        tagi(f, d+2, "loopcount2", si.loop_count);
        tagi(f, d+2, "autopitchenabled",
             si.autopitch_enabled == FevAutopitchEnabled::YES ? 1 : 0);
        tagi(f, d+2, "autopitchparameter", static_cast<int>(si.autopitch_parameter));
        tagf(f, d+2, "autopitchreference", si.autopitch_reference);
        tagf(f, d+2, "autopitchatzero", si.autopitch_at_min);
        tagf(f, d+2, "finetune", si.fine_tune);
        tagf(f, d+2, "volume", si.volume);
        tagi(f, d+2, "fadeintype", si.fade_in_type);
        tagi(f, d+2, "fadeouttype", si.fade_out_type);
        close(f, d+1, "sound");
    }

    for (const char* plat : {"_PC","_XBOX","_XBOX360","_GC","_PS2","_PSP","_PS3","_WII"}) {
        std::string etag = std::string(plat) + "_enable";
        fprintf(f, "<%s>1</%s>\n", etag.c_str(), etag.c_str());
    }

    close(f, d, "layer");
}

static void write_event(FILE* f, int d, const FevEvent& ev, const FevFile& fev) {
    bool is_simple = (ev.event_type == FevEventType::SIMPLE);

    open(f, d, "event");
    tag(f, d+1, "name", ev.name);
    tag(f, d+1, "guid", format_fmod_guid(ev.guid));
    tagi(f, d+1, "parameter_nextid", static_cast<int>(ev.parameters.size()));
    tagi(f, d+1, "layer_nextid", static_cast<int>(ev.layers.size()));

    for (int i = 0; i < static_cast<int>(ev.layers.size()); ++i)
        write_layer(f, d+1, ev.layers[i], i, fev, is_simple);

    // Event parameters — each parameter defines a control axis for the event.
    // Loop mode mapping from FevEventParameterFlags:
    //   flags.loop                  → loopmode=0 (loop)
    //   flags.oneshot_and_stop_event→ loopmode=1 (oneshot_and_stop_event)
    //   flags.oneshot               → loopmode=2 (oneshot)
    for (const auto& param : ev.parameters) {
        open(f, d+1, "parameter");
        tag(f, d+2, "name", param.name);
        tagf(f, d+2, "velocity", param.velocity);
        tagf(f, d+2, "min", param.minimum_value);
        tagf(f, d+2, "max", param.maximum_value);
        int loopmode = 0; // default: loop
        if (param.flags.oneshot_and_stop_event) loopmode = 1;
        else if (param.flags.oneshot)           loopmode = 2;
        tagi(f, d+2, "loopmode", loopmode);
        tag(f, d+2, "primary", param.flags.primary ? "Yes" : "No");
        tag(f, d+2, "keyoffonsilence", param.flags.keyoff_on_silence ? "Yes" : "No");
        tagf(f, d+2, "seekspeed", param.seek_speed);
        close(f, d+1, "parameter");
    }

    tagf(f, d+1, "car_rpm", 0);
    tagf(f, d+1, "car_rpmsmooth", 0.075);
    tagf(f, d+1, "car_loadsmooth", 0.05);
    tagf(f, d+1, "car_loadscale", 6);
    tagi(f, d+1, "car_dialog", 0);

    tagf(f, d+1, "volume_db", linear_to_db(ev.volume));
    tagf(f, d+1, "pitch", ev.pitch);
    tag(f, d+1, "pitch_units", "Octaves");
    tagf(f, d+1, "pitch_randomization", ev.pitch_randomization);
    tag(f, d+1, "pitch_randomization_units", "Octaves");
    tagf(f, d+1, "volume_randomization", ev.volume_randomization);
    tagi(f, d+1, "priority", ev.priority);
    tagi(f, d+1, "maxplaybacks", ev.max_playbacks);
    tag(f, d+1, "maxplaybacks_behavior", max_pb_event_str(ev.max_playbacks_behavior));
    tagi(f, d+1, "stealpriority", ev.steal_priority);

    const char* mode = "x_2d";
    if (ev.threed_flags.mode_3d) mode = "x_3d";
    else if (ev.threed_flags.mode_2d) mode = "x_2d";
    tag(f, d+1, "mode", mode);

    tag(f, d+1, "ignoregeometry", ev.threed_flags.ignore_geometry ? "Yes" : "No");
    tag(f, d+1, "unique", ev.threed_flags.unique ? "Yes" : "No");

    // Rolloff type from event_flags (byte 1 bits: 0x01=inverse, 0x02=linear-squared,
    // 0x04=linear, 0x08=logarithmic) and byte 2 bit 7 (0x80 = custom rolloff).
    // Falls back to threed_flags.rolloff_linear/rolloff_logarithmic if event_flags
    // has no rolloff bits set.
    const char* rolloff = "Logarithmic";
    uint8_t rf_byte1 = ev.event_flags.raw[1];
    uint8_t rf_byte2 = ev.event_flags.raw[2];
    if (rf_byte2 & 0x80)      rolloff = "Custom";
    else if (rf_byte1 & 0x04) rolloff = "Linear";
    else if (rf_byte1 & 0x02) rolloff = "LinearSquare";
    else if (rf_byte1 & 0x01) rolloff = "Inverse";
    else if (rf_byte1 & 0x08) rolloff = "Logarithmic";
    else if (ev.threed_flags.rolloff_linear) rolloff = "Linear";
    tag(f, d+1, "rolloff", rolloff);

    tagf(f, d+1, "mindistance", ev.threed_min_distance);
    tagf(f, d+1, "maxdistance", ev.threed_max_distance);

    tag(f, d+1, "headrelative",
        ev.threed_flags.position_head_relative ? "Head_relative" : "World_relative");
    tag(f, d+1, "oneshot", ev.event_flags.oneshot ? "Yes" : "No");
    tag(f, d+1, "istemplate", "No");
    tag(f, d+1, "usetemplate", "");
    tag(f, d+1, "notes", "");
    tag(f, d+1, "category", ev.category);

    tagf(f, d+1, "position_randomization", ev.threed_position_randomization);
    tagf(f, d+1, "speaker_l",   ev.speaker_l);
    tagf(f, d+1, "speaker_c",   ev.speaker_c);
    tagf(f, d+1, "speaker_r",   ev.speaker_r);
    tagf(f, d+1, "speaker_ls",  ev.speaker_ls);
    tagf(f, d+1, "speaker_rs",  ev.speaker_rs);
    tagf(f, d+1, "speaker_lb",  ev.speaker_lr);
    tagf(f, d+1, "speaker_rb",  ev.speaker_rr);
    tagf(f, d+1, "speaker_lfe", ev.speaker_lfe);
    tagi(f, d+1, "speaker_config", 0);
    tagi(f, d+1, "speaker_pan_r", 1);
    tagi(f, d+1, "speaker_pan_theta", 0);
    tagf(f, d+1, "cone_inside_angle",     ev.threed_cone_inside_angle);
    tagf(f, d+1, "cone_outside_angle",    ev.threed_cone_outside_angle);
    tagf(f, d+1, "cone_outside_volumedb", linear_to_db(ev.threed_cone_outside_volume));
    tagf(f, d+1, "doppler_scale",         ev.threed_doppler_factor);
    tagf(f, d+1, "reverbdrylevel_db",     ev.reverb_dry_level);
    tagf(f, d+1, "reverblevel_db",        ev.reverb_wet_level);
    tagf(f, d+1, "speaker_spread",        ev.threed_speaker_spread);
    tagf(f, d+1, "panlevel3d",            ev.threed_pan_level);
    tagi(f, d+1, "fadein_time",  ev.fade_in_time);
    tagi(f, d+1, "fadeout_time", ev.fade_out_time);
    tagf(f, d+1, "spawn_intensity",               ev.spawn_intensity);
    tagf(f, d+1, "spawn_intensity_randomization",  ev.spawn_intensity_randomization);

    static const struct { const char* name; int val; } kTemplateProps[] = {
        {"TEMPLATE_PROP_LAYERS",                      1},
        {"TEMPLATE_PROP_KEEP_EFFECTS_PARAMS",         1},
        {"TEMPLATE_PROP_VOLUME",                      0},
        {"TEMPLATE_PROP_PITCH",                       1},
        {"TEMPLATE_PROP_PITCH_RANDOMIZATION",         1},
        {"TEMPLATE_PROP_VOLUME_RANDOMIZATION",        1},
        {"TEMPLATE_PROP_PRIORITY",                    1},
        {"TEMPLATE_PROP_MAX_PLAYBACKS",               1},
        {"TEMPLATE_PROP_MAX_PLAYBACKS_BEHAVIOR",      1},
        {"TEMPLATE_PROP_STEAL_PRIORITY",              1},
        {"TEMPLATE_PROP_MODE",                        1},
        {"TEMPLATE_PROP_IGNORE_GEOMETRY",             1},
        {"TEMPLATE_PROP_X_3D_ROLLOFF",                1},
        {"TEMPLATE_PROP_X_3D_MIN_DISTANCE",           1},
        {"TEMPLATE_PROP_X_3D_MAX_DISTANCE",           1},
        {"TEMPLATE_PROP_X_3D_POSITION",               1},
        {"TEMPLATE_PROP_X_3D_POSITION_RANDOMIZATION", 1},
        {"TEMPLATE_PROP_X_3D_CONE_INSIDE_ANGLE",      1},
        {"TEMPLATE_PROP_X_3D_CONE_OUTSIDE_ANGLE",     1},
        {"TEMPLATE_PROP_X_3D_CONE_OUTSIDE_VOLUME",    1},
        {"TEMPLATE_PROP_X_3D_DOPPLER_FACTOR",         1},
        {"TEMPLATE_PROP_REVERB_WET_LEVEL",            1},
        {"TEMPLATE_PROP_REVERB_DRY_LEVEL",            1},
        {"TEMPLATE_PROP_X_3D_SPEAKER_SPREAD",         1},
        {"TEMPLATE_PROP_X_3D_PAN_LEVEL",              1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_L",              1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_C",              1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_R",              1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_LS",             1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_RS",             1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_LR",             1},
        {"TEMPLATE_PROP_X_2D_SPEAKER_RR",             1},
        {"TEMPLATE_PROP_X_SPEAKER_LFE",               1},
        {"TEMPLATE_PROP_ONESHOT",                     1},
        {"TEMPLATE_PROP_FADEIN_TIME",                 1},
        {"TEMPLATE_PROP_FADEOUT_TIME",                1},
        {"TEMPLATE_PROP_NOTES",                       1},
        {"TEMPLATE_PROP_USER_PROPERTIES",             1},
        {"TEMPLATE_PROP_CATEGORY",                    0},
    };
    for (const auto& tp : kTemplateProps)
        tagi(f, d+1, tp.name, tp.val);

    for (const char* plat : {"_PC","_XBOX","_XBOX360","_GC","_PS2","_PSP","_PS3","_WII"}) {
        std::string etag = std::string(plat) + "_enabled";
        fprintf(f, "<%s>1</%s>\n", etag.c_str(), etag.c_str());
    }

    close(f, d, "event");
}

// ---------------------------------------------------------------------------
// Write: event group tree (recursive)
// ---------------------------------------------------------------------------

static void write_event_group(FILE* f, int d, const FevEventGroup& grp,
                              const FevFile& fev) {
    open(f, d, "eventgroup");
    tag(f, d+1, "name", grp.name);
    tag(f, d+1, "guid", guid_from_name("grp:" + grp.name));
    tagi(f, d+1, "eventgroup_nextid", static_cast<int>(grp.subgroups.size()));
    tagi(f, d+1, "event_nextid", static_cast<int>(grp.events.size()));
    tagi(f, d+1, "open", 1);
    tag(f, d+1, "notes", "");

    for (const auto& sg : grp.subgroups)
        write_event_group(f, d+1, sg, fev);
    for (const auto& ev : grp.events)
        write_event(f, d+1, ev, fev);

    close(f, d, "eventgroup");
}

// ---------------------------------------------------------------------------
// Write: sounddef folder hierarchy + sounddefs
// ---------------------------------------------------------------------------

struct SoundDefFolder {
    std::string name;
    std::unordered_map<std::string, SoundDefFolder> children;
    std::vector<const FevSoundDefinition*> defs;
};

static void insert_sounddef(SoundDefFolder& root, const FevSoundDefinition& sd) {
    const std::string& n = sd.name;
    size_t start = (n.empty() || n[0] != '/') ? 0 : 1;
    SoundDefFolder* cur = &root;
    while (start < n.size()) {
        size_t slash = n.find('/', start);
        if (slash == std::string::npos) {
            cur->defs.push_back(&sd);
            break;
        }
        std::string comp = n.substr(start, slash - start);
        cur = &cur->children[comp];
        if (cur->name.empty()) cur->name = comp;
        start = slash + 1;
    }
}

static void write_sounddef_waveforms(FILE* f, int d, const FevSoundDefinition& sd) {
    for (const auto& wf : sd.waveforms) {
        if (wf.type == FevWaveformType::WAVETABLE) {
            const auto& wp = std::get<FevWavetableParams>(wf.params);
            open(f, d, "waveform");
            tag(f, d+1, "filename", wp.filename);
            tag(f, d+1, "soundbankname", wp.bank_name);
            tagi(f, d+1, "weight", wf.weight > 0 ? wf.weight : 100);
            tagi(f, d+1, "percentagelocked", wp.percentage_locked);
            close(f, d, "waveform");
        }
    }
}

static void write_sounddef(FILE* f, int d, const FevSoundDefinition& sd, const FevFile& fev) {
    const FevSoundDefinitionConfig* cfg = nullptr;
    if (sd.config_index < fev.sound_definition_configs.size())
        cfg = &fev.sound_definition_configs[sd.config_index];

    open(f, d, "sounddef");
    tag(f, d+1, "name", sd.name);
    tag(f, d+1, "guid", guid_from_name("sd:" + sd.name));

    if (cfg) {
        tag(f, d+1, "type", play_mode_str(cfg->play_mode));
        tagi(f, d+1, "spawntime_min",  cfg->spawn_time_min);
        tagi(f, d+1, "spawntime_max",  cfg->spawn_time_max);
        tagi(f, d+1, "spawn_max",      cfg->maximum_spawned_sounds);
        tagi(f, d+1, "mode",           0);
        tagf(f, d+1, "pitch",          cfg->pitch);
        tagi(f, d+1, "pitch_randmethod",  cfg->pitch_rand_method);
        tagf(f, d+1, "pitch_random_min",  cfg->pitch_random_min);
        tagf(f, d+1, "pitch_random_max",  cfg->pitch_random_max);
        tagf(f, d+1, "pitch_randomization", cfg->pitch_randomization);
        tagi(f, d+1, "pitch_recalculate",
             static_cast<int>(cfg->pitch_randomization_behavior));
        tagf(f, d+1, "volume_db",           linear_to_db(cfg->volume));
        tagi(f, d+1, "volume_randmethod",    cfg->volume_rand_method);
        tagf(f, d+1, "volume_random_min",    linear_to_db(cfg->volume_random_min));
        tagf(f, d+1, "volume_random_max",    linear_to_db(cfg->volume_random_max));
        tagf(f, d+1, "volume_randomization", linear_to_db(cfg->volume_randomization));
        tagf(f, d+1, "position_randomization", cfg->threed_position_randomization);
        tagi(f, d+1, "trigger_delay_min", cfg->trigger_delay_min);
        tagi(f, d+1, "trigger_delay_max", cfg->trigger_delay_max);
        tagi(f, d+1, "spawncount",        cfg->spawn_count);
    } else {
        tag(f, d+1, "type", "sequential");
        for (const char* t : {"spawntime_min","spawntime_max","spawn_max","mode",
                               "pitch","pitch_randmethod","pitch_random_min",
                               "pitch_random_max","pitch_randomization","pitch_recalculate",
                               "volume_db","volume_randmethod","volume_random_min",
                               "volume_random_max","volume_randomization",
                               "position_randomization","trigger_delay_min",
                               "trigger_delay_max","spawncount"})
            tagi(f, d+1, t, 0);
    }

    tag(f, d+1, "notes", "");
    tagi(f, d+1, "entrylistmode", 1);

    write_sounddef_waveforms(f, d+1, sd);

    close(f, d, "sounddef");
}

static void write_sounddef_folder(FILE* f, int d, const SoundDefFolder& folder,
                                  const FevFile& fev) {
    open(f, d, "sounddeffolder");
    tag(f, d+1, "name", folder.name.empty() ? "master" : folder.name);
    tag(f, d+1, "guid", guid_from_name("folder:" + folder.name));
    tagi(f, d+1, "open", 0);

    for (const auto& [n, child] : folder.children)
        write_sounddef_folder(f, d+1, child, fev);
    for (const auto* sd : folder.defs)
        write_sounddef(f, d+1, *sd, fev);

    close(f, d, "sounddeffolder");
}

// ---------------------------------------------------------------------------
// Write: soundbanks (with waveform metadata from FSB when available)
// ---------------------------------------------------------------------------

static void write_soundbank(FILE* f, int d, const FevBank& bank,
                             const std::vector<std::string>& filenames,
                             const std::unordered_map<std::string, FsbSampleHeader>& fsb_idx) {
    open(f, d, "soundbank");
    tag(f, d+1, "name", bank.name);
    tag(f, d+1, "guid", guid_from_name("bank:" + bank.name));
    tagi(f, d+1, "load_into_rsx", 0);
    tagi(f, d+1, "disable_seeking", 0);
    tagi(f, d+1, "enable_syncpoints", 1);
    tagi(f, d+1, "hasbuiltwithsyncpoints", 0);

    const char* bt = bank_type_str(bank.load_mode);
    for (const char* plat : {"_PC","_XBOX","_XBOX360","_GC","_PS2","_PSP","_PS3","_WII"}) {
        const char* pbt;
        if (strcmp(plat, "_PC") == 0) {
            pbt = bt;
        } else if (strcmp(plat,"_XBOX")==0 || strcmp(plat,"_GC")==0 || strcmp(plat,"_PS2")==0) {
            pbt = "DecompressedSample";
        } else {
            pbt = "Sample";
        }
        std::string tag_name = std::string(plat) + "_banktype";
        fprintf(f, "<%s>%s</%s>\n", tag_name.c_str(), pbt, tag_name.c_str());
    }

    tag(f, d+1, "notes", "");
    tagi(f, d+1, "rebuild", 1);

    for (const auto& fn : filenames) {
        const FsbSampleHeader* smp = nullptr;
        std::string stem = stem_lower(fn);
        auto it = fsb_idx.find(stem);
        if (it == fsb_idx.end() && stem.size() > 29)
            it = fsb_idx.find(stem.substr(0, 29));
        if (it != fsb_idx.end()) smp = &it->second;

        open(f, d+1, "waveform");
        tag(f, d+2, "filename", fn);
        tag(f, d+2, "guid", guid_from_name("wf:" + fn));
        tagi(f, d+2, "mindistance", 1);
        tagi(f, d+2, "maxdistance", 10000);
        tagi(f, d+2, "deffreq",     smp ? smp->default_freq : 44100);
        tagf(f, d+2, "defvol",      smp ? smp->default_vol  : 1.0f);
        tagi(f, d+2, "defpan",      smp ? smp->default_pan  : 0);
        tagi(f, d+2, "defpri",      smp ? smp->default_pri  : 128);
        tagi(f, d+2, "xmafiltering", 0);
        tagi(f, d+2, "channelmode",  smp ? (smp->num_channels > 1 ? 1 : 0) : 0);
        tagi(f, d+2, "quality_crossplatform", 0);
        tagi(f, d+2, "quality", -1);
        tagi(f, d+2, "optimisedratereduction", 100);
        tagi(f, d+2, "enableratereduction", 1);
        tag(f, d+2, "notes", "");
        close(f, d+1, "waveform");
    }

    int ms_default = bank.max_streams > 0 ? bank.max_streams : 10;
    for (const char* plat : {"_PC","_XBOX","_XBOX360","_GC","_PS2","_PSP","_PS3","_WII"}) {
        std::string p(plat);
        const char* fmt = (strcmp(plat,"_PC")==0)     ? "MP3"
                        : (strcmp(plat,"_XBOX360")==0) ? "XMA"
                        : (strcmp(plat,"_PS3")==0)     ? "MP2"
                        : (strcmp(plat,"_PSP")==0)     ? "VAG"
                        : (strcmp(plat,"_WII")==0)     ? "GCADPCM" : "PCM";
        int q = (strcmp(plat,"_PC")==0 || strcmp(plat,"_XBOX360")==0 ||
                 strcmp(plat,"_PS3")==0) ? 100 : 50;
        int ms = (strcmp(plat,"_PSP")==0 || strcmp(plat,"_WII")==0) ? 4 : ms_default;
        fprintf(f, "<%s_format>%s</%s_format>\n", plat, fmt, plat);
        fprintf(f, "<%s_quality>%d</%s_quality>\n", plat, q, plat);
        fprintf(f, "<%s_optimisesamplerate>0</%s_optimisesamplerate>\n", plat, plat);
        int force_sw = (strcmp(plat,"_PC")==0 || strcmp(plat,"_XBOX360")==0 ||
                        strcmp(plat,"_PS3")==0) ? 1 : 0;
        fprintf(f, "<%s_forcesoftware>%d</%s_forcesoftware>\n", plat, force_sw, plat);
        fprintf(f, "<%s_maxstreams>%d</%s_maxstreams>\n", plat, ms, plat);
    }

    close(f, d, "soundbank");
}

// Forward declarations for music system
static void write_composition(FILE* f, const FevFile& fev);

// ---------------------------------------------------------------------------
// Top-level write: emit a complete FDP XML document.
// f must be open for writing. fsb_idx may be empty (waveform metadata omitted).
// ---------------------------------------------------------------------------

static void write_fdp(FILE* f, const FevFile& fev,
                      const std::unordered_map<std::string, FsbSampleHeader>& fsb_idx) {
    auto bank_wf_idx = build_bank_waveform_index(fev);

    SoundDefFolder sdf_root;
    sdf_root.name = "master";
    for (const auto& sd : fev.sound_definitions)
        insert_sounddef(sdf_root, sd);

    fprintf(f, "<project>\n");
    tag(f, 1, "name", fev.project_name);
    tag(f, 1, "guid", guid_from_name("project:" + fev.project_name));
    tagi(f, 1, "version", 4);
    tagi(f, 1, "eventgroup_nextid", 0);
    tagi(f, 1, "soundbank_nextid", static_cast<int>(fev.banks.size()));
    tagi(f, 1, "sounddef_nextid",  static_cast<int>(fev.sound_definitions.size()));
    tagi(f, 1, "build_project", 1);
    tagi(f, 1, "build_headerfile", 1);
    tagi(f, 1, "build_banklists", 1);
    tagi(f, 1, "build_programmerreport", 1);
    tagi(f, 1, "build_applytemplate", 1);
    tag(f, 1, "currentbank",     fev.banks.empty() ? "" : fev.banks[0].name);
    tag(f, 1, "currentlanguage", "default");
    tag(f, 1, "primarylanguage", "default");
    tag(f, 1, "language",        "default");
    tag(f, 1, "templatefilename", "");
    tagi(f, 1, "templatefileopen", 0);

    for (const auto& sub : fev.root_category.subcategories)
        write_event_category(f, 1, sub);

    write_sounddef_folder(f, 1, sdf_root, fev);

    for (const auto& grp : fev.event_groups)
        write_event_group(f, 1, grp, fev);

    for (const auto& bank : fev.banks) {
        auto it = bank_wf_idx.find(bank.name);
        const std::vector<std::string> empty;
        write_soundbank(f, 1, bank,
                        it != bank_wf_idx.end() ? it->second : empty,
                        fsb_idx);
    }

    // Music system (Composition) — only for FEVs with non-empty music data.
    write_composition(f, fev);

    fprintf(f, "</project>\n");
}

// ---------------------------------------------------------------------------
// Music system: <Composition> section
// ---------------------------------------------------------------------------

// Helper to walk the music data tree and collect typed chunks
struct MusicCollected {
    std::vector<const FevMdEntl*> cue_lists;     // from entl under cues
    std::vector<const FevMdEntl*> param_lists;    // from entl under prms
    std::vector<uint32_t> param_ids;              // from prmd
    std::vector<const FevMdThmd*> themes;
    std::vector<const FevMdSgmd*> segments;
    std::vector<const FevMdLnkd*> links;
    std::vector<const FevMdTlnd*> timelines;
    std::vector<const FevMdScnd*> scenes;

    // Segment nested data: segment_id -> (samples bank+index, sample playback mode)
    struct SegmentSamples {
        FevSamplePlaybackMode playback_mode = FevSamplePlaybackMode::SEQUENTIAL;
        std::vector<std::pair<std::string, uint32_t>> samples; // bank_name, index
    };
    std::unordered_map<uint32_t, SegmentSamples> segment_samples;

    // Sample filenames from the top-level smpf > str chunk.
    // These are audio file paths (e.g. "music/avant gardens/LUMX_AG-Landing-AreaTuned.wav")
    // ordered to match the sequential segment/sample layout in the composition.
    std::vector<std::string> sample_filenames;

    // Link-from-segment data: maps from_segment_id to link IDs
    std::vector<const FevMdLfsd*> link_from_segments;

    // Conditions shared struct used by both themes and links
    struct ConditionSet {
        std::vector<const FevMdCms*> cms_conditions;
        std::vector<const FevMdCprm*> cprm_conditions;
    };

    // Theme conditions: theme_id -> conditions
    std::unordered_map<uint32_t, ConditionSet> theme_conditions;

    // Link conditions: link sequential index (0-based) -> conditions
    // Populated when cms/cprm appear inside "lnk " containers.
    std::unordered_map<uint32_t, ConditionSet> link_conditions;
};

static void collect_music_chunks(const std::vector<FevMusicDataChunk>& chunks,
                                  MusicCollected& mc, const std::string& parent_type = "",
                                  uint32_t parent_id = 0) {
    for (const auto& chunk : chunks) {
        if (chunk.type == "entl") {
            if (auto* entl = std::get_if<FevMdEntl>(&chunk.body)) {
                if (parent_type == "cues") mc.cue_lists.push_back(entl);
                else if (parent_type == "prms") mc.param_lists.push_back(entl);
            }
        } else if (chunk.type == "prmd") {
            if (auto* v = std::get_if<uint32_t>(&chunk.body)) mc.param_ids.push_back(*v);
        } else if (chunk.type == "thmd") {
            if (auto* thmd = std::get_if<FevMdThmd>(&chunk.body)) mc.themes.push_back(thmd);
        } else if (chunk.type == "sgmd") {
            if (auto* sgmd = std::get_if<FevMdSgmd>(&chunk.body)) {
                mc.segments.push_back(sgmd);
                // Collect nested samples from sgmd_nested_items.
                // Structure: sgmd_nested_items -> item -> chunks -> smps (container)
                //            -> items -> chunks -> smph, smp
                MusicCollected::SegmentSamples ss;
                for (const auto& nested_item : chunk.sgmd_nested_items) {
                    for (const auto& nc : nested_item.chunks) {
                        // smps is a container; the actual smph/smp chunks are inside it
                        if (nc.type == "smps") {
                            if (auto* smps_items = std::get_if<std::vector<FevMusicDataItem>>(&nc.body)) {
                                for (const auto& smps_item : *smps_items) {
                                    for (const auto& sc : smps_item.chunks) {
                                        if (sc.type == "smph") {
                                            if (auto* smph = std::get_if<FevMdSmph>(&sc.body))
                                                ss.playback_mode = smph->playback_mode;
                                        } else if (sc.type == "smp ") {
                                            if (auto* smp = std::get_if<FevMdSmp>(&sc.body))
                                                ss.samples.emplace_back(smp->bank_name, smp->index);
                                        }
                                    }
                                }
                            }
                        }
                        // Also handle smph/smp directly in case they appear outside smps
                        else if (nc.type == "smph") {
                            if (auto* smph = std::get_if<FevMdSmph>(&nc.body))
                                ss.playback_mode = smph->playback_mode;
                        } else if (nc.type == "smp ") {
                            if (auto* smp = std::get_if<FevMdSmp>(&nc.body))
                                ss.samples.emplace_back(smp->bank_name, smp->index);
                        }
                    }
                }
                mc.segment_samples[sgmd->segment_id] = std::move(ss);
            }
        } else if (chunk.type == "str ") {
            // Collect sample filenames from smpf > str chunks.
            // The smpf container holds a str chunk with all audio file paths used
            // by the composition's segments. These are ordered sequentially to match
            // the segment/sample layout.
            if (parent_type == "smpf") {
                if (auto* str = std::get_if<FevMdStr>(&chunk.body)) {
                    for (const auto& name : str->names)
                        mc.sample_filenames.push_back(name);
                }
            }
        } else if (chunk.type == "lnkd") {
            if (auto* lnkd = std::get_if<FevMdLnkd>(&chunk.body)) mc.links.push_back(lnkd);
        } else if (chunk.type == "lfsd") {
            if (auto* lfsd = std::get_if<FevMdLfsd>(&chunk.body)) mc.link_from_segments.push_back(lfsd);
        } else if (chunk.type == "tlnd") {
            if (auto* tlnd = std::get_if<FevMdTlnd>(&chunk.body)) mc.timelines.push_back(tlnd);
        } else if (chunk.type == "scnd") {
            if (auto* scnd = std::get_if<FevMdScnd>(&chunk.body)) mc.scenes.push_back(scnd);
        } else if (chunk.type == "cms ") {
            if (auto* cms = std::get_if<FevMdCms>(&chunk.body)) {
                // Associate with theme or link depending on parent container type.
                // "thm " containers use theme_id, "lnk " containers use link index.
                if (parent_type == "lnk ") {
                    mc.link_conditions[parent_id].cms_conditions.push_back(cms);
                } else {
                    mc.theme_conditions[parent_id].cms_conditions.push_back(cms);
                }
            }
        } else if (chunk.type == "cprm") {
            if (auto* cprm = std::get_if<FevMdCprm>(&chunk.body)) {
                if (parent_type == "lnk ") {
                    mc.link_conditions[parent_id].cprm_conditions.push_back(cprm);
                } else {
                    mc.theme_conditions[parent_id].cprm_conditions.push_back(cprm);
                }
            }
        }

        // Recurse into containers
        if (auto* items = std::get_if<std::vector<FevMusicDataItem>>(&chunk.body)) {
            uint32_t this_id = parent_id;
            std::string this_type = chunk.type;
            // Track current theme ID for condition association
            if (chunk.type == "thm ") {
                // The first sub-chunk should be thmd with the theme_id
                for (const auto& sub : *items) {
                    for (const auto& sc : sub.chunks) {
                        if (sc.type == "thmd") {
                            if (auto* thmd = std::get_if<FevMdThmd>(&sc.body))
                                this_id = thmd->theme_id;
                        }
                    }
                }
            }
            // Track current link index for condition association.
            // "lnk " containers correspond to individual links; the link index
            // is the current count of lnkd entries we've seen so far (0-based).
            // We extract the lnkd from this container to get its sequential index.
            else if (chunk.type == "lnk ") {
                // The lnkd inside this container hasn't been collected yet since
                // we recurse below. Count existing links as the index for this one.
                this_id = static_cast<uint32_t>(mc.links.size());
            }
            for (const auto& sub_item : *items)
                collect_music_chunks(sub_item.chunks, mc, this_type, this_id);
        }
    }
}

static void write_composition(FILE* f, const FevFile& fev) {
    // Check if this FEV has actual music data (non-empty comp container)
    bool has_music = false;
    for (const auto& item : fev.music_data.items) {
        for (const auto& chunk : item.chunks) {
            if (chunk.type == "comp") {
                if (auto* sub = std::get_if<std::vector<FevMusicDataItem>>(&chunk.body)) {
                    if (!sub->empty()) has_music = true;
                }
            }
        }
    }
    if (!has_music) return;

    // Collect all typed chunks from the music data tree
    MusicCollected mc;
    for (const auto& item : fev.music_data.items)
        collect_music_chunks(item.chunks, mc);

    // Build link_id → lnkd lookup. The lfsd link_ids match lnkd.segment_1_id,
    // NOT sequential array indices. Confirmed by RE: lfsd link_id 192 matches
    // lnkd with segment_1_id=192.
    std::unordered_map<uint32_t, const FevMdLnkd*> lnkd_by_s1id;
    for (const auto* lnk : mc.links)
        lnkd_by_s1id[lnk->segment_1_id] = lnk;

    // Compute max IDs for Factory UID elements
    uint32_t max_theme_id = 0, max_link_id = 0, max_param_id = 0, max_timeline_id = 0;
    for (const auto* t : mc.themes)
        if (t->theme_id > max_theme_id) max_theme_id = t->theme_id;
    for (const auto* t : mc.timelines)
        if (t->timeline_id > max_timeline_id) max_timeline_id = t->timeline_id;
    // Links use sequential IDs starting at 1
    max_link_id = static_cast<uint32_t>(mc.links.size());
    for (const auto* entl : mc.param_lists)
        for (uint32_t id : entl->ids)
            if (id > max_param_id) max_param_id = id;

    // Build segment→theme reverse mapping.
    // Step 1: Direct assignments from theme start/end sequence IDs.
    std::unordered_map<uint32_t, uint32_t> segment_to_theme;
    for (const auto* thm : mc.themes) {
        for (uint32_t sid : thm->start_sequence_ids)
            segment_to_theme[sid] = thm->theme_id;
        for (uint32_t eid : thm->end_sequence_ids)
            segment_to_theme[eid] = thm->theme_id;
    }

    // Step 2: Build link graph (segment_id → connected segment_ids) from lfsd+lnkd.
    std::unordered_map<uint32_t, std::vector<uint32_t>> link_graph;
    for (const auto* lfsd : mc.link_from_segments) {
        for (uint32_t link_id : lfsd->link_ids) {
            // Find corresponding lnkd to get destination segment
            const FevMdLnkd* lnk = nullptr;
            if (link_id > 0 && link_id <= mc.links.size())
                lnk = mc.links[link_id - 1];
            if (lnk) {
                link_graph[lfsd->from_segment_id].push_back(lnk->segment_2_id);
                link_graph[lnk->segment_2_id].push_back(lfsd->from_segment_id);
            }
        }
    }

    // Step 3: Propagate theme assignments through link graph until stable.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [seg_id, neighbors] : link_graph) {
            if (segment_to_theme.count(seg_id)) {
                for (uint32_t n : neighbors) {
                    if (!segment_to_theme.count(n)) {
                        segment_to_theme[n] = segment_to_theme[seg_id];
                        changed = true;
                    }
                }
            }
        }
    }

    // Step 4: For any remaining unresolved segments, assign to the first theme
    // to prevent null pointer crashes in FMOD Designer.
    uint32_t fallback_theme = mc.themes.empty() ? 0 : mc.themes[0]->theme_id;
    for (const auto* seg : mc.segments) {
        if (!segment_to_theme.count(seg->segment_id))
            segment_to_theme[seg->segment_id] = fallback_theme;
    }

    open(f, 0, "Composition");

    // CompositionUI — FMOD Designer 4.32.09 requires ThemeEditorItemRepository
    // and SceneEditorItemRepository to be pre-populated with GUI wrapper items.
    // Without these, the ThemeEditor's initialization loop at FUN_00532a59 tries
    // to look up themes/segments in an empty repository and crashes (null deref
    // at offset 0x1c). Confirmed via Ghidra RE of fmod_designer.exe 4.32.09.
    // Build theme_names map early (also used later for ThemeRepository output)
    std::unordered_map<uint32_t, std::string> theme_names;
    {
        std::unordered_map<uint32_t, std::string> cue_id_to_name;
        for (const auto* entl : mc.cue_lists)
            for (size_t i = 0; i < entl->ids.size() && i < entl->cue_names.size(); ++i)
                cue_id_to_name[entl->ids[i]] = entl->cue_names[i];
        for (const auto* scn : mc.scenes) {
            for (const auto& ci : scn->cue_instances) {
                if (theme_names.find(ci.condition_id) == theme_names.end()) {
                    auto nameIt = cue_id_to_name.find(ci.cue_id);
                    if (nameIt != cue_id_to_name.end())
                        theme_names[ci.condition_id] = nameIt->second;
                }
            }
        }
    }

    // Build theme→segments map for layout (all segments belonging to each theme)
    std::unordered_map<uint32_t, std::vector<uint32_t>> theme_segment_list;
    for (const auto* seg : mc.segments) {
        auto thIt = segment_to_theme.find(seg->segment_id);
        if (thIt != segment_to_theme.end())
            theme_segment_list[thIt->second].push_back(seg->segment_id);
    }

    // Track which segments have been placed to avoid duplicates
    std::unordered_set<uint32_t> placed_segments;

    open(f, 1, "CompositionUI");

    // SceneEditor with CueItems — arrange in a grid
    open(f, 2, "SceneEditor");
    open(f, 3, "SceneEditorItemRepository");
    {
        uint32_t scene_idx = 1;
        int cue_col = 0, cue_row = 0;
        const int cues_per_row = 8;
        const int cue_spacing_x = 200;
        const int cue_spacing_y = 100;
        for (const auto* scn : mc.scenes) {
            for (const auto& ci : scn->cue_instances) {
                fprintf(f, "<CueItem SceneID=\"%u\" CueID=\"%u\">\n", scene_idx, ci.cue_id);
                char pos[64];
                snprintf(pos, sizeof(pos), "%d,%d",
                         cue_col * cue_spacing_x, cue_row * cue_spacing_y);
                tag(f, 4, "Position", pos);
                fprintf(f, "</CueItem>\n");
                cue_col++;
                if (cue_col >= cues_per_row) { cue_col = 0; cue_row++; }
            }
            scene_idx++;
        }
    }
    close(f, 3, "SceneEditorItemRepository");
    close(f, 2, "SceneEditor");

    // ThemeEditor — tree layout: themes on the left, segments arranged as a
    // left-to-right tree based on link connectivity.
    // Start segments (from theme StartSegment) are column 0.
    // Their link destinations are column 1, those destinations' links are column 2, etc.
    open(f, 2, "ThemeEditor");
    open(f, 3, "ThemeEditorItemRepository");
    {
        // FMOD Designer theme/segment items are ~200x80 pixels.
        // Arrange themes in a grid layout so they don't overlap.
        // Each theme gets its own tile: theme box on the left, segment tree to the right.
        const int seg_spacing_x = 500;   // Horizontal between segment columns
        const int seg_spacing_y = 250;   // Vertical between segments in same column
        const int tile_padding_x = 600;  // Padding between theme tiles horizontally
        const int tile_padding_y = 500;  // Padding between theme tiles vertically
        int tile_col = 0;
        int tile_row_y = 50;             // Current row Y start
        int tile_row_max_h = 0;          // Tallest tile in current row
        // First pass: compute each theme's tile size
        struct ThemeTile { uint32_t theme_id; int width; int height; int max_segs; };
        std::vector<ThemeTile> tiles;

        // Build per-theme directed link graph: segment_id → [destination_segment_ids]
        // (using the lfsd→lnkd mapping we already built)
        std::unordered_map<uint32_t, std::vector<uint32_t>> seg_outlinks;
        for (const auto* lfsd : mc.link_from_segments) {
            for (uint32_t link_id : lfsd->link_ids) {
                const FevMdLnkd* lnk = nullptr;
                { auto it = lnkd_by_s1id.find(link_id); if (it != lnkd_by_s1id.end()) lnk = it->second; }
                if (lnk)
                    seg_outlinks[lfsd->from_segment_id].push_back(lnk->segment_2_id);
            }
        }

        // First pass: compute BFS column assignments for every theme
        struct ThemeLayout {
            uint32_t theme_id;
            std::string name;
            std::string zone_group;
            std::vector<std::vector<uint32_t>> columns; // column → segment IDs
            int num_columns;
            int max_rows;
            bool is_empty;  // no segments, no links — skip in layout
        };
        std::vector<ThemeLayout> layouts;

        // Zone grouping by name prefix
        auto get_zone = [](const std::string& name) -> std::string {
            static const char* prefixes[] = {
                "AG_", "FV_", "GF_", "NT_", "NS_", "NJ_", "CP_",
                "Concert_", "Gnarled_Forest_", "Nimbus_Station",
                "Monastery_", "Spaceship", "Property_", "Pet",
                "Race", "Silence", "Winter", nullptr
            };
            static const char* groups[] = {
                "AG", "FV", "GF", "NT", "NS", "NJ", "CP",
                "Concert", "GF", "NS",
                "Monastery", "Spaceship", "Property", "Pet",
                "Race", "Misc", "Property"
            };
            for (int i = 0; prefixes[i]; i++) {
                if (name.compare(0, strlen(prefixes[i]), prefixes[i]) == 0)
                    return groups[i];
            }
            return "Other";
        };

        for (const auto* thm : mc.themes) {
            ThemeLayout tl;
            tl.theme_id = thm->theme_id;
            auto tnIt = theme_names.find(thm->theme_id);
            tl.name = tnIt != theme_names.end() ? tnIt->second : "";
            tl.zone_group = get_zone(tl.name);

            auto segIt = theme_segment_list.find(thm->theme_id);
            // Skip deprecated/empty themes
            bool deprecated = (tl.name == "Depreciated" || tl.name == "test-depreciated" ||
                               tl.name == "Depot" || tl.name.empty());
            if (deprecated || segIt == theme_segment_list.end() || segIt->second.empty()) {
                tl.num_columns = 0;
                tl.max_rows = 1;
                tl.is_empty = true;
                layouts.push_back(std::move(tl));
                continue;
            }
            tl.is_empty = false;

            std::unordered_set<uint32_t> theme_segs(segIt->second.begin(), segIt->second.end());

            // BFS from start segments to build column assignments
            std::unordered_map<uint32_t, int> seg_column; // segment_id → column (0-based)
            std::vector<std::vector<uint32_t>> columns;   // column index → segment IDs
            {
                std::queue<uint32_t> bfs;
                // Start segments are column 0
                for (uint32_t sid : thm->start_sequence_ids) {
                    if (!theme_segs.count(sid)) continue;
                    if (seg_column.count(sid)) continue;
                    seg_column[sid] = 0;
                    bfs.push(sid);
                }
                // BFS through outlinks
                while (!bfs.empty()) {
                    uint32_t cur = bfs.front(); bfs.pop();
                    int col = seg_column[cur];
                    auto olIt = seg_outlinks.find(cur);
                    if (olIt != seg_outlinks.end()) {
                        for (uint32_t dest : olIt->second) {
                            if (!theme_segs.count(dest)) continue;
                            if (seg_column.count(dest)) continue;
                            seg_column[dest] = col + 1;
                            bfs.push(dest);
                        }
                    }
                }
                // Find disconnected subgraphs: segments with outlinks but
                // not yet placed. Seed them as roots (column 0) and BFS again.
                bool found_new = true;
                while (found_new) {
                    found_new = false;
                    for (uint32_t sid : segIt->second) {
                        if (seg_column.count(sid)) continue;
                        // Check if this segment has outlinks within the theme
                        auto olIt = seg_outlinks.find(sid);
                        if (olIt == seg_outlinks.end()) continue;
                        bool has_theme_dest = false;
                        for (uint32_t d : olIt->second)
                            if (theme_segs.count(d)) { has_theme_dest = true; break; }
                        if (!has_theme_dest) continue;
                        // Check if any placed segment links TO this one (make it col+1)
                        bool linked_from_placed = false;
                        for (auto& [src, dests] : seg_outlinks) {
                            if (!seg_column.count(src)) continue;
                            for (uint32_t d : dests) {
                                if (d == sid) {
                                    seg_column[sid] = seg_column[src] + 1;
                                    linked_from_placed = true; break;
                                }
                            }
                            if (linked_from_placed) break;
                        }
                        if (!linked_from_placed)
                            seg_column[sid] = 0; // disconnected root
                        bfs.push(sid);
                        found_new = true;
                        // BFS from this new seed
                        while (!bfs.empty()) {
                            uint32_t cur = bfs.front(); bfs.pop();
                            int col = seg_column[cur];
                            auto ol = seg_outlinks.find(cur);
                            if (ol != seg_outlinks.end()) {
                                for (uint32_t dest : ol->second) {
                                    if (!theme_segs.count(dest)) continue;
                                    if (seg_column.count(dest)) continue;
                                    seg_column[dest] = col + 1;
                                    bfs.push(dest);
                                }
                            }
                        }
                    }
                }
                // Any remaining unvisited segments (no links at all) → column 0
                for (uint32_t sid : segIt->second) {
                    if (!seg_column.count(sid))
                        seg_column[sid] = 0;
                }
                // Group by column
                for (auto& [sid, col] : seg_column) {
                    while (static_cast<int>(columns.size()) <= col)
                        columns.push_back({});
                    columns[col].push_back(sid);
                }
            }

            // Store layout info
            int max_rows = 0;
            for (auto& col : columns)
                if (static_cast<int>(col.size()) > max_rows)
                    max_rows = static_cast<int>(col.size());
            if (max_rows < 1) max_rows = 1;

            tl.columns = std::move(columns);
            tl.num_columns = static_cast<int>(tl.columns.size());
            tl.max_rows = max_rows;
            layouts.push_back(std::move(tl));
        }

        // Second pass: group by zone, arrange each zone as a vertical column.
        // Zones are laid out left to right across the canvas.
        const int theme_box_width = 500;

        // Group layouts by zone
        std::map<std::string, std::vector<size_t>> zone_indices; // zone → layout indices
        for (size_t i = 0; i < layouts.size(); i++) {
            if (!layouts[i].is_empty)
                zone_indices[layouts[i].zone_group].push_back(i);
        }
        // Also add empty themes to a skip group (still need ThemeItem for Designer)
        std::vector<size_t> empty_indices;
        for (size_t i = 0; i < layouts.size(); i++)
            if (layouts[i].is_empty) empty_indices.push_back(i);

        int zone_x = 50;
        const int zone_gap = 800;  // Gap between zone columns

        // Place each zone as a vertical column
        for (auto& [zone_name, indices] : zone_indices) {
            int zone_y = 50;
            int zone_max_width = 0;

            for (size_t idx : indices) {
                auto& tl = layouts[idx];

                int tile_w = theme_box_width + std::max(1, tl.num_columns) * seg_spacing_x;
                int tile_h = std::max(1, tl.max_rows) * seg_spacing_y;

                int theme_center_y = zone_y + (tile_h - seg_spacing_y) / 2;
                fprintf(f, "<ThemeItem ThemeID=\"%u\">\n", tl.theme_id);
                char pos[64]; snprintf(pos, sizeof(pos), "%d,%d", zone_x, theme_center_y);
                tag(f, 4, "Position", pos);
                fprintf(f, "</ThemeItem>\n");

                int seg_base_x = zone_x + theme_box_width;
                for (int col = 0; col < static_cast<int>(tl.columns.size()); col++) {
                    auto& col_segs = tl.columns[col];
                    int col_height = static_cast<int>(col_segs.size()) * seg_spacing_y;
                    int col_start_y = zone_y + (tile_h - col_height) / 2;

                    for (int row = 0; row < static_cast<int>(col_segs.size()); row++) {
                        uint32_t sid = col_segs[row];
                        if (placed_segments.count(sid)) continue;
                        placed_segments.insert(sid);

                        int sx = seg_base_x + col * seg_spacing_x;
                        int sy = col_start_y + row * seg_spacing_y;
                        fprintf(f, "<SegmentItem SegmentID=\"%u\">\n", sid);
                        tagi(f, 4, "Theme", tl.theme_id);
                        snprintf(pos, sizeof(pos), "%d,%d", sx, sy);
                        tag(f, 4, "Position", pos);
                        fprintf(f, "</SegmentItem>\n");
                    }
                }

                if (tile_w > zone_max_width) zone_max_width = tile_w;
                zone_y += tile_h + tile_padding_y;
            }
            zone_x += zone_max_width + zone_gap;
        }

        // Place empty/deprecated themes off to the far right (required for Designer stability)
        {
            int empty_y = 50;
            for (size_t idx : empty_indices) {
                auto& tl = layouts[idx];
                fprintf(f, "<ThemeItem ThemeID=\"%u\">\n", tl.theme_id);
                char pos[64]; snprintf(pos, sizeof(pos), "%d,%d", zone_x, empty_y);
                tag(f, 4, "Position", pos);
                fprintf(f, "</ThemeItem>\n");
                empty_y += 200;
            }
        }
        {
            // Unused code block end — handle remaining segments

        // Any remaining unplaced segments
        int remain_y = 50;
        for (const auto* seg : mc.segments) {
            if (placed_segments.count(seg->segment_id)) continue;
            placed_segments.insert(seg->segment_id);
            uint32_t tid = segment_to_theme.count(seg->segment_id)
                ? segment_to_theme[seg->segment_id] : fallback_theme;
            fprintf(f, "<SegmentItem SegmentID=\"%u\">\n", seg->segment_id);
            tagi(f, 4, "Theme", tid);
            char pos[64]; snprintf(pos, sizeof(pos), "%d,%d", zone_x + 600, remain_y);
            tag(f, 4, "Position", pos);
            fprintf(f, "</SegmentItem>\n");
            remain_y += seg_spacing_y;
        }
        }
    close(f, 3, "ThemeEditorItemRepository");
    close(f, 2, "ThemeEditor");

    close(f, 1, "CompositionUI");

    // Cues
    open(f, 1, "CueRepository");
    for (const auto* entl : mc.cue_lists) {
        for (size_t i = 0; i < entl->ids.size(); ++i) {
            fprintf(f, "<Cue ID=\"%u\">\n", entl->ids[i]);
            if (i < entl->cue_names.size())
                tag(f, 2, "Name", entl->cue_names[i]);
            close(f, 1, "Cue");
        }
    }
    close(f, 1, "CueRepository");

    // ExtLinkFactory — must come before ExtLinkRepository
    open(f, 1, "ExtLinkFactory");
    tagi(f, 2, "UID", max_link_id + 1);
    close(f, 1, "ExtLinkFactory");

    // Links — build from lfsd (from-segment data) which maps source segment IDs
    // to link IDs. Each link ID indexes into the lnkd array.
    // Build link_id → lnkd index map
    std::unordered_map<uint32_t, size_t> lnkd_by_id;
    {
        // lnkd chunks are numbered sequentially by their lnkh headers.
        // The lfsd link_ids reference these sequential IDs.
        // We need to map from lfsd link_id to the actual lnkd data.
        // The lnkd array is ordered by their headers (lnkh count),
        // and lfsd link_ids reference them by their sequential lnkd index.
        // Build: link sequential index → lnkd pointer.
    }

    // Build a map from lnkd sequential index to the lnkd pointer, so we can
    // look up link conditions by the same index used during collection.
    // Also build a reverse map from (segment_1_id, segment_2_id) -> lnkd index
    // for matching lfsd link_ids to their conditions.
    std::unordered_map<uint32_t, size_t> lnkd_seg1_to_idx;
    for (size_t i = 0; i < mc.links.size(); ++i)
        lnkd_seg1_to_idx[mc.links[i]->segment_1_id] = i;

    open(f, 1, "ExtLinkRepository");
    uint32_t link_uid = 1;
    for (const auto* lfsd : mc.link_from_segments) {
        for (uint32_t link_id : lfsd->link_ids) {
            // link_id matches lnkd.segment_1_id (confirmed via RE)
            auto lnkIt = lnkd_by_s1id.find(link_id);
            if (lnkIt == lnkd_by_s1id.end()) continue;
            const FevMdLnkd* lnk = lnkIt->second;

            // Determine if this link has non-default transition values.
            // Default transition is "end" only (at_segment_end=true, on_bar=false, on_beat=false).
            // If the link specifies anything else, UseDefaultTransitionType should be 0.
            bool is_default_transition =
                lnk->transition_behavior.at_segment_end &&
                !lnk->transition_behavior.on_bar &&
                !lnk->transition_behavior.on_beat;

            // StartSegmentID = the lfsd's from_segment_id (actual segment ID)
            // EndSegmentID = the lnkd's segment_2_id (destination segment ID)
            fprintf(f, "<ExtLink ID=\"%u\" StartSegmentID=\"%u\" EndSegmentID=\"%u\">\n",
                    link_uid, lfsd->from_segment_id, lnk->segment_2_id);
            tagi(f, 2, "Weight", 0);
            tagi(f, 2, "UseDefaultTransitionType", is_default_transition ? 1 : 0);

            // Link conditions from cms/cprm chunks nested inside the lnk container.
            // Format follows FMOD Designer's XML schema for Condition elements.
            auto cond_it = mc.link_conditions.find(link_id);
            if (cond_it != mc.link_conditions.end() &&
                (!cond_it->second.cms_conditions.empty() || !cond_it->second.cprm_conditions.empty())) {
                open(f, 2, "Condition");
                for (const auto* cms : cond_it->second.cms_conditions) {
                    open(f, 3, "MusicStateCondition");
                    tagi(f, 4, "Type", static_cast<int>(cms->condition_type));
                    tagi(f, 4, "ThemeID", cms->theme_id);
                    tagi(f, 4, "CueID", cms->cue_id);
                    close(f, 3, "MusicStateCondition");
                }
                for (const auto* cprm : cond_it->second.cprm_conditions) {
                    open(f, 3, "ParameterCondition");
                    tagi(f, 4, "Type", static_cast<int>(cprm->condition_type));
                    tagi(f, 4, "ParameterID", cprm->param_id);
                    tagi(f, 4, "Value1", cprm->value_1);
                    tagi(f, 4, "Value2", cprm->value_2);
                    close(f, 3, "ParameterCondition");
                }
                close(f, 2, "Condition");
            } else {
                fprintf(f, "<Condition/>\n");
            }

            open(f, 2, "TransitionType");
            tagi(f, 3, "end", lnk->transition_behavior.at_segment_end ? 1 : 0);
            tagi(f, 3, "bar", lnk->transition_behavior.on_bar ? 1 : 0);
            tagi(f, 3, "beat", lnk->transition_behavior.on_beat ? 1 : 0);
            close(f, 2, "TransitionType");
            close(f, 1, "ExtLink");
            link_uid++;
        }
    }
    close(f, 1, "ExtLinkRepository");

    // ParameterFactory — must come before ParameterRepository
    open(f, 1, "ParameterFactory");
    tagi(f, 2, "UID", max_param_id + 1);
    close(f, 1, "ParameterFactory");

    // Parameters
    open(f, 1, "ParameterRepository");
    for (const auto* entl : mc.param_lists) {
        for (size_t i = 0; i < entl->ids.size(); ++i) {
            fprintf(f, "<Parameter ID=\"%u\">\n", entl->ids[i]);
            if (i < entl->cue_names.size())
                tag(f, 2, "Name", entl->cue_names[i]);
            tagf(f, 2, "Value", 0);
            close(f, 1, "Parameter");
        }
    }
    close(f, 1, "ParameterRepository");

    // Scenes
    open(f, 1, "SceneRepository");
    uint32_t scene_uid = 1;
    for (const auto* scn : mc.scenes) {
        fprintf(f, "<Scene ID=\"%u\">\n", scene_uid++);
        // CueSheet is pairs: cue_id, condition_id, cue_id, condition_id, ...
        // FMOD Designer parses this with stride 2: item[i]=cue, item[i+1]=condition.
        // Confirmed via Ghidra RE of fmod_designer.exe FUN_00514d6f (Scene XML reader).
        std::string cuesheet;
        for (const auto& ci : scn->cue_instances) {
            if (!cuesheet.empty()) cuesheet += ", ";
            cuesheet += std::to_string(ci.cue_id) + ", " + std::to_string(ci.condition_id);
        }
        tag(f, 2, "CueSheet", cuesheet);
        close(f, 1, "Scene");
    }
    close(f, 1, "SceneRepository");

    // Build a map from sample index (bank_name + sample_index) to filename.
    // The sample_filenames from smpf > str are ordered sequentially. Each segment's
    // nested smp chunks reference samples by bank_name + index. We distribute the
    // filenames to segments by matching the smp references to filenames via their
    // sequential position in the global sample list.
    // For the initial implementation, we assign filenames to segments sequentially:
    // iterate segments in order, and for each segment with N samples, consume the
    // next N filenames from the global list.
    std::unordered_map<uint32_t, std::vector<std::string>> segment_filenames;
    {
        size_t filename_cursor = 0;
        for (const auto* seg : mc.segments) {
            auto sit = mc.segment_samples.find(seg->segment_id);
            if (sit != mc.segment_samples.end() && !sit->second.samples.empty()) {
                size_t sample_count = sit->second.samples.size();
                for (size_t i = 0; i < sample_count && filename_cursor < mc.sample_filenames.size(); ++i) {
                    segment_filenames[seg->segment_id].push_back(mc.sample_filenames[filename_cursor++]);
                }
            }
        }
    }

    // Segments
    open(f, 1, "SegmentRepository");
    for (const auto* seg : mc.segments) {
        fprintf(f, "<Segment ID=\"%u\">\n", seg->segment_id);
        // Look up which theme owns this segment
        auto thIt = segment_to_theme.find(seg->segment_id);
        tagi(f, 2, "Theme", thIt != segment_to_theme.end() ? thIt->second : 0);
        tagi(f, 2, "Timeline", seg->timeline_id);
        // Generate a short segment name from: first filename stem, or theme_name + ID
        {
            std::string seg_name;
            auto fnIt = segment_filenames.find(seg->segment_id);
            if (fnIt != segment_filenames.end() && !fnIt->second.empty()) {
                // Use first filename's stem (strip path and extension)
                const auto& fn = fnIt->second[0];
                size_t slash = fn.rfind('/');
                size_t dot = fn.rfind('.');
                size_t start = (slash != std::string::npos) ? slash + 1 : 0;
                if (dot != std::string::npos && dot > start)
                    seg_name = fn.substr(start, dot - start);
                else
                    seg_name = fn.substr(start);
            } else {
                // No filename — use theme name + segment ID
                auto thIt2 = segment_to_theme.find(seg->segment_id);
                uint32_t tid = thIt2 != segment_to_theme.end() ? thIt2->second : 0;
                auto tnIt = theme_names.find(tid);
                if (tnIt != theme_names.end() && !tnIt->second.empty())
                    seg_name = tnIt->second + "_" + std::to_string(seg->segment_id);
                else
                    seg_name = "seg_" + std::to_string(seg->segment_id);
            }
            tag(f, 2, "Name", seg_name);
        }
        tag(f, 2, "Notes", "");

        // Sample filenames from smpf > str chunk. The source audio is extracted
        // from sibling FSBs as WAV files by fev_project_setup. Original .aif
        // extensions are replaced with .wav since we extract as WAV format.
        {
            auto fnIt = segment_filenames.find(seg->segment_id);
            if (fnIt != segment_filenames.end()) {
                for (const auto& fn : fnIt->second) {
                    std::string fixed = fn;
                    // Replace .aif extension with .wav (we extract as WAV)
                    if (fixed.size() >= 4) {
                        std::string ext = fixed.substr(fixed.size() - 4);
                        for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                        if (ext == ".aif") fixed = fixed.substr(0, fixed.size() - 4) + ".wav";
                    }
                    tag(f, 2, "Filename", fixed);
                }
            }
        }

        fprintf(f, "<TimeSignature>%d/%d</TimeSignature>\n",
                seg->time_signature_beats, seg->time_signature_beat_value);
        open(f, 2, "TransitionType");
        tagi(f, 3, "end", 1);
        tagi(f, 3, "bar", 0);
        tagi(f, 3, "beat", 0);
        close(f, 2, "TransitionType");
        tagf(f, 2, "BeatsPerMinute", seg->beats_per_minute);
        // StepSequence: 16 x 2-bit sync beat values packed into a u32
        // sync_beats[4] bytes → u32 (little-endian)
        uint32_t step_seq = static_cast<uint32_t>(seg->sync_beats[0])
                          | (static_cast<uint32_t>(seg->sync_beats[1]) << 8)
                          | (static_cast<uint32_t>(seg->sync_beats[2]) << 16)
                          | (static_cast<uint32_t>(seg->sync_beats[3]) << 24);
        tagi(f, 2, "StepSequence", step_seq);
        auto sit = mc.segment_samples.find(seg->segment_id);
        if (sit != mc.segment_samples.end())
            tagi(f, 2, "SamplePlayMode", static_cast<int>(sit->second.playback_mode));
        close(f, 1, "Segment");
    }
    close(f, 1, "SegmentRepository");

    // ThemeFactory — must come before ThemeRepository
    open(f, 1, "ThemeFactory");
    tagi(f, 2, "UID", max_theme_id + 1);
    close(f, 1, "ThemeFactory");

    // theme_names already built above (before CompositionUI)

    // Themes
    open(f, 1, "ThemeRepository");
    for (const auto* thm : mc.themes) {
        fprintf(f, "<Theme ID=\"%u\">\n", thm->theme_id);
        // Use cue-derived name if available, otherwise leave empty
        auto nameIt = theme_names.find(thm->theme_id);
        tag(f, 2, "Name", nameIt != theme_names.end() ? nameIt->second : "");
        tag(f, 2, "Notes", "");
        tagi(f, 2, "StartMode", static_cast<int>(thm->playback_method));
        tagi(f, 2, "TransitionStyle", static_cast<int>(thm->default_transition));
        tagi(f, 2, "SyncStyle", static_cast<int>(thm->quantization));

        for (uint32_t sid : thm->start_sequence_ids) {
            fprintf(f, "<StartSegment SegmentID=\"%u\">\n", sid);
            tagi(f, 3, "Weight", 0);
            // Write theme conditions on StartSegments if available
            auto tcIt = mc.theme_conditions.find(thm->theme_id);
            if (tcIt != mc.theme_conditions.end() &&
                (!tcIt->second.cms_conditions.empty() || !tcIt->second.cprm_conditions.empty())) {
                open(f, 3, "Condition");
                for (const auto* cms : tcIt->second.cms_conditions) {
                    open(f, 4, "MusicStateCondition");
                    tagi(f, 5, "Type", static_cast<int>(cms->condition_type));
                    tagi(f, 5, "ThemeID", cms->theme_id);
                    tagi(f, 5, "CueID", cms->cue_id);
                    close(f, 4, "MusicStateCondition");
                }
                for (const auto* cprm : tcIt->second.cprm_conditions) {
                    open(f, 4, "ParameterCondition");
                    tagi(f, 5, "Type", static_cast<int>(cprm->condition_type));
                    tagi(f, 5, "ParameterID", cprm->param_id);
                    tagi(f, 5, "Value1", cprm->value_1);
                    tagi(f, 5, "Value2", cprm->value_2);
                    close(f, 4, "ParameterCondition");
                }
                close(f, 3, "Condition");
            } else {
                fprintf(f, "<Condition/>\n");
            }
            close(f, 2, "StartSegment");
        }

        // StopSegments as comma-separated list
        std::string stops;
        for (uint32_t eid : thm->end_sequence_ids) {
            if (!stops.empty()) stops += ", ";
            stops += std::to_string(eid);
        }
        if (!stops.empty()) tag(f, 2, "StopSegments", stops);

        tagi(f, 2, "Timeout", thm->transition_timeout);
        tagi(f, 2, "CrossfadeLength", thm->crossfade_duration);
        close(f, 1, "Theme");
    }
    close(f, 1, "ThemeRepository");

    // TimelineFactory — must come before TimelineRepository
    open(f, 1, "TimelineFactory");
    tagi(f, 2, "UID", max_timeline_id + 1);
    close(f, 1, "TimelineFactory");

    // Timelines
    open(f, 1, "TimelineRepository");
    for (const auto* tl : mc.timelines) {
        fprintf(f, "<Timeline ID=\"%u\">\n", tl->timeline_id);
        tag(f, 2, "Name", "tl_" + std::to_string(tl->timeline_id));
        close(f, 1, "Timeline");
    }
    close(f, 1, "TimelineRepository");

    close(f, 0, "Composition");
}

} // namespace fdp_write
