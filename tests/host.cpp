// Loads Anvil.clap as a host does and checks it: description and ports, parameters, state, presets, the sequencer against
// the host's transport (timing to the sample, stop, restart, jumps, a loop that wraps inside a block), the same
// output whatever the block size, the buttons, MIDI triggering, a softer step in a louder one's attack and bounded
// output at extreme settings. Writes demo renders as WAV files into the output folder.
//   host Anvil.clap OUT_FOLDER
#include "wav.h"
#include "png.h"
#include "../src/melodee_screen.h"
#include "../src/sixteen_screen.h"

#include <clap/clap.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <cstdlib>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (! (c)) { ++failures; std::printf ("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

const clap_plugin_entry* entry = nullptr;
const clap_plugin_factory* factory = nullptr;

// The host side: log, and the two a preset load tells (counted); the plug-in must cope without the rest.
int rescans = 0, textRescans = 0, callbacks = 0, flushes = 0, paints = 0, processRequests = 0, presetsLoaded = 0, presetErrors = 0;
const void* hostExtension (const clap_host*, const char* id)
{
    static const clap_host_log log { [] (const clap_host*, clap_log_severity s, const char* m) { std::printf ("  [plug-in log %d] %s\n", (int) s, m); } };
    static const clap_host_params params { [] (const clap_host*, clap_param_rescan_flags f) {
                                               rescans += (f & CLAP_PARAM_RESCAN_VALUES) != 0;
                                               textRescans += (f & CLAP_PARAM_RESCAN_TEXT) != 0;
                                           },
                                           [] (const clap_host*, clap_id, clap_param_clear_flags) {}, [] (const clap_host*) { ++flushes; } };
    static const melodee_screen_host_t screen { [] (const void*, uint32_t) { ++paints; } };
    if (std::strcmp (id, MELODEE_SCREEN_CLAP_HOST) == 0) return &screen;
    static const clap_host_preset_load presetLoad {
        [] (const clap_host*, uint32_t, const char*, const char*, int32_t, const char*) { ++presetErrors; },
        [] (const clap_host*, uint32_t, const char*, const char*) { ++presetsLoaded; },
    };
    if (std::strcmp (id, CLAP_EXT_LOG) == 0) return &log;
    if (std::strcmp (id, CLAP_EXT_PARAMS) == 0) return &params;
    if (std::strcmp (id, CLAP_EXT_PRESET_LOAD) == 0) return &presetLoad;
    return nullptr;
}

clap_host host {
    CLAP_VERSION, nullptr, "Anvil test host", "tests", "", "1",
    hostExtension, [] (const clap_host*) {}, [] (const clap_host*) { ++processRequests; }, [] (const clap_host*) { ++callbacks; },
};

// Input events for one process call, in time order.
struct Events
{
    struct Any { union { clap_event_header header; clap_event_param_value param; clap_event_note note; clap_event_midi midi; }; };
    std::vector<Any> list;
    clap_input_events in { this, [] (const clap_input_events* e) { return (uint32_t) static_cast<const Events*> (e->ctx)->list.size(); },
                           [] (const clap_input_events* e, uint32_t i) { return &static_cast<const Events*> (e->ctx)->list[i].header; } };
    void param (uint32_t time, clap_id id, double value)
    {
        Any a {};
        a.param = { { sizeof (clap_event_param_value), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 }, id, nullptr, -1, -1, -1, -1, value };
        list.push_back (a);
    }
    void note (uint32_t time, uint16_t type, int16_t key, double velocity)
    {
        Any a {};
        a.note = { { sizeof (clap_event_note), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 }, -1, 0, 0, key, velocity };
        list.push_back (a);
    }
    // Raw MIDI, as a host with the MIDI dialect sends it (hdslaveOS forwards CC 120 / 123 this way).
    void midi (uint32_t time, uint8_t status, uint8_t a1, uint8_t a2)
    {
        Any a {};
        a.midi = { { sizeof (clap_event_midi), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 }, 0, { status, a1, a2 } };
        list.push_back (a);
    }
    void sort() { std::stable_sort (list.begin(), list.end(), [] (const Any& a, const Any& b) { return a.header.time < b.header.time; }); }
};

// What the plug-in sends back: parameter values it changed itself (the buttons returning to 0).
struct OutEvents
{
    std::vector<std::pair<clap_id, double>> values;
    std::vector<uint32_t> times; // each value's frame in its block
    clap_output_events out { this, [] (const clap_output_events* o, const clap_event_header* h) {
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* v = reinterpret_cast<const clap_event_param_value*> (h);
            static_cast<OutEvents*> (o->ctx)->values.push_back ({ v->param_id, v->value });
            static_cast<OutEvents*> (o->ctx)->times.push_back (h->time);
        }
        return true;
    } };
};

// The transport a block hears: Melodee's way, the place at the block's start counted on by its frames.
struct Transport
{
    bool playing = true;
    double bpm = 120.0, beats = 0.0;
};

struct Instance
{
    const clap_plugin* plugin = nullptr;
    const clap_plugin_params* params = nullptr;
    const clap_plugin_state* state = nullptr;
    const clap_plugin_preset_load* presets = nullptr;
    std::map<std::string, clap_id> ids;
    double rate = 48000.0;

    explicit Instance (double sampleRate = 48000.0, uint32_t maxFrames = 4096) : rate (sampleRate)
    {
        const auto* d = factory->get_plugin_descriptor (factory, 0);
        plugin = factory->create_plugin (factory, &host, d->id);
        CHECK (plugin != nullptr && plugin->init (plugin));
        params = static_cast<const clap_plugin_params*> (plugin->get_extension (plugin, CLAP_EXT_PARAMS));
        state = static_cast<const clap_plugin_state*> (plugin->get_extension (plugin, CLAP_EXT_STATE));
        presets = static_cast<const clap_plugin_preset_load*> (plugin->get_extension (plugin, CLAP_EXT_PRESET_LOAD));
        for (uint32_t i = 0; params && i < params->count (plugin); ++i)
        {
            clap_param_info info {};
            params->get_info (plugin, i, &info);
            ids[info.name] = info.id;
        }
        CHECK (plugin->activate (plugin, rate, 1, maxFrames));
        CHECK (plugin->start_processing (plugin));
    }
    ~Instance()
    {
        plugin->stop_processing (plugin);
        plugin->deactivate (plugin);
        plugin->destroy (plugin);
    }
    clap_id id (const std::string& name)
    {
        const auto it = ids.find (name);
        if (it == ids.end())
        {
            std::printf ("FAIL no parameter named \"%s\"\n", name.c_str());
            ++failures;
            return CLAP_INVALID_ID;
        }
        return it->second;
    }
    // A factory preset by its load key, as a host's preset browser loads it.
    bool loadPreset (const char* key)
    {
        return presets != nullptr && presets->from_location (plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key);
    }
    double value (const std::string& name)
    {
        double v = 0.0;
        params->get_value (plugin, id (name), &v);
        return v;
    }
    // The value whose text is `text` (a choice by its name).
    double valueOf (const std::string& name, const std::string& text)
    {
        double v = 0.0;
        CHECK (params->text_to_value (plugin, id (name), text.c_str(), &v));
        return v;
    }

    // Renders `frames` in blocks of `block`, with `events(frame, list)` adding events for the block starting at
    // `frame`; the transport is asked for each block.
    void render (uint32_t frames, uint32_t block, std::vector<float>& left, std::vector<float>& right,
                 const std::function<Transport (uint64_t)>& transport, const std::function<void (uint64_t, uint32_t, Events&)>& events = {},
                 OutEvents* out = nullptr)
    {
        left.assign (frames, 0.0f);
        right.assign (frames, 0.0f);
        OutEvents ignored;
        for (uint64_t at = 0; at < frames; at += block)
        {
            const uint32_t n = (uint32_t) std::min<uint64_t> (block, frames - at);
            const auto t = transport (at);
            clap_event_transport tr {};
            tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
            tr.flags = CLAP_TRANSPORT_HAS_TEMPO | (t.beats >= 0.0 ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0) | (t.playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
            tr.tempo = t.bpm;
            tr.song_pos_beats = (clap_beattime) std::llround (t.beats * (double) CLAP_BEATTIME_FACTOR);
            Events e;
            if (events)
                events (at, n, e);
            e.sort();
            float* outs[2] = { left.data() + at, right.data() + at };
            clap_audio_buffer buffer {};
            buffer.data32 = outs;
            buffer.channel_count = 2;
            clap_process p {};
            p.steady_time = (int64_t) at;
            p.frames_count = n;
            p.transport = &tr;
            p.audio_outputs = &buffer;
            p.audio_outputs_count = 1;
            p.in_events = &e.in;
            p.out_events = out ? &out->out : &ignored.out;
            CHECK (plugin->process (plugin, &p) != CLAP_PROCESS_ERROR);
        }
    }
    void set (Events& e, const std::string& name, double value) { e.param (0, id (name), value); }
    // Parameter changes before anything plays: one silent, stopped block.
    void setup (const std::vector<std::pair<std::string, double>>& values)
    {
        std::vector<float> l, r;
        render (1, 1, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0)
                for (auto& [name, v] : values)
                    set (e, name, v);
        });
    }
};

// Sample positions where a hit starts: where the signal passes `threshold`, in a rise that began after at least
// `gap` samples below `quiet`.
std::vector<long> onsets (const std::vector<float>& x, float threshold = 1e-3f, float quiet = 1e-4f, long gap = 96)
{
    std::vector<long> found;
    long silent = gap;      // samples below quiet so far
    bool armed = true;      // this rise began after a long enough quiet
    bool counted = false;   // and has been counted
    for (long i = 0; i < (long) x.size(); ++i)
    {
        const float a = std::fabs (x[(size_t) i]);
        if (a < quiet)
        {
            ++silent;
            continue;
        }
        if (silent > 0) // a rise starts here
            armed = silent >= gap, counted = false;
        silent = 0;
        if (armed && ! counted && a > threshold)
            found.push_back (i), counted = true;
    }
    return found;
}

Transport playingFrom (double startBeats, double bpm, double rate, uint64_t at)
{
    return { true, bpm, startBeats + (double) at * bpm / 60.0 / rate };
}

// A short, clean hit on every step: square VCO 1 only, no pitch envelope, fast VCA with the shortest decay.
std::vector<std::pair<std::string, double>> clickPatch (Instance& a)
{
    // VCO 1 at its lowest (4.5 Hz): within an attack the square is a steady level, so every hit starts alike.
    return { { "VCO 1 Freq", 0.0 }, { "VCO 1 Level", 1.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.0 }, { "VCO 1 EG Amt", 0.0 }, { "VCO 2 EG Amt", 0.0 },
             { "1-2 FM Amt", 0.0 }, { "VCO EG > FM 1-2 Amt", 0.0 }, { "Cutoff", 1.0 }, { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 },
             { "Noise/VCF Mod", 0.0 }, { "VCA EG", a.valueOf ("VCA EG", "Fast") }, { "VCA Decay", 0.0 }, { "Volume", 0.8 },
             { "Clock Div", a.valueOf ("Clock Div", "1/8") } };
}

void testDescription()
{
    CHECK (factory->get_plugin_count (factory) == 1);
    const auto* d = factory->get_plugin_descriptor (factory, 0);
    CHECK (d != nullptr && std::string (d->name) == "hdslave Anvil" && std::string (d->id) == "com.hdslave.anvil");
    bool instrument = false;
    for (const char* const* f = d->features; f && *f; ++f)
        instrument |= std::strcmp (*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0;
    CHECK (instrument);

    Instance a;
    const auto* audio = static_cast<const clap_plugin_audio_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_AUDIO_PORTS));
    const auto* notes = static_cast<const clap_plugin_note_ports*> (a.plugin->get_extension (a.plugin, CLAP_EXT_NOTE_PORTS));
    CHECK (audio && audio->count (a.plugin, true) == 0 && audio->count (a.plugin, false) == 1);
    clap_audio_port_info port {};
    CHECK (audio && audio->get (a.plugin, 0, false, &port) && port.channel_count == 2);
    CHECK (notes && notes->count (a.plugin, true) == 1 && notes->count (a.plugin, false) == 0);
}

void testParameters()
{
    Instance a;
    const uint32_t count = a.params->count (a.plugin);
    CHECK (count == 54);
    std::map<clap_id, int> seen;
    for (uint32_t i = 0; i < count; ++i)
    {
        clap_param_info info {};
        CHECK (a.params->get_info (a.plugin, i, &info));
        CHECK (++seen[info.id] == 1);
        CHECK (info.min_value <= info.default_value && info.default_value <= info.max_value);
        double v = 0.0;
        CHECK (a.params->get_value (a.plugin, info.id, &v) && v == info.default_value);
        char text[64] = {};
        CHECK (a.params->value_to_text (a.plugin, info.id, v, text, sizeof (text)) && text[0] != 0);
        double back = -1e9;
        CHECK (a.params->text_to_value (a.plugin, info.id, text, &back));
        // Text round trips to the same value, within the display's precision.
        CHECK (std::fabs (back - v) <= 0.02 * (info.max_value - info.min_value) + 1e-9);
    }
    for (const char* name : { "VCO Decay", "Seq Pitch Mod", "Hard Sync", "VCO 1 Freq", "VCO 1 EG Amt", "VCO 1 Wave", "VCO 2 Freq",
                              "VCO 2 EG Amt", "VCO 2 Wave", "1-2 FM Amt", "VCO EG > FM 1-2 Amt", "VCO 1 Level", "Noise Level",
                              "VCO 2 Level", "Cutoff", "Resonance", "VCF Mode", "VCF Decay", "VCF EG Amt", "Noise/VCF Mod", "VCA EG",
                              "VCA Decay", "Volume", "Clock Div", "Run", "Trigger", "Advance", "Pitch 1", "Pitch 8", "Velocity 1", "Velocity 8",
                              "Noise Color", "Length", "Swing", "Notes", "Direction", "Chance", "Ratchet", "Vel > Cutoff",
                              "Pitch > Cutoff", "VCF EG > VCO 2", "Quantize" })
        CHECK (a.ids.count (name) == 1);
}

void testState()
{
    std::vector<float> l1, r1, l2, r2;
    Instance a;
    a.setup ({ { "VCO 1 Freq", 0.31 }, { "1-2 FM Amt", 0.4 }, { "VCO EG > FM 1-2 Amt", -0.6 }, { "Pitch 3", 0.7 }, { "Velocity 5", 0.2 },
               { "Hard Sync", 1.0 }, { "VCF Mode", 1.0 } });
    std::vector<uint8_t> saved;
    clap_ostream os { &saved, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
        auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
        v->insert (v->end(), (const uint8_t*) d, (const uint8_t*) d + n);
        return (int64_t) n;
    } };
    CHECK (a.state->save (a.plugin, &os));
    Instance b;
    struct Reader { const std::vector<uint8_t>* v; size_t at; } reader { &saved, 0 };
    clap_istream is { &reader, [] (const clap_istream* s, void* to, uint64_t n) -> int64_t {
        auto* r = static_cast<Reader*> (s->ctx);
        const size_t k = std::min<size_t> ((size_t) n, r->v->size() - r->at);
        std::memcpy (to, r->v->data() + r->at, k);
        r->at += k;
        return (int64_t) k;
    } };
    CHECK (b.state->load (b.plugin, &is));
    for (auto& [name, id] : a.ids)
        CHECK (a.value (name) == b.value (name));
    b.setup ({}); // the same one-frame start as a's
    // A state load settles at once, with no glide, while values set by hand glide; both rest a stopped block
    // first (a silent engine takes its knobs as they are there), and from then the sounds are the same to the bit.
    const auto rest = [] (uint64_t) { return Transport { false, 120.0, 0.0 }; };
    a.render (128, 128, l1, r1, rest);
    b.render (128, 128, l2, r2, rest);
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    a.render (48000, 128, l1, r1, t);
    b.render (48000, 128, l2, r2, t);
    CHECK (l1 == l2 && r1 == r2);
    // A truncated or foreign state is refused.
    std::vector<uint8_t> junk { 'J', 'U', 'N', 'K' };
    Reader bad { &junk, 0 };
    clap_istream isBad { &bad, is.read };
    CHECK (! b.state->load (b.plugin, &isBad));
}

// The factory presets: listed by preset discovery as a host's browser reads them, loaded by key through
// preset-load, each setting every value (the demos check each one's values and sound).
struct Listed { std::string name, key; bool ours = false; uint32_t flags = 0; };
std::vector<Listed> listPresets (int& locations)
{
    std::vector<Listed> out;
    locations = -1;
    const auto* f = static_cast<const clap_preset_discovery_factory*> (entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID));
    CHECK (f != nullptr && entry->get_factory (CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT) == (const void*) f);
    if (f == nullptr || f->count (f) != 1)
        return out;
    const auto* d = f->get_descriptor (f, 0);
    CHECK (d != nullptr && f->get_descriptor (f, 1) == nullptr);
    struct Index
    {
        std::vector<clap_preset_discovery_location> found;
        clap_preset_discovery_indexer indexer {};
    } index;
    index.indexer = { CLAP_VERSION, "tests", "", "", "1", &index,
                      [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_filetype*) { return true; },
                      [] (const clap_preset_discovery_indexer* i, const clap_preset_discovery_location* l) {
                          static_cast<Index*> (i->indexer_data)->found.push_back (*l);
                          return true;
                      },
                      [] (const clap_preset_discovery_indexer*, const clap_preset_discovery_soundpack*) { return true; },
                      [] (const clap_preset_discovery_indexer*, const char*) -> const void* { return nullptr; } };
    CHECK (f->create (f, &index.indexer, "com.example.other") == nullptr);
    const auto* provider = f->create (f, &index.indexer, d->id);
    CHECK (provider != nullptr && provider->init (provider));
    if (provider == nullptr)
        return out;
    locations = (int) index.found.size();
    CHECK (locations == 1 && index.found[0].kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && index.found[0].location == nullptr);
    struct Receive
    {
        clap_preset_discovery_metadata_receiver r {};
        static std::vector<Listed>& of (const clap_preset_discovery_metadata_receiver* r) { return *static_cast<std::vector<Listed>*> (r->receiver_data); }
    } receive;
    using R = Receive;
    receive.r.receiver_data = &out;
    receive.r.on_error = [] (const clap_preset_discovery_metadata_receiver*, int32_t, const char*) { CHECK (false); };
    receive.r.begin_preset = [] (const clap_preset_discovery_metadata_receiver* r, const char* name, const char* key) {
        R::of (r).push_back ({ name ? name : "", key ? key : "" });
        return true;
    };
    receive.r.add_plugin_id = [] (const clap_preset_discovery_metadata_receiver* r, const clap_universal_plugin_id* id) {
        R::of (r).back().ours = std::string (id->abi) == "clap" && std::string (id->id) == "com.hdslave.anvil";
    };
    receive.r.set_soundpack_id = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_flags = [] (const clap_preset_discovery_metadata_receiver* r, uint32_t flags) { R::of (r).back().flags = flags; };
    receive.r.add_creator = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_description = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.set_timestamps = [] (const clap_preset_discovery_metadata_receiver*, clap_timestamp, clap_timestamp) {};
    receive.r.add_feature = [] (const clap_preset_discovery_metadata_receiver*, const char*) {};
    receive.r.add_extra_info = [] (const clap_preset_discovery_metadata_receiver*, const char*, const char*) {};
    CHECK (! provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere.preset", &receive.r));
    CHECK (out.empty());
    CHECK (provider->get_metadata (provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receive.r));
    provider->destroy (provider);
    return out;
}

void testPresets()
{
    int locations = 0;
    const auto listed = listPresets (locations);
    std::vector<std::string> names, keys;
    for (const auto& p : listed)
    {
        names.push_back (p.name), keys.push_back (p.key);
        CHECK (p.ours && (p.flags & CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT) != 0);
    }
    CHECK ((names == std::vector<std::string> { "Default", "Kick", "FM Perc", "Sync Bass", "Noise Hat", "Acid Steps", "Ratchet Snare",
                                                "Random Zap", "Ping-Pong Toms", "Pad Kit" }));
    CHECK ((keys == std::vector<std::string> { "default", "kick", "fm-perc", "sync-bass", "noise-hat", "acid-steps", "ratchet-snare",
                                               "random-zap", "ping-pong-toms", "pad-kit" }));

    Instance a;
    CHECK (a.presets != nullptr);
    // Unknown keys and kinds are refused, and the host hears why.
    const int errors = presetErrors;
    CHECK (! a.loadPreset ("no-such-preset") && ! a.loadPreset (nullptr));
    CHECK (! a.presets->from_location (a.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, "/nowhere", "kick"));
    CHECK (presetErrors == errors + 3);
    // Default sets everything back, whatever was turned before; the host is told the values moved.
    a.setup ({ { "Volume", 0.1 }, { "Cutoff", 0.2 }, { "Hard Sync", 1.0 }, { "Pitch 4", -0.5 }, { "Clock Div", 7.0 } });
    const int rescanned = rescans, loaded = presetsLoaded;
    CHECK (a.loadPreset ("default"));
    CHECK (rescans == rescanned + 1 && presetsLoaded == loaded + 1);
    for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
    {
        clap_param_info info {};
        a.params->get_info (a.plugin, i, &info);
        double v = -1.0;
        a.params->get_value (a.plugin, info.id, &v);
        CHECK (v == info.default_value);
    }
    // Loaded while playing: the next block plays it (here, Kick's 1/8 steps instead of the 1/32 just set).
    std::vector<float> l, r;
    Instance b;
    b.setup ({ { "Clock Div", b.valueOf ("Clock Div", "1/32") } });
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    b.render (128, 128, l, r, t);
    CHECK (b.loadPreset ("kick") && b.value ("Clock Div") == b.valueOf ("Clock Div", "1/8"));
    b.render (48000, 128, l, r, t);
    bool finite = true;
    for (float s : l)
        finite &= std::isfinite (s);
    CHECK (finite);
}

void testTiming()
{
    // 120 BPM, eighth-note steps: a step every 12,000 frames at 48 kHz; the first sounds at frame 0 of Play.
    std::vector<float> l, r;
    Instance a;
    a.setup (clickPatch (a));
    a.render (48000 * 4, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
    auto hits = onsets (l);
    if (std::getenv ("ANVIL_DEBUG"))
    {
        std::printf ("debug: %zu hits:", hits.size());
        for (size_t i = 0; i < hits.size() && i < 20; ++i) std::printf (" %ld", hits[i]);
        float peak = 0; long last = -1; for (long i = 0; i < (long) l.size(); ++i) { if (std::fabs (l[i]) > peak) peak = std::fabs (l[i]); if (std::fabs (l[i]) > 1e-4f) last = i; }
        std::printf ("\ndebug: peak %f; |x|>1e-4 at 0..12000:", peak);
        long runs = 0; for (long i = 1; i < 12000; ++i) if ((std::fabs (l[i]) > 1e-4f) != (std::fabs (l[i - 1]) > 1e-4f)) { if (runs++ < 12) std::printf (" %ld", i); }
        std::printf ("  last loud %ld\n", last);
    }
    CHECK (hits.size() == 16);
    // From the second hit on (the first starts with the filter empty, a few frames slower to rise), to the frame.
    for (size_t i = 1; i < hits.size(); ++i)
        CHECK (std::labs (hits[i] - hits[1] - (long) (i - 1) * 12000) <= 2); // +-2: the square's sign the filter held
    CHECK (! hits.empty() && hits[0] < 48); // within the attack of frame 0
    CHECK (l == r);

    // Sixteenth notes at 90 BPM: 8,000 frames a step.
    Instance b;
    auto patch = clickPatch (b);
    patch.push_back ({ "Clock Div", b.valueOf ("Clock Div", "1/16") });
    b.setup (patch);
    b.render (48000 * 2, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 90.0, 48000.0, at); });
    hits = onsets (l);
    CHECK (hits.size() == 12);
    for (size_t i = 1; i < hits.size(); ++i)
        CHECK (std::labs (hits[i] - hits[1] - (long) (i - 1) * 8000) <= 1);

    // Other rates: 44.1 and 96 kHz keep the musical timing.
    for (double rate : { 44100.0, 96000.0 })
    {
        Instance c (rate);
        c.setup (clickPatch (c));
        c.render ((uint32_t) rate * 2, 128, l, r, [rate] (uint64_t at) { return playingFrom (0.0, 120.0, rate, at); });
        hits = onsets (l, 1e-3f, 1e-4f, (long) (rate / 500));
        if (std::getenv ("ANVIL_DEBUG"))
        {
            std::printf ("debug %.0f:", rate);
            for (long h : hits) std::printf (" %ld", h);
            std::printf ("\n");
        }
        CHECK (hits.size() == 8);
        for (size_t i = 1; i < hits.size(); ++i)
            CHECK (std::labs (hits[i] - hits[1] - (long) std::llround ((i - 1) * rate / 4)) <= 1);
        CHECK (! hits.empty() && hits[0] < (long) (rate / 1000));
    }
}

void testStopRestartJump()
{
    std::vector<float> l, r;
    // Plays 1 s, stops for 1 s, plays again from the top: hits stop with the transport and start again on frame 0 of
    // the second Play.
    Instance a;
    a.setup (clickPatch (a));
    a.render (48000 * 3, 128, l, r, [] (uint64_t at) {
        if (at < 48000) return playingFrom (0.0, 120.0, 48000.0, at);
        if (at < 96000) return Transport { false, 120.0, 2.0 };
        return playingFrom (0.0, 120.0, 48000.0, at - 96000);
    });
    auto hits = onsets (l);
    CHECK (hits.size() == 8);
    for (long h : hits)
        CHECK (h < 48000 || h >= 96000);
    CHECK (hits.size() == 8 && hits[4] - 96000 < 48 && hits[4] >= 96000);

    // A jump into the middle of a step waits for the next step; a jump to just after a step's start (inside one
    // block, as when a loop wraps between two blocks) plays that step at once.
    Instance b;
    b.setup (clickPatch (b));
    constexpr uint64_t jump = 128 * 100; // a block boundary
    b.render (48000 * 2, 128, l, r, [] (uint64_t at) {
        if (at < jump) return playingFrom (0.0, 120.0, 48000.0, at);
        return playingFrom (3.3, 120.0, 48000.0, at - jump); // the next step starts at beat 3.5: 0.2 beat on
    });
    hits = onsets (l);
    const long expected = (long) jump + 4800;
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= (long) jump && h < expected - 10; }) == hits.end());
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= expected && h < expected + 48; }) != hits.end());

    Instance c;
    c.setup (clickPatch (c));
    constexpr uint64_t wrap = 128 * 40;
    c.render (48000, 128, l, r, [] (uint64_t at) {
        if (at < wrap) return playingFrom (1.0, 120.0, 48000.0, at);
        return playingFrom (100.0 / 24000.0, 120.0, 48000.0, at - wrap); // 100 frames past the top
    });
    hits = onsets (l);
    CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= (long) wrap && h < (long) wrap + 48; }) != hits.end());

    // A host with no song position (beats < 0 here: the flag left out): the sequence counts from Play itself.
    Instance d;
    d.setup (clickPatch (d));
    d.render (48000 * 2, 128, l, r, [] (uint64_t) { return Transport { true, 120.0, -1.0 }; });
    hits = onsets (l);
    CHECK (hits.size() == 8);
    for (size_t i = 1; i < hits.size(); ++i)
        CHECK (std::labs (hits[i] - hits[1] - (long) (i - 1) * 12000) <= 2);
}

// The clocks (0-based) a run's hits fell on, `clockFrames` apart.
std::vector<long> clocksOf (const std::vector<long>& hits, long clockFrames)
{
    std::vector<long> out;
    for (long h : hits)
        out.push_back ((h + clockFrames / 4) / clockFrames);
    return out;
}
double energy (const std::vector<float>& l, long from, long n)
{
    double e = 0.0;
    for (long i = from; i < from + n && i < (long) l.size(); ++i)
        e += (double) l[(size_t) i] * l[(size_t) i];
    return e;
}
// The click patch with every step at full velocity, at sixteenths: each hit alike, so one hit's energy is another's.
std::vector<std::pair<std::string, double>> evenClicks (Instance& a)
{
    auto p = clickPatch (a);
    for (int i = 1; i <= 8; ++i)
        p.push_back ({ "Velocity " + std::to_string (i), 1.0 });
    p.push_back ({ "Clock Div", a.valueOf ("Clock Div", "1/16") });
    return p;
}

void testLoopsAndClockChanges()
{
    std::vector<float> l, r;
    // A 4-beat loop at 110 BPM whose end falls inside a block (frame 104,727.3), the host telling of the wrap only at
    // the next block as Melodee does: the steps run on to the loop's end, which is its start. The downbeat is one
    // hit, not the loop's end and then the loop's start again 105 frames later: its energy is the hit before's.
    constexpr double perFrame = 110.0 / 60.0 / 48000.0, loopEnd = 4.0 / perFrame, step = 0.25 / perFrame;
    {
        Instance a;
        a.setup (evenClicks (a));
        a.render ((uint32_t) loopEnd + 24000, 128, l, r, [] (uint64_t at) { return Transport { true, 110.0, std::fmod ((double) at * perFrame, 4.0) }; });
        const double before = energy (l, (long) (loopEnd - step), 2000), down = energy (l, (long) loopEnd, 2000);
        if (std::getenv ("ANVIL_DEBUG")) std::printf ("debug wrap energy: %.4f then %.4f\n", before, down);
        CHECK (before > 0.0 && down < before * 1.15 && down > before * 0.85);
        const auto hits = onsets (l);
        CHECK (std::count_if (hits.begin(), hits.end(), [&] (long h) { return h > loopEnd - 100 && h < loopEnd + step - 100; }) == 1);
    }
    // A 2-beat loop with Chance 75 %: the step at the loop's end (the 8th clock) stays silent, the downbeat (clock 0)
    // sounds. Told a block late, the downbeat still plays, at most a block late, also when the host's blocks come in
    // parts smaller than that (Melodee plays a plug-in in parts between its steps and tells the place only at its own
    // blocks of 128).
    for (uint32_t part : { 128u, 32u })
    {
        constexpr double loop2 = 2.0 / perFrame;
        Instance a;
        auto p = evenClicks (a);
        p.push_back ({ "Chance", 0.75 });
        a.setup (p);
        a.render ((uint32_t) loop2 + 12000, part, l, r, [] (uint64_t at) {
            const uint64_t block = at / 128 * 128; // Melodee's block: the place at its start, counted on through it
            return Transport { true, 110.0, std::fmod ((double) block * perFrame, 2.0) + (double) (at - block) * perFrame };
        });
        const auto hits = onsets (l);
        const auto down = std::find_if (hits.begin(), hits.end(), [&] (long h) { return h > loop2 - 100; });
        if (std::getenv ("ANVIL_DEBUG")) std::printf ("debug wrap part %u: %ld\n", part, down == hits.end() ? -1L : *down);
        CHECK (down != hits.end() && *down >= (long) loop2 && *down < (long) loop2 + 128 + 60);
    }
    // Drift: a tempo change inside a block (120 to 125 BPM, 64 frames before the next block) moves the host's place a
    // few frames from where the steps counted: the step played 64 frames ago is not played again, the next comes at
    // the new tempo.
    {
        Instance a;
        a.setup (evenClicks (a));
        a.render (48000, 128, l, r, [] (uint64_t at) {
            if (at < 24064) return playingFrom (0.0, 120.0, 48000.0, at);
            return Transport { true, 125.0, 1.0 + (double) (at - 24000) * 125.0 / 60.0 / 48000.0 };
        });
        CHECK (energy (l, 24000, 2000) < energy (l, 18000, 2000) * 1.15);
        const auto hits = onsets (l);
        CHECK (std::find_if (hits.begin(), hits.end(), [] (long h) { return h >= 24000 + 5760 && h < 24000 + 5760 + 48; }) != hits.end());
    }
    // Clock Div changed on a downbeat while playing (1/16 to 1/8 at beat 2): that downbeat still plays.
    {
        Instance a;
        a.setup (evenClicks (a));
        a.render (96000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 48000) a.set (e, "Clock Div", a.valueOf ("Clock Div", "1/8"));
        });
        const auto got = clocksOf (onsets (l), 6000);
        if (std::getenv ("ANVIL_DEBUG")) { std::printf ("debug regrid:"); for (long c : got) std::printf (" %ld", c); std::printf ("\n"); }
        CHECK ((got == std::vector<long> { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14 }));
    }
    // A flush while active (the host's way between process calls) takes a button too: Trigger plays, and returns to 0.
    // A press stamped past its block (a host's mistake) is answered inside the block.
    {
        Instance a;
        a.setup (clickPatch (a));
        Events e;
        e.param (0, a.id ("Trigger"), 1.0);
        OutEvents out;
        a.params->flush (a.plugin, &e.in, &out.out);
        CHECK ((out.values == std::vector<std::pair<clap_id, double>> { { a.id ("Trigger"), 0.0 } }));
        OutEvents late;
        a.render (12000, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& ev) {
            if (at == 128 * 50) ev.param (135, a.id ("Trigger"), 1.0);
        }, &late);
        const auto hits = onsets (l);
        CHECK (! hits.empty() && hits[0] < 48);
        CHECK (late.times.size() == 1 && late.times[0] <= 127);
    }
}

// --- The song's place a little off its line ------------------------------------------------------------------------
// hdslaveOS tells the place only at its own 128-frame blocks, worked out from its ticks, and it is not quite a straight
// line from block to block: a swung tick moves it by up to 0.2 tick (0.0021 of a quarter at swing 60), a tick on a
// block's edge and MTC's speed trim by less, and a plug-in two tracks play is run twice a block (its second call told a
// block on, the next block's first call a block back). None of that is a locate: no step may play twice, none be lost.

// One process call: its frames, the place it is told, and where its output goes in the song (from its frame).
struct Call
{
    uint32_t frames;
    double beats;
    uint64_t at;
};

// The calls hdslaveOS makes over `blocks` of its blocks at `bpm` and `rate`: each block told the place `place (frame)`
// at its start, counted on through it. `cut (block)` in 1..127 plays a block in two calls split there (an instrument
// is played in parts, split where a note falls); `twice` runs every block twice (a plug-in two tracks share).
std::vector<Call> hostCalls (uint64_t blocks, double bpm, double rate, const std::function<double (uint64_t)>& place,
                             const std::function<uint32_t (uint64_t)>& cut = {}, bool twice = false)
{
    const double perFrame = bpm / 60.0 / rate;
    std::vector<Call> calls;
    for (uint64_t b = 0; b < blocks; ++b)
    {
        const uint64_t at = b * 128;
        for (int run = 0; run < (twice ? 2 : 1); ++run)
        {
            const double told = place (at) + run * 128 * perFrame;
            const uint32_t s = cut ? cut (b) : 0;
            if (s > 0 && s < 128)
            {
                calls.push_back ({ s, told, at });
                calls.push_back ({ 128 - s, told + s * perFrame, at + s });
            }
            else
                calls.push_back ({ 128, told, at });
        }
    }
    return calls;
}

// Plays the calls, the song playing throughout; each call's output is added where it goes.
std::vector<float> playCalls (Instance& a, const std::vector<Call>& calls, double bpm)
{
    std::vector<float> l (calls.empty() ? 0 : calls.back().at + calls.back().frames, 0.0f), cl (128), cr (128);
    Events none;
    OutEvents ignored;
    int64_t steady = 0;
    for (const auto& c : calls)
    {
        clap_event_transport tr {};
        tr.header = { sizeof (clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
        tr.tempo = bpm;
        tr.song_pos_beats = (clap_beattime) std::llround (c.beats * (double) CLAP_BEATTIME_FACTOR);
        std::fill (cl.begin(), cl.end(), 0.0f);
        std::fill (cr.begin(), cr.end(), 0.0f);
        float* outs[2] = { cl.data(), cr.data() };
        clap_audio_buffer buffer {};
        buffer.data32 = outs;
        buffer.channel_count = 2;
        clap_process p {};
        p.steady_time = steady;
        p.frames_count = c.frames;
        p.transport = &tr;
        p.audio_outputs = &buffer;
        p.audio_outputs_count = 1;
        p.in_events = &none.in;
        p.out_events = &ignored.out;
        CHECK (a.plugin->process (a.plugin, &p) != CLAP_PROCESS_ERROR);
        steady += c.frames;
        for (uint32_t i = 0; i < c.frames; ++i)
            l[(size_t) (c.at + i)] += cl[i];
    }
    return l;
}

// hdslaveOS's place at a frame with the sequencer's swing (`swing` %): its ticks (96 a quarter) are swing/50 of their
// length in the first sixteenth of each eighth and (100 - swing)/50 in the second, and the place is the next tick's
// number less the frames still to it in plain ticks (EngineHost: song = (tick_now - until_tick / tick_length) / 96).
// So it stands 0.2 tick back at each long tick's start (swing 60), 0.2 on at each short one's; never before the top.
struct SwungPlace
{
    double base;   // frames a plain tick
    double swing;
    std::vector<double> starts { 0.0 }; // each tick's first frame
    size_t from = 0;                    // the tick the last frame asked for was in
    SwungPlace (double bpm, double rate, double swingPercent) : base (rate * 60.0 / bpm / 96.0), swing (swingPercent) {}
    double length (size_t k) const { return (k % 48) < 24 ? base * swing / 50.0 : base * (100.0 - swing) / 50.0; }
    // The first frame of tick k.
    double tickAt (size_t k)
    {
        while (starts.size() <= k + 1)
            starts.push_back (starts.back() + length (starts.size() - 1));
        return starts[k];
    }
    double operator() (uint64_t frame)
    {
        size_t k = tickAt (from) <= (double) frame ? from : 0;
        while (tickAt (k + 1) <= (double) frame)
            ++k;
        from = k;
        return std::max (0.0, ((double) k + 1.0 - (tickAt (k + 1) - (double) frame) / base) / 96.0);
    }
};

// How a run's clocks played: each clock expected at expect[k] (frames), its stretch's energy against a lone hit's. A
// clock played twice holds more (its hit begun again before it died away), a lost one almost none; `worst`: the
// farthest a hit started from its clock. Only the stretches that end inside the song count (the place told near its
// end may bring the next clock in early).
struct Played
{
    int clocks = 0, hits = 0, twice = 0, lost = 0;
    long worst = 0;
};
Played played (const std::vector<float>& l, const std::vector<double>& expect, double lone, double twiceAbove)
{
    Played p;
    const long early = 400; // a stretch starts this far before its clock
    long end = 0;           // where the last stretch counted ends
    for (size_t k = 0; k + 1 < expect.size() && (long) expect[k + 1] - early <= (long) l.size(); ++k)
    {
        const long from = std::max (0L, (long) expect[k] - early);
        end = (long) expect[k + 1] - early;
        const double e = energy (l, from, end - from);
        ++p.clocks;
        p.twice += e > lone * twiceAbove;
        p.lost += e < lone * 0.5;
    }
    for (long h : onsets (l))
    {
        if (h >= end)
            break;
        ++p.hits;
        long nearest = 1L << 40;
        for (double x : expect)
            nearest = std::min (nearest, std::labs (h - (long) std::llround (x)));
        p.worst = std::max (p.worst, nearest);
    }
    return p;
}

// The median of the stretches' energies: a lone hit's, from a straight run.
double loneHit (const std::vector<float>& l, const std::vector<double>& expect)
{
    std::vector<double> e;
    for (size_t k = 0; k + 1 < expect.size(); ++k)
        e.push_back (energy (l, std::max (0L, (long) expect[k] - 400), (long) (expect[k + 1] - expect[k])));
    std::nth_element (e.begin(), e.begin() + (long) e.size() / 2, e.end());
    return e.empty() ? 0.0 : e[e.size() / 2];
}

void testPositionJitter()
{
    const double rate = 44100.0;
    const bool debug = std::getenv ("ANVIL_DEBUG") != nullptr;
    // Each tempo's song: 1/32 clicks, every one alike; the clocks of a straight place, where they fall; a lone hit.
    for (double bpm : { 127.0, 40.0 })
    {
        const double perFrame = bpm / 60.0 / rate, grid = 0.125;
        const double beats = bpm > 100.0 ? 64.0 : 24.0;
        const uint64_t blocks = (uint64_t) (beats / perFrame / 128.0);
        const uint64_t frames = blocks * 128;
        std::vector<double> straightAt;
        for (int k = 0; k * grid / perFrame < (double) frames + grid / perFrame; ++k) // one past the song's end
            straightAt.push_back (k * grid / perFrame);
        const long near = (long) (0.003 / perFrame) + 48; // within 0.003 quarter of its clock (the place's own error)
        auto song = [&] (const std::vector<Call>& calls) {
            Instance a (rate);
            a.setup (evenClicks (a));
            a.setup ({ { "Clock Div", a.valueOf ("Clock Div", "1/32") } });
            return playCalls (a, calls, bpm);
        };
        const auto straight = song (hostCalls (blocks, bpm, rate, [&] (uint64_t at) { return (double) at * perFrame; }));
        const double lone = loneHit (straight, straightAt);
        auto check = [&] (const char* name, const std::vector<float>& l, const std::vector<double>& expect, double twiceAbove, long within) {
            const auto p = played (l, expect, lone, twiceAbove);
            if (debug)
                std::printf ("debug jitter %.0f BPM %s: %d clocks, %d hits, %d twice, %d lost, worst %ld frames\n", bpm, name, p.clocks,
                             p.hits, p.twice, p.lost, p.worst);
            CHECK (p.twice == 0 && p.lost == 0);
            CHECK (p.hits == p.clocks);
            CHECK (p.worst <= within);
        };
        check ("straight", straight, straightAt, 1.12, 48);

        // Up to 0.002 quarter off the line, either way, block by block.
        std::mt19937 random (7);
        std::vector<double> off (blocks + 1);
        for (auto& o : off)
            o = std::uniform_real_distribution<double> (-0.002, 0.002) (random);
        auto jittered = [&] (uint64_t at) { return std::max (0.0, (double) at * perFrame + off[at / 128]); };
        check ("off 0.002", song (hostCalls (blocks, bpm, rate, jittered)), straightAt, 1.12, near);
        // The same, each block played in two parts split where a note falls.
        std::vector<uint32_t> cuts (blocks);
        for (auto& c : cuts)
            c = 1 + (uint32_t) (random() % 127);
        check ("off 0.002, in parts", song (hostCalls (blocks, bpm, rate, jittered, [&] (uint64_t b) { return cuts[b]; })), straightAt, 1.12, near);

        // The sequencer's swing at 60 %: the place stands back 0.2 tick at each long tick, on at each short one. The
        // clocks fall where the swung ticks put them.
        SwungPlace swung (bpm, rate, 60.0);
        std::vector<double> swungAt;
        for (size_t k = 0; k < straightAt.size(); ++k)
            swungAt.push_back (swung.tickAt (k * 12));
        check ("swing 60", song (hostCalls (blocks, bpm, rate, [&] (uint64_t at) { return swung (at); })), swungAt, 1.12, near);

        // Run twice a block (two tracks play it): each block's second run is told a block on, the next block's first a
        // block back. Each clock plays once, in the run told its place first (a block early in the song). The runs'
        // outputs overlap here, so a hit is set against the run's own median.
        std::vector<double> earlyAt { 0.0 };
        for (size_t k = 1; k < straightAt.size(); ++k)
            earlyAt.push_back (straightAt[k] - 128.0);
        const auto shared = song (hostCalls (blocks, bpm, rate, [&] (uint64_t at) { return (double) at * perFrame; }, {}, true));
        const double sharedLone = loneHit (shared, earlyAt);
        const auto p = played (shared, earlyAt, sharedLone, 1.4);
        if (debug)
            std::printf ("debug jitter %.0f BPM run twice: %d clocks, %d hits, %d twice, %d lost, worst %ld frames\n", bpm, p.clocks,
                         p.hits, p.twice, p.lost, p.worst);
        CHECK (p.twice == 0 && p.lost == 0 && p.hits == p.clocks && p.worst <= 48);
    }
    // A real locate still counts: the place moved back across a clock by more than a 64th note plays that clock again
    // when the song reaches it; by less, the clock is held. Two beats of 1/32 at 127 BPM, the place moved back 0.03
    // quarter after the clock at beat 1.
    {
        const double bpm = 127.0, perFrame = bpm / 60.0 / rate;
        const uint64_t blocks = (uint64_t) (2.0 / perFrame / 128.0), at = (uint64_t) (1.03 / perFrame / 128.0) * 128;
        for (double by : { 0.05, 0.08 })
        {
            Instance a (rate);
            a.setup (evenClicks (a));
            a.setup ({ { "Clock Div", a.valueOf ("Clock Div", "1/32") } });
            const auto l = playCalls (a, hostCalls (blocks, bpm, rate, [&] (uint64_t f) { return (double) f * perFrame - (f >= at ? by : 0.0); }), bpm);
            const size_t hits = onsets (l).size();
            if (debug)
                std::printf ("debug jitter locate back %.2f: %zu hits\n", by, hits);
            CHECK (hits == (by < 0.0625 ? 16u : 17u));
        }
    }
}

// The edges of a jump. Steps of 1/64 at 127 BPM (44.1 kHz), the song from beat 0 in hdslaveOS's blocks of 128.
void testJumpEdges()
{
    const double rate = 44100.0, bpm = 127.0, perFrame = bpm / 60.0 / rate;
    const bool debug = std::getenv ("ANVIL_DEBUG") != nullptr;
    auto clicks = [&] (Instance& a, std::vector<std::pair<std::string, double>> more = {}) {
        a.setup (evenClicks (a));
        more.push_back ({ "Clock Div", a.valueOf ("Clock Div", "1/64") });
        a.setup (more);
    };
    // A place told exactly a 64th note behind where the last block's frames led is no jump, as hdslaveOS has it (a move
    // of more than a 64th is one): the step played is held, whatever the block before was told. Before, the CLAP
    // rounding of the two places decided, so the same move was a jump after one block and not after another.
    {
        const uint64_t blocks = (uint64_t) (2.0 / perFrame / 128.0), at = (uint64_t) (1.02 / perFrame / 128.0) * 128;
        std::vector<size_t> counts;
        for (double off : { 0.001, -0.001, 0.0013, -0.0007, 0.0002, -0.0019, 0.0, 0.0017 })
        {
            Instance a (rate);
            clicks (a);
            auto place = [&] (uint64_t f) {
                const double before = (double) (at - 128) * perFrame + off; // the block before, told `off` off its line
                if (f < at - 128)
                    return (double) f * perFrame;
                if (f < at)
                    return before;
                return before + (double) (f - at + 128) * perFrame - 1.0 / 16.0;
            };
            counts.push_back (onsets (playCalls (a, hostCalls (blocks, bpm, rate, place), bpm)).size());
            if (debug)
                std::printf ("debug exactly a 64th back after %+.4f: %zu hits\n", off, counts.back());
        }
        CHECK (std::count (counts.begin(), counts.end(), counts[0]) == (long) counts.size());
        CHECK (counts[0] == 31); // the steps to beat 1.94, the song held a 64th behind: none played twice
    }
    // A move on by less than a 64th note that passes two clocks (Swing 75 at 1/64 puts a late clock 1/128 before the
    // next one: a locate on, a MIDI clock catching up) is a jump, as it was: the step the song landed on plays at once,
    // not the two passed on one frame (the louder of them). Here the song's place goes from just before clock 17 (a late
    // one, loud) to 0.002 quarter past clock 18 (soft) at a block's start.
    {
        std::vector<std::pair<std::string, double>> swung { { "Swing", 0.75 } };
        for (int s = 1; s <= 8; ++s)
            swung.push_back ({ "Velocity " + std::to_string (s), s % 2 == 0 ? 1.0 : 0.25 }); // odd clocks loud
        const double c15 = (14 + 1.5) * 0.0625, c16 = 1.0, c17 = (16 + 1.5) * 0.0625, c18 = 1.125;
        const uint64_t at = (uint64_t) (c17 / perFrame / 128.0) * 128, blocks = at / 128 + 40;
        const double landed = c18 + 0.002;
        Instance a (rate);
        clicks (a, swung);
        const auto l = playCalls (a, hostCalls (blocks, bpm, rate, [&] (uint64_t f) {
            return f < at ? (double) f * perFrame : landed + (double) (f - at) * perFrame; }), bpm);
        const double loud = energy (l, (long) (c15 / perFrame), 600), soft = energy (l, (long) (c16 / perFrame), 600);
        const double there = energy (l, (long) at, 600);
        if (debug)
            std::printf ("debug two clocks passed: loud %.4f soft %.4f, at the jump %.4f\n", loud, soft, there);
        CHECK (loud > soft * 4.0);
        CHECK (there > soft * 0.8 && there < soft * 1.25);
    }
}

void testBlockSizes()
{
    // The same transport and events heard in blocks of 128, 37 and 1 frames: the same output, to the bit.
    std::vector<float> l128, r128, l, r;
    auto patch = [] (Instance& a) {
        return std::vector<std::pair<std::string, double>> { { "1-2 FM Amt", 0.35 }, { "VCO EG > FM 1-2 Amt", 0.5 }, { "Hard Sync", 1.0 },
                                                             { "Noise/VCF Mod", 0.3 }, { "Resonance", 0.7 }, { "Clock Div", a.valueOf ("Clock Div", "1/16") } };
    };
    auto events = [] (Instance* a) {
        return [a] (uint64_t at, uint32_t n, Events& e) {
            const uint64_t when = 30000; // a cutoff move inside some block
            if (when >= at && when < at + n)
                e.param ((uint32_t) (when - at), a->id ("Cutoff"), 0.3);
        };
    };
    auto t = [] (uint64_t at) { return playingFrom (0.0, 133.0, 48000.0, at); };
    {
        Instance a;
        a.setup (patch (a));
        a.render (48000, 128, l128, r128, t, events (&a));
    }
    for (uint32_t block : { 37u, 1u, 4096u })
    {
        Instance a;
        a.setup (patch (a));
        a.render (48000, block, l, r, t, events (&a));
        CHECK (l == l128 && r == r128);
    }
    // Play landing mid-step (1/8 at 120 BPM: a step every 12000 frames): whether the step just past plays at once is
    // one host block's question (128 frames), whatever block size this host calls with — 60 frames in plays it at
    // every block size, 1000 frames in leaves it at every block size, to the bit.
    for (double past : { 60.0, 1000.0 })
    {
        const auto mid = [past] (uint64_t at) { return playingFrom (past / 24000.0, 120.0, 48000.0, at); };
        std::vector<float> first;
        for (uint32_t block : { 128u, 37u, 1u, 4096u })
        {
            Instance a;
            a.setup (clickPatch (a));
            a.render (48000, block, l, r, mid);
            if (block == 128)
                first = l;
            else
                CHECK (l == first);
        }
        const auto hits = onsets (first);
        if (past == 60.0)
            CHECK (! hits.empty() && hits[0] < 48); // the step just past plays at once
        else
            CHECK (! hits.empty() && hits[0] >= 11000 - 48 && hits[0] < 11000 + 48); // the next step is the first
    }
}

void testButtonsAndNotes()
{
    std::vector<float> l, r;
    // Stopped: Trigger fires the envelopes and returns to 0; a MIDI note does the same.
    Instance a;
    a.setup (clickPatch (a));
    OutEvents out;
    a.render (48000, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 128 * 10) a.set (e, "Trigger", 1.0);
        if (at == 128 * 200) e.note (5, CLAP_EVENT_NOTE_ON, 60, 1.0);
    }, &out);
    auto hits = onsets (l);
    CHECK (hits.size() == 2);
    CHECK (hits.size() == 2 && hits[0] >= 1280 && hits[0] < 1280 + 48 && hits[1] >= 25605 && hits[1] < 25605 + 48);
    CHECK (std::find (out.values.begin(), out.values.end(), std::pair<clap_id, double> { a.id ("Trigger"), 0.0 }) != out.values.end());

    // Run off: the host plays but the sequence holds and stays silent; Advance moves it a step without playing it;
    // Trigger then plays the step it is on.
    Instance b;
    auto patch = clickPatch (b);
    patch.push_back ({ "Run", 0.0 });
    b.setup (patch);
    b.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 128 * 50) b.set (e, "Advance", 1.0);
        if (at == 128 * 100) b.set (e, "Trigger", 1.0);
    });
    hits = onsets (l);
    CHECK (hits.size() == 1 && hits[0] >= 12800 && hits[0] < 12800 + 48);

    // Run switched on while the host plays: the step it is on plays at once, and the next steps follow the clock.
    Instance c;
    auto held = clickPatch (c);
    held.push_back ({ "Run", 0.0 });
    c.setup (held);
    c.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); }, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 128 * 30) c.set (e, "Run", 1.0);
    });
    hits = onsets (l);
    CHECK (hits.size() == 4 && hits[0] >= 3840 && hits[0] < 3840 + 48);
    CHECK (hits.size() == 4 && std::labs ((hits[1] - 12000) - (hits[0] - 3840)) <= 1 && std::labs ((hits[3] - 36000) - (hits[0] - 3840)) <= 1);
}

// A new instance whose very first block already plays (added while the song runs, or a host whose first call plays)
// plays exactly as one that waited a stopped block first: its defaults are no press of Run, so there is no step at
// once and no second step on the first clock, and the sequence is where the song puts it. Run switched on on a
// clock's own frame (with Play, or later) plays that clock's step, once.
void testFirstPlay()
{
    std::vector<float> l, r, fresh, waited;
    for (double start : { 0.0, 2.3, 4.0 })
    {
        const auto from = [start] (uint64_t at) { return playingFrom (start, 120.0, 48000.0, at); };
        Instance a; // no stopped block: its first block plays
        a.render (48000 * 3, 128, fresh, r, from);
        Instance b; // a stopped block first
        b.render (128, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; });
        b.render (48000 * 3, 128, waited, r, from);
        CHECK (fresh == waited);
        CHECK (onsets (fresh).size() == onsets (waited).size());
    }

    // Run switched on at frame 0 with Play: as if it had been on (one step at frame 0, step 1, the song's). From beat 2.3
    // too, between clocks: the song's place decides, not the held step.
    for (double start : { 0.0, 2.3 })
    {
        const auto from = [start] (uint64_t at) { return playingFrom (start, 120.0, 48000.0, at); };
        Instance a;
        a.setup ({ { "Run", 0.0 } });
        a.render (48000 * 2, 128, fresh, r, from, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) a.set (e, "Run", 1.0);
        });
        Instance b;
        b.setup ({ { "Run", 1.0 } });
        b.render (48000 * 2, 128, waited, r, from);
        CHECK (fresh == waited);
    }

    // Run switched on on a later clock's frame (1/8 at 120 BPM: clock 1 at frame 12000), held at step 1 till then:
    // that clock plays the step the song has there (step 2, velocity 0.6), once; not step 1 (1.0) and step 2 at once.
    {
        Instance a;
        auto held = clickPatch (a);
        held.push_back ({ "Run", 0.0 });
        a.setup (held);
        a.render (48000, 128, fresh, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); },
                  [&] (uint64_t at, uint32_t, Events& e) {
                      if (at == 12000 - 12000 % 128) e.param (12000 % 128, a.id ("Run"), 1.0);
                  });
        Instance b;
        b.setup (clickPatch (b));
        b.render (48000, 128, waited, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        const auto hits = onsets (fresh);
        CHECK (! hits.empty() && hits[0] >= 12000 && hits[0] < 12000 + 48);
        auto peak = [] (const std::vector<float>& x, long from, long to) {
            float p = 0.0f;
            for (long i = from; i < to; ++i) p = std::max (p, std::fabs (x[(size_t) i]));
            return p;
        };
        // Step 1's hit at frame 0 in the instance that ran from the top is velocity 1.0, step 2's at 12000 is 0.6.
        const float step1 = peak (waited, 0, 6000), step2 = peak (waited, 12000, 18000), got = peak (fresh, 12000, 18000);
        CHECK (step1 > 0.0f && std::fabs (got / step1 - step2 / step1) < 0.05f);
    }
}

void testLengthAndSwing()
{
    std::vector<float> l, r;
    auto peaks = [&] (long stepFrames, int steps) {
        std::vector<float> out;
        for (int k = 0; k < steps; ++k)
        {
            float m = 0.0f;
            for (long i = k * stepFrames; i < (k + 1) * stepFrames; ++i)
                m = std::max (m, std::fabs (l[(size_t) i]));
            out.push_back (m);
        }
        return out;
    };
    // Length 3: the steps' velocities go 1, 2, 3, 1, 2, 3 ...; the other steps (their velocity 0.7) never play.
    {
        Instance a;
        auto patch = clickPatch (a);
        patch.push_back ({ "Length", 3 });
        for (int i = 1; i <= 8; ++i)
            patch.push_back ({ "Velocity " + std::to_string (i), i == 1 ? 1.0 : i == 2 ? 0.45 : i == 3 ? 0.2 : 0.7 });
        a.setup (patch);
        a.render (48000 * 4, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        const auto p = peaks (12000, 16);
        if (std::getenv ("ANVIL_DEBUG")) { std::printf ("debug length peaks:"); for (float v : p) std::printf (" %.4f", v); std::printf ("\n"); }
        bool cycles = true;
        for (int k = 4; k < 16; ++k) // from the second hit on (the first starts with the filter empty)
            cycles &= std::fabs (p[(size_t) k] - p[(size_t) (k - 3)]) < 1e-3f * p[(size_t) (k - 3)] + 1e-6f;
        CHECK (cycles && p[3] > p[4] * 1.5f && p[4] > p[5] * 1.5f);
    }
    // Length 3 and Advance: Advance moves on within the three steps.
    {
        Instance a;
        auto patch = clickPatch (a);
        patch.push_back ({ "Length", 3 });
        patch.push_back ({ "Run", 0.0 });
        for (int i = 1; i <= 8; ++i)
            patch.push_back ({ "Velocity " + std::to_string (i), i == 1 ? 1.0 : i == 2 ? 0.45 : i == 3 ? 0.2 : 0.7 });
        a.setup (patch);
        // Stopped: Trigger plays step 1 (once to warm the filter, once to measure); three Advances later it is
        // step 1 again, and one more Advance step 2.
        a.render (48000 * 3, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0 || at == 128 * 100 || at == 128 * 300 || at == 128 * 500) a.set (e, "Trigger", 1.0);
            if (at == 128 * 200) { e.param (0, a.id ("Advance"), 1.0); e.param (1, a.id ("Advance"), 1.0); e.param (2, a.id ("Advance"), 1.0); }
            if (at == 128 * 400) a.set (e, "Advance", 1.0);
        });
        auto peakAt = [&] (size_t from) { float m = 0.0f; for (size_t i = from; i < from + 6000; ++i) m = std::max (m, std::fabs (l[i])); return m; };
        const float first = peakAt (12800), again = peakAt (38400), second = peakAt (64000);
        if (std::getenv ("ANVIL_DEBUG")) std::printf ("debug advance: %.4f %.4f %.4f\n", first, again, second);
        CHECK (first > 0.01f && std::fabs (first - again) < 0.02f * first && second < first * 0.7f);
    }
    // Swing: the second step of each pair comes late, at the swing's share of the pair (eighths at 120 BPM: a pair is
    // 24,000 frames).
    for (double swing : { 0.66, 0.75 })
    {
        Instance a;
        auto patch = clickPatch (a);
        patch.push_back ({ "Swing", swing });
        a.setup (patch);
        a.render (48000 * 4, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        const auto hits = onsets (l);
        if (std::getenv ("ANVIL_DEBUG")) { std::printf ("debug swing %.2f:", swing); for (long h : hits) std::printf (" %ld", h); std::printf ("\n"); }
        CHECK (hits.size() == 16);
        bool placed = hits.size() == 16;
        auto at = [swing] (size_t i) { return (double) (i / 2) * 24000.0 + ((i & 1) ? 24000.0 * (float) swing : 0.0); };
        for (size_t i = 3; placed && i < hits.size(); ++i) // from the third hit, the first after the filter warmed
            placed = std::labs ((hits[i] - hits[2]) - std::lround (at (i) - at (2))) <= 2;
        CHECK (placed);
    }
    // A jump past an odd step's grid line but before its late clock: that step still comes, at its swung place.
    {
        Instance a;
        auto patch = clickPatch (a);
        patch.push_back ({ "Swing", 0.75 });
        a.setup (patch);
        const uint64_t jump = 128 * 50;
        a.render (48000, 128, l, r, [jump] (uint64_t at) {
            if (at < jump) return playingFrom (0.0, 120.0, 48000.0, at);
            return playingFrom (0.6, 120.0, 48000.0, at - jump); // the step's grid line is 0.5, its late clock 0.75
        });
        const auto hits = onsets (l);
        const long expected = (long) jump + 3600; // 0.15 beat on
        CHECK (std::find_if (hits.begin(), hits.end(), [&] (long h) { return h >= expected - 2 && h <= expected + 48; }) != hits.end());
    }
}

long crossings (const std::vector<float>& x, size_t from, size_t to, float hysteresis);

// Each step's own level (its velocity) tells which step played: the peak of each step's hit.
std::vector<float> stepPeaks (const std::vector<float>& l, long stepFrames, int steps)
{
    std::vector<float> out;
    for (int k = 0; k < steps; ++k)
    {
        float m = 0.0f;
        for (long i = k * stepFrames; i < (k + 1) * stepFrames && i < (long) l.size(); ++i)
            m = std::max (m, std::fabs (l[(size_t) i]));
        out.push_back (m);
    }
    return out;
}

// The step (0-based) whose level a peak is: the nearest of `levels`.
int whichStep (float peak, const std::vector<float>& levels)
{
    int best = 0;
    for (int i = 1; i < (int) levels.size(); ++i)
        if (std::fabs (levels[(size_t) i] - peak) < std::fabs (levels[(size_t) best] - peak))
            best = i;
    return best;
}

void testNotesAndDirection()
{
    std::vector<float> l, r;
    auto patch = [] (Instance& a, int length) {
        auto p = clickPatch (a);
        p.push_back ({ "Length", length });
        for (int i = 1; i <= 8; ++i)
            p.push_back ({ "Velocity " + std::to_string (i), 0.12 * i });
        return p;
    };
    // The steps' levels, from a forward run (from its second hit: the first starts with the filter empty).
    Instance f;
    f.setup (patch (f, 4));
    f.render (48000 * 3, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
    const auto forward = stepPeaks (l, 12000, 12);
    const std::vector<float> levels { forward[4], forward[5], forward[6], forward[7] };
    CHECK (levels[0] < levels[1] && levels[1] < levels[2] && levels[2] < levels[3]);
    // Backward 1 4 3 2, Ping-Pong 1 2 3 4 3 2, Random: the same steps when played again, not in order.
    struct Way { const char* name; std::vector<int> order; };
    for (const Way& way : { Way { "Backward", { 0, 3, 2, 1 } }, Way { "Ping-Pong", { 0, 1, 2, 3, 2, 1 } } })
    {
        Instance a;
        auto p = patch (a, 4);
        p.push_back ({ "Direction", a.valueOf ("Direction", way.name) });
        a.setup (p);
        a.render (48000 * 4, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        const auto peaks = stepPeaks (l, 12000, 16);
        bool ordered = true;
        for (int k = 1; k < 16; ++k)
            ordered &= whichStep (peaks[(size_t) k], levels) == way.order[(size_t) k % way.order.size()];
        CHECK (ordered);
    }
    std::vector<int> once, again;
    for (auto* steps : { &once, &again })
    {
        Instance a;
        auto p = patch (a, 4);
        p.push_back ({ "Direction", a.valueOf ("Direction", "Random") });
        a.setup (p);
        a.render (48000 * 4, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        const auto peaks = stepPeaks (l, 12000, 16);
        for (int k = 1; k < 16; ++k)
            steps->push_back (whichStep (peaks[(size_t) k], levels));
    }
    int inOrder = 0;
    for (size_t k = 1; k < once.size(); ++k)
        inOrder += once[k] == (once[k - 1] + 1) % 4;
    CHECK (once == again && inOrder < 10);
    // Notes: Steps, the transport stopped: notes 36 to 39 play steps 1 to 4, 44 is step 1 again, and a softer note
    // plays its step softer.
    Instance n;
    auto p = patch (n, 8);
    p.push_back ({ "Notes", n.valueOf ("Notes", "Steps") });
    n.setup (p);
    const int keys[] = { 36, 36, 37, 38, 39, 44, 39 };
    const double velocity[] = { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.5 };
    n.render (48000 * 2, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
        for (int i = 0; i < 7; ++i)
            if (at == (uint64_t) i * 12032) e.note (0, CLAP_EVENT_NOTE_ON, (int16_t) keys[i], velocity[i]);
    });
    const auto hit = stepPeaks (l, 12032, 7);
    CHECK (whichStep (hit[1], levels) == 0 && whichStep (hit[2], levels) == 1 && whichStep (hit[3], levels) == 2);
    CHECK (whichStep (hit[4], levels) == 3 && whichStep (hit[5], levels) == 0 && hit[6] < hit[4] * 0.7f);
}

void testChanceAndRatchet()
{
    std::vector<float> l, r, l2, r2;
    auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
    // Chance 0 %: nothing plays; 50 %: about half the steps, the same ones when played again.
    {
        Instance a;
        auto p = clickPatch (a);
        p.push_back ({ "Chance", 0.0 });
        a.setup (p);
        a.render (48000 * 2, 128, l, r, t);
        CHECK (onsets (l).empty());
    }
    std::vector<long> once, again;
    for (auto* hits : { &once, &again })
    {
        Instance a;
        auto p = clickPatch (a);
        p.push_back ({ "Chance", 0.5 });
        a.setup (p);
        a.render (48000 * 8, 128, l, r, t);
        *hits = onsets (l);
    }
    CHECK (once == again && once.size() >= 8 && once.size() <= 24); // of 32 steps
    // Ratchet 3: three hits a step, a third of a step apart (eighths at 120 BPM: 4,000 frames).
    Instance a;
    auto p = clickPatch (a);
    p.push_back ({ "Ratchet", 3 });
    p.push_back ({ "VCA Decay", 0.0 });
    a.setup (p);
    a.render (48000 * 2, 128, l, r, t);
    const auto hits = onsets (l, 1e-3f, 1e-4f, 96);
    CHECK (hits.size() == 24);
    bool spaced = hits.size() == 24;
    for (size_t i = 4; spaced && i < hits.size(); ++i) // from the second step on
        spaced = std::labs ((hits[i] - hits[3]) - (long) (i - 3) * 4000) <= 2;
    CHECK (spaced);
}

void testPatchRoutings()
{
    // A darker patch than the click: VCO 1 at 144 Hz through a low cutoff.
    auto render = [] (std::vector<std::pair<std::string, double>> extra) {
        Instance a;
        std::vector<std::pair<std::string, double>> v { { "VCO 1 Level", 1.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.0 }, { "VCO 1 Freq", 0.5 },
            { "VCO 1 EG Amt", 0.0 }, { "Cutoff", 0.35 }, { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 }, { "VCA Decay", 0.5 }, { "Clock Div", 3.0 },
            { "Velocity 1", 1.0 }, { "Velocity 2", 0.2 }, { "Pitch 1", 0.0 }, { "Pitch 2", 0.0 }, { "Length", 2.0 } };
        v.insert (v.end(), extra.begin(), extra.end());
        a.setup (v);
        std::vector<float> l, r;
        a.render (48000 * 2, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        return l;
    };
    // A routing moves the cutoff as far as turning Cutoff would (the knob spans 10 octaves): one step on repeat
    // (Length 1) with a routing, against no routing and the knob that far up: the same sound from the second step
    // (the first differs: a knob set just before Play is still easing in, where a routing lands on the step).
    auto rmsDiff = [] (const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0, e = 0;
        for (size_t i = 12000; i < a.size(); ++i) d += (double) (a[i] - b[i]) * (a[i] - b[i]), e += (double) b[i] * b[i];
        return std::sqrt (d / (e + 1e-30));
    };
    // Pitch > Cutoff +100 %: Pitch 1 an octave up (0.2 of +-5 octaves) takes the cutoff an octave (0.1) up; -100 % down.
    CHECK (rmsDiff (render ({ { "Length", 1 }, { "Pitch 1", 0.2 }, { "Pitch > Cutoff", 1.0 } }), render ({ { "Length", 1 }, { "Pitch 1", 0.2 }, { "Cutoff", 0.45 } })) < 1e-4);
    CHECK (rmsDiff (render ({ { "Length", 1 }, { "Pitch 1", 0.2 }, { "Pitch > Cutoff", -1.0 } }), render ({ { "Length", 1 }, { "Pitch 1", 0.2 }, { "Cutoff", 0.25 } })) < 1e-4);
    // Vel > Cutoff +100 %: velocity 0.5 takes it 2 octaves up (4 at full velocity).
    CHECK (rmsDiff (render ({ { "Length", 1 }, { "Velocity 1", 0.5 }, { "Vel > Cutoff", 1.0 }, { "Cutoff", 0.25 } }),
                    render ({ { "Length", 1 }, { "Velocity 1", 0.5 }, { "Cutoff", 0.45 } })) < 1e-4);
    // And without them the two steps' cutoffs are the same: the routings at 0 change nothing.
    CHECK (rmsDiff (render ({ { "Vel > Cutoff", 0.0 }, { "Pitch > Cutoff", 0.0 } }), render ({})) == 0.0);
    // VCF EG > VCO 2: VCO 2 alone, a long VCF EG: its pitch high at the hit and falling with the envelope.
    Instance a;
    a.setup ({ { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 1.0 }, { "VCO 2 Wave", 1.0 }, { "VCO 2 Freq", 0.4 }, { "VCO 2 EG Amt", 0.0 },
               { "Cutoff", 1.0 }, { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 }, { "VCF Decay", 0.6 }, { "VCA EG", 0.0 }, { "VCA Decay", 1.0 },
               { "VCF EG > VCO 2", 0.25 }, { "1-2 FM Amt", 0.0 } });
    std::vector<float> l, r;
    a.render (48000, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
        if (at == 0) a.set (e, "Trigger", 1.0);
    });
    const double early = crossings (l, 0, 2400, 0.002f) / 0.05, late = crossings (l, 38400, 48000, 0.002f) / 0.2;
    CHECK (early > late * 2.0);
}

void testVelocity()
{
    // A soft step after a loud one with a long tail: the tail is not cut (it keeps falling smoothly), and a
    // zero-velocity step adds nothing.
    std::vector<float> l, r;
    Instance a;
    auto patch = clickPatch (a);
    patch.push_back ({ "VCA Decay", 0.8 });
    for (int i = 2; i <= 8; ++i)
        patch.push_back ({ "Velocity " + std::to_string (i), i == 2 ? 0.0 : 0.1 });
    patch.push_back ({ "Velocity 1", 1.0 });
    a.setup (patch);
    a.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
    auto rms = [&] (long from, long n) { double s = 0; for (long i = from; i < from + n; ++i) s += (double) l[(size_t) i] * l[(size_t) i]; return std::sqrt (s / n); };
    const double before = rms (12000 - 960, 960), after = rms (12000, 960), later = rms (24000, 960);
    if (std::getenv ("ANVIL_DEBUG"))
        for (long at = 0; at < 48000; at += 2000)
            std::printf ("debug velocity %ld: %.4f\n", at, rms (at, 960));
    CHECK (before > 0.01 && after < before * 1.05 && after > before * 0.5); // step 2 (velocity 0): no dip, no new hit
    CHECK (later < after * 1.05);                                             // step 3 (0.1): below the tail, no rise
}

// A softer step while a louder one is still rising does not cut its attack: the hit sounds to the bit as the louder
// one alone until its peak (the softer step's shorter decay then takes over), and as loud.
void testSofterStepInAttack()
{
    std::vector<float> l, r, alone;
    auto patch = [] (Instance& a, const char* vcaEg) {
        auto p = clickPatch (a);
        p.push_back ({ "VCA EG", a.valueOf ("VCA EG", vcaEg) });
        p.push_back ({ "VCA Decay", 0.5 }); // 316 ms: the level at the peak is the hit's loudest
        return p;
    };
    auto firstDifference = [] (const std::vector<float>& a, const std::vector<float>& b) {
        long i = 0;
        while (i < (long) a.size() && i < (long) b.size() && a[(size_t) i] == b[(size_t) i])
            ++i;
        return i;
    };
    auto loudest = [] (const std::vector<float>& x) {
        float m = 0.0f;
        for (float s : x)
            m = std::max (m, std::fabs (s));
        return m;
    };
    // Notes: Steps, stopped: step 1 at velocity 1, then at 0.4 on the same frame (a host that gives a block's notes
    // all on its first frame) or half a millisecond later (24 frames: the first one's 1 ms attack half done).
    auto notes = [&] (std::vector<std::pair<uint64_t, double>> list, std::vector<float>& out) {
        Instance a;
        auto p = patch (a, "Fast");
        p.push_back ({ "Notes", a.valueOf ("Notes", "Steps") });
        a.setup (p);
        a.render (12000, 128, out, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t n, Events& e) {
            for (auto& [when, velocity] : list)
                if (when >= at && when < at + n)
                    e.note ((uint32_t) (when - at), CLAP_EVENT_NOTE_ON, 36, velocity);
        });
    };
    notes ({ { 1280, 1.0 } }, alone);
    CHECK (firstDifference (alone, std::vector<float> (alone.size(), 0.0f)) >= 1280 && loudest (alone) > 0.3f);
    for (uint64_t later : { 0, 24 })
    {
        notes ({ { 1280, 1.0 }, { 1280 + later, 0.4 } }, l);
        if (std::getenv ("ANVIL_DEBUG"))
            std::printf ("debug softer note %llu frames later: same until %ld, loudest %.4f of %.4f\n", (unsigned long long) later,
                         firstDifference (l, alone), loudest (l), loudest (alone));
        CHECK (firstDifference (l, alone) >= 1280 + 47 && loudest (l) > loudest (alone) * 0.999f);
    }
    // Two steps on one frame, the louder sent first or second (step 1 at 1.0, step 2 at 0.4, pitched up a fifth, Vel >
    // Cutoff on): the louder plays whole, its pitch and cutoff too, whatever order the host sends them in.
    auto pair = [&] (bool louderFirst, bool both, std::vector<float>& out) {
        Instance a;
        auto p = patch (a, "Fast");
        p.push_back ({ "Notes", a.valueOf ("Notes", "Steps") });
        p.push_back ({ "Pitch 2", 7.0 });
        p.push_back ({ "Vel > Cutoff", 0.5 });
        a.setup (p);
        a.render (12000, 128, out, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t n, Events& e) {
            if (1280 < at || 1280 >= at + n)
                return;
            const uint32_t t = (uint32_t) (1280 - at);
            if (louderFirst) e.note (t, CLAP_EVENT_NOTE_ON, 36, 1.0);
            if (both) e.note (t, CLAP_EVENT_NOTE_ON, 37, 0.4);
            if (! louderFirst) e.note (t, CLAP_EVENT_NOTE_ON, 36, 1.0);
        });
    };
    std::vector<float> loudAlone, loudFirst, loudSecond;
    pair (true, false, loudAlone);
    pair (true, true, loudFirst);
    pair (false, true, loudSecond);
    CHECK (loudest (loudAlone) > 0.3f && loudFirst == loudAlone && loudSecond == loudAlone);
    // VCA EG Slow (100 ms to the peak) at 1/32 (62.5 ms a step at 120 BPM): step 1 at velocity 1, step 2 at 0.6 while
    // step 1 still rises. Step 1 reaches its full peak, as with both steps at 1.
    auto steps = [&] (double second, std::vector<float>& out) {
        Instance a;
        auto p = patch (a, "Slow");
        p.push_back ({ "Clock Div", a.valueOf ("Clock Div", "1/32") });
        p.push_back ({ "Length", 2 });
        p.push_back ({ "Velocity 1", 1.0 });
        p.push_back ({ "Velocity 2", second });
        a.setup (p);
        a.render (6000, 128, out, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
    };
    steps (1.0, alone);
    steps (0.6, l);
    if (std::getenv ("ANVIL_DEBUG"))
        std::printf ("debug slow attack, a softer step 2: same until %ld, loudest %.4f of %.4f\n", firstDifference (l, alone), loudest (l), loudest (alone));
    CHECK (firstDifference (l, alone) >= 4750 && loudest (l) > loudest (alone) * 0.999f);
}

// Rising zero crossings (with a little hysteresis) in [from, to): the frequency of a clean wave, times the window.
long crossings (const std::vector<float>& x, size_t from, size_t to, float hysteresis = 0.002f)
{
    long n = 0;
    bool high = x[from] > 0.0f;
    for (size_t i = from; i < to; ++i)
    {
        if (! high && x[i] > hysteresis) high = true, ++n;
        else if (high && x[i] < -hysteresis) high = false;
    }
    return n;
}

void testPitchAndFm()
{
    // One held hit (Trigger with the transport stopped), VCO 1 alone as a triangle through an open filter.
    auto held = [] (Instance& a, std::vector<std::pair<std::string, double>> extra) {
        std::vector<std::pair<std::string, double>> v { { "VCO 1 Level", 1.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.0 },
            { "VCO 1 Wave", 1.0 }, { "VCO 2 Wave", 1.0 }, { "VCO 1 EG Amt", 0.0 }, { "VCO 2 EG Amt", 0.0 }, { "1-2 FM Amt", 0.0 },
            { "VCO EG > FM 1-2 Amt", 0.0 }, { "Cutoff", 1.0 }, { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 }, { "VCA EG", 0.0 },
            { "VCA Decay", 1.0 }, { "Volume", 0.5 }, { "VCO 1 Freq", 0.5 }, { "VCO 2 Freq", 0.5 } };
        v.insert (v.end(), extra.begin(), extra.end());
        a.setup (v);
        std::vector<float> l, r;
        a.render (48000, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) a.set (e, "Trigger", 1.0);
        });
        return l;
    };
    auto hz = [] (const std::vector<float>& l, double from, double to) { return crossings (l, (size_t) (from * 48000), (size_t) (to * 48000)) / (to - from); };
    {
        Instance a; // VCO 1 Freq at the middle: 4.5 Hz * 2^5 = 144 Hz
        const double f = hz (held (a, {}), 0.2, 0.7);
        CHECK (std::fabs (f - 144.0) <= 2.0);
    }
    {
        Instance a; // step 1's Pitch at +20 % of +-5 octaves: an octave up, on VCO 1 with Seq Pitch Mod at VCO 1&2
        const double f = hz (held (a, { { "Pitch 1", 0.2 }, { "Seq Pitch Mod", a.valueOf ("Seq Pitch Mod", "VCO 1&2") } }), 0.2, 0.7);
        CHECK (std::fabs (f - 288.0) <= 2.0);
    }
    for (const char* route : { "Off", "VCO 2" })
    {
        Instance a; // not on VCO 1 in either
        const double f = hz (held (a, { { "Pitch 1", 0.2 }, { "Seq Pitch Mod", a.valueOf ("Seq Pitch Mod", route) } }), 0.2, 0.7);
        CHECK (std::fabs (f - 144.0) <= 2.0);
    }
    {
        Instance a; // ...and VCO 2 hears it with VCO 2
        const double f = hz (held (a, { { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 1.0 }, { "Pitch 1", 0.2 },
                                         { "Seq Pitch Mod", a.valueOf ("Seq Pitch Mod", "VCO 2") } }), 0.2, 0.7);
        CHECK (std::fabs (f - 288.0) <= 2.0);
    }

    // The new knob: VCO EG > FM 1-2 Amt. VCO 2 alone at 144 Hz, VCO 1 at 36 Hz as the modulator, a short VCO EG.
    // Linear FM by a triangle at depth d raises VCO 2's average frequency (it cannot go below 0 Hz).
    const std::vector<std::pair<std::string, double>> fm { { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 1.0 }, { "VCO 1 Freq", 0.3 },
                                                           { "VCO Decay", 0.6 }, { "VCA EG", 0.0 } };
    auto with = [&] (double amount, double eg) {
        auto v = fm;
        v.push_back ({ "1-2 FM Amt", amount });
        v.push_back ({ "VCO EG > FM 1-2 Amt", eg });
        return v;
    };
    Instance plain, up, down;
    const auto none = held (plain, with (0.0, 0.0));
    const auto rising = held (up, with (0.0, 1.0));    // FM deep at the hit, gone as the VCO EG falls
    const auto falling = held (down, with (1.0, -1.0)); // FM gone at the hit, full as the VCO EG falls
    const double noneEarly = hz (none, 0.0, 0.05), noneLate = hz (none, 0.8, 1.0);
    const double upEarly = hz (rising, 0.0, 0.05), upLate = hz (rising, 0.8, 1.0);
    const double downEarly = hz (falling, 0.0, 0.05), downLate = hz (falling, 0.8, 1.0);
    if (std::getenv ("ANVIL_DEBUG"))
        std::printf ("debug fm: none %.0f/%.0f  up %.0f/%.0f  down %.0f/%.0f\n", noneEarly, noneLate, upEarly, upLate, downEarly, downLate);
    CHECK (std::fabs (noneEarly - 144.0) <= 25.0 && std::fabs (noneLate - 144.0) <= 6.0);
    CHECK (upEarly > noneEarly + 25.0 && upLate < noneLate + 15.0);      // deep FM at the hit, little by the end
    CHECK (downEarly < noneEarly + 15.0 && downLate > noneLate + 25.0);  // none at the hit, deep by the end
}

void testExtremes()
{
    // Random settings, every one in its range: the output stays a number and within full scale.
    std::mt19937 random (7);
    std::vector<float> l, r;
    for (int round = 0; round < 60; ++round)
    {
        Instance a;
        std::vector<std::pair<std::string, double>> values;
        for (uint32_t i = 0; i < a.params->count (a.plugin); ++i)
        {
            clap_param_info info {};
            a.params->get_info (a.plugin, i, &info);
            if (std::string (info.name) == "Trigger" || std::string (info.name) == "Advance")
                continue;
            std::uniform_real_distribution<double> d (info.min_value, info.max_value);
            double v = round < 2 ? (round == 0 ? info.min_value : info.max_value) : d (random);
            if (info.flags & CLAP_PARAM_IS_STEPPED)
                v = std::round (v);
            values.push_back ({ info.name, v });
        }
        a.setup (values);
        a.render (24000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 170.0, 48000.0, at); });
        bool finite = true;
        float peak = 0.0f;
        for (float s : l)
            finite &= std::isfinite (s), peak = std::max (peak, std::fabs (s));
        CHECK (finite && peak <= 1.0f);
    }
}

void demos (const std::string& folder)
{
    // A few patterns to listen to, 8 s each at 120 BPM: the factory presets. Each preset (loaded by its key) must
    // give exactly the values listed here and the same sound; the files are rendered from the preset.
    struct Demo { const char* file; const char* preset; std::vector<std::pair<std::string, double>> values; };
    Instance probe;
    const double fast = probe.valueOf ("VCA EG", "Fast"), slow = probe.valueOf ("VCA EG", "Slow");
    const double eighth = probe.valueOf ("Clock Div", "1/8"), sixteenth = probe.valueOf ("Clock Div", "1/16");
    std::vector<Demo> list {
        { "demo-default.wav", "default", {} },
        { "demo-kick.wav", "kick", { { "VCO 1 Freq", 0.18 }, { "VCO 1 EG Amt", 0.55 }, { "VCO Decay", 0.25 }, { "VCO 1 Wave", 1.0 }, { "VCO 2 Level", 0.0 },
                             { "Noise Level", 0.05 }, { "Cutoff", 0.45 }, { "VCF EG Amt", 0.3 }, { "VCF Decay", 0.3 }, { "VCA EG", fast },
                             { "VCA Decay", 0.45 }, { "Clock Div", eighth } } },
        { "demo-fm-perc.wav", "fm-perc", { { "1-2 FM Amt", 0.25 }, { "VCO EG > FM 1-2 Amt", 0.7 }, { "VCO Decay", 0.3 }, { "VCO 2 Level", 1.0 },
                                { "VCO 1 Level", 0.0 }, { "Cutoff", 0.7 }, { "Resonance", 0.4 }, { "VCA Decay", 0.35 }, { "Clock Div", sixteenth },
                                { "Pitch 2", 0.4 }, { "Pitch 4", -0.3 }, { "Pitch 6", 0.8 }, { "Velocity 3", 0.3 }, { "Velocity 7", 0.5 } } },
        { "demo-sync-bass.wav", "sync-bass", { { "Hard Sync", 1.0 }, { "VCO 2 EG Amt", 0.6 }, { "VCO 1 Freq", 0.3 }, { "VCO 2 Freq", 0.45 },
                                  { "VCO 1 Level", 0.3 }, { "VCO 2 Level", 0.9 }, { "Cutoff", 0.4 }, { "Resonance", 0.55 }, { "VCF EG Amt", 0.5 },
                                  { "VCA EG", slow }, { "VCA Decay", 0.5 }, { "Clock Div", sixteenth }, { "Pitch 1", 0.0 }, { "Pitch 3", 0.5 },
                                  { "Pitch 5", -0.4 }, { "Pitch 8", 0.25 } } },
        { "demo-noise-hat.wav", "noise-hat", { { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 1.0 }, { "VCF Mode", 1.0 }, { "Cutoff", 0.75 },
                                  { "Resonance", 0.3 }, { "VCA Decay", 0.15 }, { "Clock Div", sixteenth }, { "Velocity 2", 0.4 }, { "Velocity 4", 0.4 },
                                  { "Velocity 6", 0.4 }, { "Velocity 8", 0.4 } } },
        { "demo-acid-steps.wav", "acid-steps", { { "VCO 1 Freq", 0.34 }, { "VCO 2 Freq", 0.342 }, { "VCO 1 Level", 0.7 }, { "VCO 2 Level", 0.6 }, { "VCO 1 EG Amt", 0.0 }, { "VCO 2 EG Amt", 0.0 }, { "VCO Decay", 0.3 }, { "Cutoff", 0.42 }, { "Resonance", 0.6 }, { "VCF EG Amt", 0.45 }, { "VCF Decay", 0.33 }, { "VCA Decay", 0.42 }, { "Clock Div", 5.0 }, { "Length", 7.0 }, { "Swing", 0.58 }, { "Quantize", 4.0 }, { "Pitch > Cutoff", 0.5 }, { "Vel > Cutoff", 0.4 }, { "Pitch 3", 0.2 }, { "Pitch 4", 0.05 }, { "Pitch 6", 0.117 }, { "Pitch 7", 0.083 }, { "Velocity 2", 0.5 }, { "Velocity 3", 0.8 }, { "Velocity 4", 0.6 }, { "Velocity 5", 1.0 }, { "Velocity 6", 0.4 }, { "Velocity 7", 0.7 } } },
        { "demo-ratchet-snare.wav", "ratchet-snare", { { "VCO 1 Freq", 0.565 }, { "VCO 1 Wave", 1.0 }, { "VCO 1 EG Amt", 0.15 }, { "VCO Decay", 0.2 }, { "VCO 1 Level", 0.5 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.85 }, { "Cutoff", 0.72 }, { "Resonance", 0.25 }, { "VCF EG Amt", 0.2 }, { "VCF Decay", 0.25 }, { "VCA Decay", 0.3 }, { "Clock Div", 3.0 }, { "Ratchet", 2.0 }, { "Chance", 0.85 }, { "Velocity 1", 0.35 }, { "Velocity 2", 0.25 }, { "Velocity 3", 1.0 }, { "Velocity 4", 0.3 }, { "Velocity 5", 0.35 }, { "Velocity 6", 0.25 }, { "Velocity 7", 1.0 }, { "Velocity 8", 0.45 } } },
        { "demo-random-zap.wav", "random-zap", { { "VCO 2 Freq", 0.75 }, { "VCO 2 EG Amt", 0.7 }, { "VCO Decay", 0.28 }, { "1-2 FM Amt", 0.3 }, { "VCO EG > FM 1-2 Amt", 0.5 }, { "VCO 1 Freq", 0.45 }, { "VCO 1 Level", 0.0 }, { "VCO 2 Level", 1.0 }, { "Cutoff", 0.8 }, { "Resonance", 0.35 }, { "VCF EG Amt", 0.0 }, { "VCF EG > VCO 2", 0.25 }, { "VCF Decay", 0.35 }, { "VCA Decay", 0.32 }, { "Clock Div", 5.0 }, { "Direction", 3.0 }, { "Chance", 0.7 }, { "Quantize", 6.0 }, { "Pitch 2", 0.2 }, { "Pitch 3", -0.2 }, { "Pitch 4", 0.117 }, { "Pitch 5", 0.3 }, { "Pitch 6", -0.1 }, { "Pitch 7", 0.4 }, { "Pitch 8", 0.083 }, { "Velocity 2", 0.7 }, { "Velocity 3", 0.9 }, { "Velocity 4", 0.6 }, { "Velocity 6", 0.8 }, { "Velocity 7", 0.65 }, { "Velocity 8", 0.75 } } },
        { "demo-ping-pong-toms.wav", "ping-pong-toms", { { "VCO 1 Freq", 0.42 }, { "VCO 1 Wave", 1.0 }, { "VCO 1 EG Amt", 0.25 }, { "VCO Decay", 0.3 }, { "VCO 2 Freq", 0.425 }, { "VCO 2 Wave", 1.0 }, { "VCO 2 Level", 0.4 }, { "VCO 2 EG Amt", 0.25 }, { "Noise Level", 0.05 }, { "Cutoff", 0.6 }, { "Resonance", 0.2 }, { "VCF EG Amt", 0.25 }, { "VCF Decay", 0.35 }, { "VCA Decay", 0.45 }, { "Clock Div", 5.0 }, { "Length", 6.0 }, { "Direction", 2.0 }, { "Swing", 0.62 }, { "Quantize", 3.0 }, { "Pitch 1", 0.15 }, { "Pitch 2", 0.1 }, { "Pitch 3", 0.05 }, { "Pitch 5", -0.05 }, { "Pitch 6", -0.1 }, { "Velocity 2", 0.7 }, { "Velocity 3", 0.85 }, { "Velocity 4", 0.6 }, { "Velocity 5", 0.9 }, { "Velocity 6", 0.65 } } },
        { "demo-pad-kit.wav", "pad-kit", { { "Notes", 1.0 }, { "VCO 1 Wave", 1.0 }, { "VCO 1 Freq", 0.3 }, { "VCO 1 EG Amt", 0.5 }, { "VCO Decay", 0.22 }, { "VCO 1 Level", 1.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.35 }, { "Cutoff", 0.5 }, { "Resonance", 0.15 }, { "VCF EG Amt", 0.35 }, { "VCF Decay", 0.25 }, { "VCA Decay", 0.35 }, { "Pitch > Cutoff", 0.8 }, { "Vel > Cutoff", 0.3 }, { "Clock Div", 5.0 }, { "Pitch 2", 0.25 }, { "Pitch 3", 0.4 }, { "Pitch 4", 0.55 }, { "Pitch 5", 0.7 }, { "Pitch 6", 0.85 }, { "Pitch 7", 1.0 }, { "Pitch 8", 0.6 }, { "Velocity 2", 0.5 }, { "Velocity 3", 0.8 }, { "Velocity 4", 0.5 }, { "Velocity 5", 0.9 }, { "Velocity 6", 0.45 }, { "Velocity 7", 0.7 } } },
    };
    for (auto& demo : list)
    {
        Instance a, b;
        a.setup (demo.values);
        CHECK (b.loadPreset (demo.preset));
        b.setup ({}); // the same one-frame start as a's
        for (auto& [name, id] : a.ids)
            CHECK (a.value (name) == b.value (name));
        std::vector<float> l, r, l2, r2;
        // As in testState: a preset load settles at once and hand-set values glide, so both rest a stopped block.
        const auto rest = [] (uint64_t) { return Transport { false, 120.0, 0.0 }; };
        a.render (128, 128, l, r, rest);
        b.render (128, 128, l2, r2, rest);
        auto t = [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); };
        a.render (48000 * 8, 128, l, r, t);
        b.render (48000 * 8, 128, l2, r2, t);
        CHECK (l == l2 && r == r2);
        const auto path = folder + "/" + demo.file;
        CHECK (writeWav (path.c_str(), l2, r2, 48000));
    }
}
} // namespace

void testQuantize()
{
    // One held hit (Trigger, the transport stopped), VCO 1 alone as a triangle at 144 Hz through an open filter;
    // step 1's Pitch at 10.8 % of +-5 octaves: 6.48 semitones up.
    auto held = [] (Instance& a, std::vector<std::pair<std::string, double>> extra) {
        std::vector<std::pair<std::string, double>> v { { "VCO 1 Level", 1.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.0 },
            { "VCO 1 Wave", 1.0 }, { "VCO 1 EG Amt", 0.0 }, { "VCO 2 EG Amt", 0.0 }, { "Cutoff", 1.0 }, { "Resonance", 0.0 },
            { "VCF EG Amt", 0.0 }, { "VCA Decay", 1.0 }, { "Volume", 0.5 }, { "VCO 1 Freq", 0.5 } };
        v.insert (v.end(), extra.begin(), extra.end());
        a.setup (v);
        std::vector<float> l, r;
        a.render (48000, 128, l, r, [] (uint64_t) { return Transport { false, 120.0, 0.0 }; }, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) a.set (e, "Trigger", 1.0);
        });
        return crossings (l, 9600, 33600) / 0.5;
    };
    struct Case { const char* quantize; double pitch, hz; };
    for (const Case& c : { Case { "Off", 0.108, 209.5 }, Case { "Semitones", 0.108, 203.6 }, Case { "Major", 0.108, 215.7 },
                           Case { "Fifths", 0.108, 215.7 }, Case { "Octaves", 0.108, 288.0 }, Case { "Off", -0.108, 99.0 },
                           Case { "Semitones", -0.108, 101.8 }, Case { "Minor Pent", -0.108, 96.1 },
                           Case { "Octaves", -0.108, 72.0 } })
    {
        Instance a;
        const double f = held (a, { { "Pitch 1", c.pitch }, { "Quantize", a.valueOf ("Quantize", c.quantize) } });
        if (std::getenv ("ANVIL_DEBUG"))
            std::printf ("debug quantize %s %+.3f: %.1f Hz (want %.1f)\n", c.quantize, c.pitch, f, c.hz);
        CHECK (std::fabs (f - c.hz) <= 2.0);
    }
    // The steps' Pitch reads as Quantize plays it; a change of Quantize asks the host to read the texts again.
    Instance a;
    char text[64] = {};
    auto pitchText = [&] () { a.params->value_to_text (a.plugin, a.id ("Pitch 1"), 0.108, text, sizeof (text)); return std::string (text); };
    CHECK (pitchText() == "+6.5 st");
    a.plugin->on_main_thread (a.plugin); // as a host does after any earlier request
    const int callbacksBefore = callbacks, textBefore = textRescans;
    a.setup ({ { "Quantize", a.valueOf ("Quantize", "Semitones") } });
    CHECK (pitchText() == "+6 st");
    CHECK (callbacks > callbacksBefore);
    a.plugin->on_main_thread (a.plugin);
    CHECK (textRescans == textBefore + 1);
    a.setup ({ { "Quantize", a.valueOf ("Quantize", "Major") } });
    CHECK (pitchText() == "+7 st");
    double back = 0;
    CHECK (a.params->text_to_value (a.plugin, a.id ("Pitch 1"), "+7 st", &back) && std::fabs (back - 7.0 / 60.0) < 1e-6);
}

// Melodee's screen: the page draws, answers the wheel, Push and Minus / Plus, names K1-K3, sends what it changes to
// the host and the engine, and plays a step. Its pictures go to OUT_FOLDER/screen-*.png for a look.
void testScreen (const std::string& folder)
{
    Instance a;
    const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
    CHECK (screen != nullptr);
    if (screen == nullptr)
        return;
    std::vector<uint32_t> pixels (MELODEE_SCREEN_WIDTH * MELODEE_SCREEN_PAGE_HEIGHT, 0);
    melodee_screen_frame_t frame {};
    frame.pixels = pixels.data();
    frame.width = MELODEE_SCREEN_WIDTH, frame.height = MELODEE_SCREEN_PAGE_HEIGHT, frame.stride = MELODEE_SCREEN_WIDTH;
    frame.area = MELODEE_SCREEN_PAGE;
    frame.colour = 0x00ff8a3du;
    for (int k = 0; k < 3; ++k)
        frame.pots[k] = -1, frame.pots_live[k] = 1;
    uint64_t now = 1000;
    auto paint = [&] (bool whole = false) { frame.redraw_all = whole ? 1u : 0u, frame.time_ms = now; return screen->draw (a.plugin, &frame); };
    auto shot = [&] (const char* name) { paint (true); CHECK (writePng ((folder + "/screen-" + name + ".png").c_str(), pixels.data(), 320, 218, 320, 2)); };
    auto in = [&] (uint32_t kind, int value, bool shift = false) { melodee_screen_input_t e { kind, value, shift ? 1u : 0u }; return screen->input (a.plugin, &e); };
    auto knobs = [&] () { std::vector<uint32_t> ids (3); screen->controls (a.plugin, ids.data()); return ids; };
    auto play = [&] (uint32_t frames, bool playing, double fromBeats, OutEvents* out = nullptr) {
        std::vector<float> l, r;
        a.render (frames, 128, l, r, [&] (uint64_t at) { return playing ? playingFrom (fromBeats, 120.0, 48000.0, at) : Transport { false, 120.0, 0.0 }; }, {}, out);
        now += frames * 1000 / 48000;
        return l;
    };

    // The wire torus schedules its next pose even before the first audio block.
    int before = paints;
    CHECK (paint (true));
    CHECK (paints > before);
    CHECK (! paint());
    now += 800;
    CHECK (paint());
    now += 2600;
    before = paints;
    CHECK (paint());
    CHECK (paints > before);
    CHECK (! paint());
    // Stopped and left alone for 4 s, the page rests: it asks for no next paint (battery). Opened again, it animates.
    now += 4600;
    before = paints;
    paint();
    CHECK (paints == before);
    paint (true);
    CHECK (paints > before);
    // A value moved from outside the page (a pot, automation, a preset) asks for a paint, which shows it.
    before = paints;
    a.setup ({ { "Cutoff", 0.3 } });
    CHECK (paints > before);
    CHECK (paint());
    before = paints;
    CHECK (a.loadPreset ("kick"));
    CHECK (paints > before);
    CHECK (a.loadPreset ("default"));
    // STEPS: K1-K3 are step 1's Pitch and Velocity and the Cutoff; the wheel picks step 3.
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Pitch 1"), a.id ("Velocity 1"), a.id ("Cutoff") }));
    CHECK (in (MELODEE_SCREEN_WHEEL, 2));
    CHECK ((knobs() == std::vector<clap_id> { a.id ("Pitch 3"), a.id ("Velocity 3"), a.id ("Cutoff") }));
    // Push plays step 3 (stopped, nothing else would sound), and asks the host to run the plug-in. A press's age
    // counts frames rendered, never wall time: a host asleep between the press and its next block rendered none,
    // so the press still plays once at that block however long it slept — and no paint retires it, so whether the
    // page stayed open, or closed at once with no paint between press and wake, cannot change what sounds.
    auto quiet = play (4800, false, 0.0);
    CHECK (std::all_of (quiet.begin(), quiet.end(), [] (float v) { return v == 0.0f; }));
    const int processBefore = processRequests;
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    CHECK (processRequests > processBefore);
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    paint(); // the page paints on while the plug-in sleeps: a paint used to retire the waiting press; it must not
    auto hit = play (4800, false, 0.0);
    CHECK (std::any_of (hit.begin(), hit.end(), [] (float v) { return std::fabs (v) > 1e-3f; }));
    play (48000, false, 0.0); // the hit rings out
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    std::this_thread::sleep_for (std::chrono::milliseconds (600)); // press, page closed (no paint), then the wake:
    hit = play (4800, false, 0.0);                                  // the same sound as with the page open
    CHECK (std::any_of (hit.begin(), hit.end(), [] (float v) { return std::fabs (v) > 1e-3f; }));
    play (48000, false, 0.0);
    // A press queued across a deactivation is gone: the frame count restarts at activate, and no press of the
    // old count fires on the new one.
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    a.plugin->deactivate (a.plugin);
    CHECK (a.plugin->activate (a.plugin, 48000.0, 1, 4096));
    quiet = play (4800, false, 0.0);
    CHECK (std::all_of (quiet.begin(), quiet.end(), [] (float v) { return v == 0.0f; }));
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    hit = play (4800, false, 0.0);
    CHECK (std::any_of (hit.begin(), hit.end(), [] (float v) { return std::fabs (v) > 1e-3f; }));
    // Playing: the page asks to be painted as the steps go by, and lights the step playing.
    a.setup ({ { "Pitch 2", 0.3 }, { "Pitch 4", -0.25 }, { "Pitch 6", 0.15 }, { "Velocity 3", 0.4 }, { "Quantize", a.valueOf ("Quantize", "Minor") },
               { "Length", 7 } });
    const int paintsBefore = paints;
    play (48000 + 6000, true, 0.0);
    CHECK (paints > paintsBefore + 4);
    frame.playing = 1;
    const int paintsAsked = paints;
    CHECK (paint());
    CHECK (paints > paintsAsked); // the page schedules its next pose
    shot ("steps");
    // VCO: Plus moves to the next group; K1-K3 follow the cursor; Push edits, the wheel changes 1-2 FM Amt, the
    // host hears it after the next block, the engine plays it.
    CHECK (in (MELODEE_SCREEN_STEP, 1));
    CHECK ((knobs() == std::vector<uint32_t> { a.id ("VCO Decay"), a.id ("1-2 FM Amt"), a.id ("VCO EG > FM 1-2 Amt") }));
    CHECK (in (MELODEE_SCREEN_WHEEL, 1));
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    const int flushesBefore = flushes, rescansBefore = rescans;
    CHECK (in (MELODEE_SCREEN_WHEEL, 16));
    CHECK (flushes > flushesBefore);
    CHECK (rescans > rescansBefore); // the host reads the value again at once, run or not
    CHECK (std::fabs (a.value ("1-2 FM Amt") - 16.0 / 128.0) < 1e-6);
    OutEvents told;
    play (128, true, 2.25, &told);
    CHECK (std::any_of (told.values.begin(), told.values.end(), [&] (auto& v) { return v.first == a.id ("1-2 FM Amt") && std::fabs (v.second - 0.125) < 1e-6; }));
    frame.pots[1] = 90, frame.pots_live[1] = 0; // K2 has yet to meet its value: the card shows where to move it
    shot ("vco-editing");
    frame.pots[1] = -1, frame.pots_live[1] = 1;
    CHECK (in (MELODEE_SCREEN_PUSH, 0)); // leave editing
    // VCF, then SEQ: Trigger is a button (no pot); Push on it plays the step the sequence is on.
    CHECK (in (MELODEE_SCREEN_STEP, 1) && in (MELODEE_SCREEN_STEP, 1));
    shot ("vcf");
    for (int i = 0; i < 4; ++i)
        in (MELODEE_SCREEN_STEP, 1);
    CHECK (! in (MELODEE_SCREEN_STEP, 1)); // the last group
    CHECK (in (MELODEE_SCREEN_WHEEL, 9));
    CHECK ((knobs() == std::vector<uint32_t> { MELODEE_SCREEN_NO_CONTROL, MELODEE_SCREEN_NO_CONTROL, MELODEE_SCREEN_NO_CONTROL })); // Trigger, Advance: buttons
    shot ("seq");
    a.setup ({ { "VCA Decay", 1.0 } });
    play (24000, false, 0.0);
    CHECK (in (MELODEE_SCREEN_PUSH, 0));
    hit = play (4800, false, 0.0);
    CHECK (std::any_of (hit.begin(), hit.end(), [] (float v) { return std::fabs (v) > 1e-3f; }));
    // Editing a choice: Quantize, a step a detent; Shift shows the controls.
    CHECK (in (MELODEE_SCREEN_WHEEL, -3));
    CHECK ((knobs()[0] == a.id ("Quantize")));
    in (MELODEE_SCREEN_PUSH, 0);
    in (MELODEE_SCREEN_WHEEL, -1); // Minor (set above) to Major
    CHECK (a.value ("Quantize") == a.valueOf ("Quantize", "Major"));
    in (MELODEE_SCREEN_PUSH, 0);
    frame.shift = 1;
    shot ("shift");
    frame.shift = 0;
}

// 0.6: the pads. With Run on (the sequencer's own switch, the transport playing or not) every MIDI note plays the step
// its key names (36-43 steps 1-8, 44-51 steps 1-8 again ...) as the sequencer plays that step, without moving the
// sequence; with Run off, Notes: Pitch (the default) plays the voice at the key's pitch, Notes: Steps the steps.
namespace
{
// A pitched voice to tell the steps by: VCO 1's triangle alone at 144 Hz through an open filter, a 10 s VCA decay, each
// step its own Pitch and Velocity (unquantized, steps 1-8 are 144, 203.6, 288, 101.8, 171.2, 242.2, 72, 342.4 Hz).
const double padPitch[8] = { 0.0, 0.1, 0.2, -0.1, 0.05, 0.15, -0.2, 0.25 };
const double padVelocity[8] = { 1.0, 0.6, 0.8, 0.7, 0.9, 0.5, 0.75, 0.65 };
double padHz (int s) { return 144.0 * std::exp2 (padPitch[s] * 5.0); }
using Values = std::vector<std::pair<std::string, double>>;
Values padPatch (Instance& a, const Values& extra = {})
{
    Values v { { "VCO 1 Level", 1.0 }, { "VCO 2 Level", 0.0 }, { "Noise Level", 0.0 }, { "VCO 1 Wave", 1.0 }, { "VCO 2 Wave", 1.0 },
               { "VCO 1 EG Amt", 0.0 }, { "VCO 2 EG Amt", 0.0 }, { "1-2 FM Amt", 0.0 }, { "VCO EG > FM 1-2 Amt", 0.0 }, { "Cutoff", 1.0 },
               { "Resonance", 0.0 }, { "VCF EG Amt", 0.0 }, { "VCA EG", a.valueOf ("VCA EG", "Fast") }, { "VCA Decay", 1.0 },
               { "Volume", 0.5 }, { "VCO 1 Freq", 0.5 }, { "VCO 2 Freq", 0.5 } };
    for (int s = 0; s < 8; ++s)
        v.push_back ({ "Pitch " + std::to_string (s + 1), padPitch[s] }), v.push_back ({ "Velocity " + std::to_string (s + 1), padVelocity[s] });
    v.insert (v.end(), extra.begin(), extra.end());
    return v;
}
// The routes a step's pitch and velocity take, Quantize, and Chance 0 (the sequence itself would sound no step).
Values padRoutes (Instance& a)
{
    return { { "Cutoff", 0.55 }, { "Pitch > Cutoff", 0.5 }, { "Vel > Cutoff", 0.4 }, { "VCF EG Amt", 0.2 }, { "VCF EG > VCO 2", 0.3 },
             { "VCO 2 Level", 0.5 }, { "Quantize", a.valueOf ("Quantize", "Minor") }, { "Chance", 0.0 } };
}
Values operator+ (Values a, const Values& b) { a.insert (a.end(), b.begin(), b.end()); return a; }

// Events at absolute frames, each sent in the block that holds its frame (in list order on one frame).
using Act = std::function<void (Instance&, Events&, uint32_t)>;
using Timeline = std::vector<std::pair<uint64_t, Act>>;
Act noteOn (int key, double velocity) { return [=] (Instance&, Events& e, uint32_t t) { e.note (t, CLAP_EVENT_NOTE_ON, (int16_t) key, velocity); }; }
Act noteOff (int key) { return [=] (Instance&, Events& e, uint32_t t) { e.note (t, CLAP_EVENT_NOTE_OFF, (int16_t) key, 0.0); }; }
Act value (const char* name, double v) { return [=] (Instance& a, Events& e, uint32_t t) { e.param (t, a.id (name), v); }; }
Act midi (uint8_t status, uint8_t a1, uint8_t a2) { return [=] (Instance&, Events& e, uint32_t t) { e.midi (t, status, a1, a2); }; }
// hdslaveOS's silence: CC 120 and 123 at 0 on every channel, and the chokes its CLAP instance makes of them.
Act storm (int key)
{
    return [=] (Instance&, Events& e, uint32_t t) {
        for (uint8_t ch = 0; ch < 16; ++ch)
            e.midi (t, (uint8_t) (0xb0 | ch), 120, 0), e.midi (t, (uint8_t) (0xb0 | ch), 123, 0);
        e.note (t, CLAP_EVENT_NOTE_CHOKE, (int16_t) key, 0.0);
        e.note (t, CLAP_EVENT_NOTE_OFF, (int16_t) key, 0.0);
    };
}
// The sequencer playing step `s` itself: Advance s times from step 1 (on frames 0, 1 ...), then Trigger at `frame`.
Timeline stepByPanel (int s, uint64_t frame)
{
    Timeline t;
    for (int k = 0; k < s; ++k)
        t.push_back ({ (uint64_t) k, value ("Advance", 1.0) });
    t.push_back ({ frame, value ("Trigger", 1.0) });
    return t;
}
std::vector<float> run (Instance& a, uint32_t frames, bool playing, const Timeline& events, uint32_t block = 128, double bpm = 120.0)
{
    std::vector<float> l, r;
    a.render (frames, block, l, r, [=] (uint64_t at) { return playing ? playingFrom (0.0, bpm, 48000.0, at) : Transport { false, bpm, 0.0 }; },
              [&] (uint64_t at, uint32_t n, Events& e) {
                  for (auto& [when, act] : events)
                      if (when >= at && when < at + n)
                          act (a, e, (uint32_t) (when - at));
              });
    CHECK (l == r);
    return l;
}
// The frequency heard in [from, from + length) (0.4 s unless told).
double hzAt (const std::vector<float>& l, size_t from, size_t length = 19200) { return crossings (l, from, from + length) / (length / 48000.0); }
bool near (double hz, double want) { return std::fabs (hz - want) <= std::max (4.0, want * 0.02); }
float loudest (const std::vector<float>& x) { float m = 0.0f; for (float v : x) m = std::max (m, std::fabs (v)); return m; }
} // namespace

void testPads()
{
    // The choice's names: Notes is Pitch (the default) or Steps; its old name Trigger still reads as Pitch.
    {
        Instance a;
        char text[64] = {};
        CHECK (a.params->value_to_text (a.plugin, a.id ("Notes"), 0.0, text, sizeof (text)) && std::string (text) == "Pitch");
        CHECK (a.params->value_to_text (a.plugin, a.id ("Notes"), 1.0, text, sizeof (text)) && std::string (text) == "Steps");
        CHECK (a.value ("Notes") == 0.0 && a.value ("Run") == 1.0);
        double v = -1.0;
        CHECK (a.params->text_to_value (a.plugin, a.id ("Notes"), "Trigger", &v) && v == 0.0);
        CHECK (a.valueOf ("Notes", "Pitch") == 0.0 && a.valueOf ("Notes", "Steps") == 1.0);
    }
    // Run on, the transport stopped: each of notes 36 to 51 (pads 1 to 16) plays the step it names (pads 9-16 are
    // steps 1-8 again), as the panel's Trigger plays it on that step: to the bit, its quantized pitch, its velocity,
    // the routes, though Chance 0 would never sound it. Notes: Pitch or Steps, the same. A softer pad, softer.
    for (double notes : { 0.0, 1.0 })
        for (int key = 36; key <= 51; ++key)
        {
            const int s = (key - 36) % 8;
            Instance pad, panel;
            pad.setup (padPatch (pad, padRoutes (pad) + Values { { "Notes", notes } }));
            panel.setup (padPatch (panel, padRoutes (panel) + Values { { "Notes", notes } }));
            const auto got = run (pad, 24000, false, { { 1280, noteOn (key, 1.0) } });
            const auto want = run (panel, 24000, false, stepByPanel (s, 1280));
            CHECK (loudest (want) > 0.05f && got == want);
        }
    {
        Instance pad, panel;
        pad.setup (padPatch (pad, padRoutes (pad)));
        panel.setup (padPatch (panel, padRoutes (panel) + Values { { "Velocity 6", padVelocity[5] * 0.5 } }));
        CHECK (run (pad, 24000, false, { { 1280, noteOn (41, 0.5) } }) == run (panel, 24000, false, stepByPanel (5, 1280)));
    }
    // The pitch heard, Quantize Off: each pad its step's. The sequence did not move: Trigger then plays step 1, at its
    // own pitch (a previewed pitch holds only till a step replaces it), and Advance step 2.
    {
        Instance a;
        a.setup (padPatch (a));
        const auto l = run (a, 48000 * 6, false, { { 0, noteOn (39, 1.0) }, { 24000, noteOn (42, 1.0) }, { 48000, noteOn (50, 1.0) },
                                                    { 72000, noteOn (43, 0.8) }, { 96000, value ("Trigger", 1.0) }, { 120000, noteOn (37, 1.0) },
                                                    { 144000, value ("Advance", 1.0) }, { 168000, value ("Trigger", 1.0) } });
        const int expect[] = { 3, 6, 6, 7, 0, 1, -1, 1 };
        for (int i = 0; i < 8; ++i)
            if (expect[i] >= 0)
            {
                const double hz = hzAt (l, (size_t) i * 24000 + 1000);
                if (std::getenv ("ANVIL_DEBUG")) std::printf ("debug pad %d: %.1f Hz (step %d: %.1f)\n", i, hz, expect[i] + 1, padHz (expect[i]));
                CHECK (near (hz, padHz (expect[i])));
            }
    }
    // While the host plays (1/4 steps: 24,000 frames): a pad between two steps plays its step at once; the sequence goes
    // on as it would have (step 2 next, then 3), each at its own pitch.
    {
        Instance a, b;
        a.setup (padPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/4") } }));
        b.setup (padPatch (b, { { "Clock Div", b.valueOf ("Clock Div", "1/4") } }));
        const auto with = run (a, 72000, true, { { 5888, noteOn (39, 1.0) } }); // pad 4, in step 1 (a block's first frame)
        const auto without = run (b, 72000, true, {});
        CHECK (near (hzAt (without, 7000, 14400), padHz (0)) && near (hzAt (with, 7000, 14400), padHz (3)));
        CHECK (near (hzAt (with, 25000), padHz (1)) && near (hzAt (with, 49000), padHz (2)));
        CHECK (near (hzAt (with, 25000), hzAt (without, 25000)) && near (hzAt (with, 49000), hzAt (without, 49000)));
    }
    // A pad on Play's frame (all of a block's MIDI comes on its first frame): the louder of the pad's step and step 1
    // plays, whole; on a tie the later, the sequence's. Softer or tied, it is as if no pad was hit, to the bit; louder,
    // its step sounds on the downbeat and the sequence goes on with step 2.
    for (double fourth : { 0.3, 1.0 })
    {
        Instance a, b;
        a.setup (padPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/4") }, { "Velocity 4", fourth } }));
        b.setup (padPatch (b, { { "Clock Div", b.valueOf ("Clock Div", "1/4") }, { "Velocity 4", fourth } }));
        CHECK (run (a, 48000, true, { { 0, noteOn (39, 1.0) } }) == run (b, 48000, true, {}));
    }
    {
        Instance a;
        a.setup (padPatch (a, { { "Clock Div", a.valueOf ("Clock Div", "1/4") }, { "Velocity 1", 0.5 }, { "Velocity 4", 1.0 } }));
        const auto l = run (a, 72000, true, { { 0, noteOn (39, 1.0) } });
        CHECK (near (hzAt (l, 1000), padHz (3)) && near (hzAt (l, 25000), padHz (1)) && near (hzAt (l, 49000), padHz (2)));
    }
    // Two pads on one frame: the louder step plays, whole, whichever the host sends first; on a tie the later.
    for (bool tie : { false, true })
        for (bool fiveFirst : { false, true })
        {
            Instance pad, panel;
            const Values v { { "Velocity 2", tie ? 0.8 : padVelocity[1] }, { "Velocity 5", tie ? 0.8 : padVelocity[4] } };
            pad.setup (padPatch (pad, padRoutes (pad) + v));
            panel.setup (padPatch (panel, padRoutes (panel) + v));
            const int later = fiveFirst ? 1 : 4, wins = tie ? later : 4;
            const auto got = run (pad, 24000, false, { { 1280, noteOn (fiveFirst ? 40 : 37, 1.0) }, { 1280, noteOn (fiveFirst ? 37 : 40, 1.0) } });
            CHECK (got == run (panel, 24000, false, stepByPanel (wins, 1280)));
        }
    // A re-pressed pad, as hdslaveOS sends it: on, on, off. On one frame: one hit. Pressed again later: it plays again;
    // the note-off changes nothing.
    {
        Instance pad, panel;
        pad.setup (padPatch (pad));
        panel.setup (padPatch (panel));
        CHECK (run (pad, 24000, false, { { 1280, noteOn (38, 1.0) }, { 1280, noteOn (38, 1.0) }, { 1280, noteOff (38) } })
               == run (panel, 24000, false, stepByPanel (2, 1280)));
        Instance a, b;
        a.setup (clickPatch (a));
        b.setup (clickPatch (b));
        const auto offs = run (a, 48000, false, { { 1280, noteOn (38, 1.0) }, { 12800, noteOn (38, 1.0) }, { 25600, noteOff (38) } });
        const auto none = run (b, 48000, false, { { 1280, noteOn (38, 1.0) }, { 12800, noteOn (38, 1.0) } });
        const auto hits = onsets (offs);
        CHECK (offs == none && hits.size() == 2 && hits[0] >= 1280 && hits[0] < 1280 + 48 && hits[1] >= 12800 && hits[1] < 12800 + 48);
    }
    // MIDI's own note-on (a host with the MIDI dialect) plays the same; velocity 0 is a note-off, and a note-off stops
    // nothing (a hit rings out). A storm with nothing sounding plays nothing and moves no value.
    {
        Instance a, b, c;
        a.setup (padPatch (a));
        b.setup (padPatch (b));
        c.setup (padPatch (c));
        const auto clapNote = run (a, 48000, false, { { 1280, noteOn (38, 1.0) } });
        const auto midiNote = run (b, 48000, false, { { 1280, midi (0x90, 38, 127) }, { 12800, midi (0x90, 40, 0) },
                                                      { 25600, noteOff (38) } });
        CHECK (midiNote == clapNote);
        double values = 0.0;
        for (auto& [name, id] : c.ids)
            values += c.value (name);
        const auto quiet = run (c, 12800, false, { { 0, storm (38) }, { 128, midi (0x90, 38, 0) } });
        double after = 0.0;
        for (auto& [name, id] : c.ids)
            after += c.value (name);
        CHECK (loudest (quiet) == 0.0f && after == values);
    }
    // Run off, Notes: Pitch (the default): chromatic. Pad 1's key (36) plays the Freq knobs as they are; each key a
    // semitone, not snapped by Quantize (Major here: 37 would snap to 36 or 38).
    for (auto [key, hz] : { std::pair<int, double> { 36, 144.0 }, { 48, 288.0 }, { 60, 576.0 }, { 37, 152.6 }, { 72, 1152.0 }, { 24, 72.0 } })
    {
        Instance a;
        a.setup (padPatch (a, { { "Run", 0.0 }, { "Quantize", a.valueOf ( "Quantize", "Major") } }));
        const double got = hzAt (run (a, 24000, false, { { 0, noteOn (key, 1.0) } }), 2000);
        if (std::getenv ("ANVIL_DEBUG")) std::printf ("debug key %d: %.1f Hz (want %.1f)\n", key, got, hz);
        CHECK (near (got, hz));
    }
    // ...and the pitch stays within a step's (-5 octaves: key 96 is as far up as a Pitch knob goes; higher keys play
    // as 96, to the bit).
    {
        Instance a, b, c;
        a.setup (padPatch (a, { { "Run", 0.0 } }));
        b.setup (padPatch (b, { { "Run", 0.0 } }));
        c.setup (padPatch (c, { { "Run", 0.0 } }));
        CHECK (near (hzAt (run (a, 24000, false, { { 0, noteOn (96, 1.0) } }), 2000), 4608.0));
        CHECK (run (b, 24000, false, { { 0, noteOn (110, 1.0) } }) == run (c, 24000, false, { { 0, noteOn (96, 1.0) } }));
    }
    // ...the key as a step of its own: the same, to the bit, as Trigger on a step whose Pitch is the key's distance
    // from pad 1's key (36) and whose Velocity is the note's (Quantize Off there), through every route.
    for (auto [key, velocity] : { std::pair<int, double> { 72, 1.0 }, { 48, 0.5 }, { 37, 0.8 } })
    {
        Instance pad, panel;
        const Values routes { { "Run", 0.0 }, { "Seq Pitch Mod", pad.valueOf ("Seq Pitch Mod", "VCO 2") }, { "VCO 1 Level", 0.5 } };
        pad.setup (padPatch (pad, padRoutes (pad) + routes + Values { { "Quantize", pad.valueOf ("Quantize", "Major") } }));
        panel.setup (padPatch (panel, padRoutes (panel) + routes
                                     + Values { { "Quantize", 0.0 }, { "Pitch 1", (key - 36) / 60.0 }, { "Velocity 1", velocity } }));
        const auto got = run (pad, 24000, false, { { 1280, noteOn (key, velocity) } });
        CHECK (loudest (got) > 0.05f && got == run (panel, 24000, false, stepByPanel (0, 1280)));
    }
    // ...and the sequence stays where it is: Trigger plays step 1, at its pitch; Advance while a key rings moves the
    // pitch to step 2's (quantized again: Major makes its 6 semitones 7).
    {
        Instance a;
        a.setup (padPatch (a, { { "Run", 0.0 } }));
        const auto l = run (a, 72000, false, { { 0, noteOn (72, 1.0) }, { 24000, value ("Trigger", 1.0) } });
        CHECK (near (hzAt (l, 2000), 1152.0) && near (hzAt (l, 26000), padHz (0)));
        Instance b;
        b.setup (padPatch (b, { { "Run", 0.0 }, { "Quantize", b.valueOf ("Quantize", "Major") } }));
        const auto m = run (b, 48000, false, { { 0, noteOn (48, 1.0) }, { 24000, value ("Advance", 1.0) } });
        CHECK (near (hzAt (m, 2000), 288.0) && near (hzAt (m, 26000), 144.0 * std::exp2 (7.0 / 12.0)));
    }
    // Run off, Notes: Steps: the pads play the steps, as before 0.6 (Pad Kit's way: its eight drums on the pads alone).
    {
        Instance pad, panel;
        pad.setup (padPatch (pad, { { "Run", 0.0 }, { "Notes", 1.0 } }));
        panel.setup (padPatch (panel, { { "Run", 0.0 }, { "Notes", 1.0 } }));
        CHECK (run (pad, 24000, false, { { 1280, noteOn (46, 1.0) } }) == run (panel, 24000, false, stepByPanel (2, 1280)));
        Instance kit, kitPanel;
        CHECK (kit.loadPreset ("pad-kit") && kitPanel.loadPreset ("pad-kit"));
        kit.setup ({ { "Run", 0.0 } });
        kitPanel.setup ({ { "Run", 0.0 } });
        const auto drum = run (kit, 24000, false, { { 1280, noteOn (41, 1.0) } });
        CHECK (loudest (drum) > 0.05f && drum == run (kitPanel, 24000, false, stepByPanel (5, 1280)));
    }
    // Two keys on one frame with Run off: the louder plays, whole; on a tie the later.
    for (bool tie : { false, true })
        for (bool highFirst : { false, true })
        {
            Instance pad, alone;
            pad.setup (padPatch (pad, { { "Run", 0.0 } }));
            alone.setup (padPatch (alone, { { "Run", 0.0 } }));
            const double soft = tie ? 1.0 : 0.5;
            const auto got = run (pad, 24000, false, { { 1280, highFirst ? noteOn (72, 1.0) : noteOn (48, soft) },
                                                       { 1280, highFirst ? noteOn (48, soft) : noteOn (72, 1.0) } });
            const int wins = tie ? (highFirst ? 48 : 72) : 72;
            CHECK (got == run (alone, 24000, false, { { 1280, noteOn (wins, wins == 72 ? 1.0 : soft) } }));
        }
    // The page's Push plays the step picked, whatever Run and Notes are (it is no note).
    {
        Instance a, panel;
        a.setup (padPatch (a, { { "Run", 0.0 } }));
        panel.setup (padPatch (panel, { { "Run", 0.0 } }));
        const auto* screen = static_cast<const melodee_screen_t*> (a.plugin->get_extension (a.plugin, MELODEE_SCREEN_CLAP));
        CHECK (screen != nullptr);
        if (screen != nullptr)
        {
            melodee_screen_input_t wheel { MELODEE_SCREEN_WHEEL, 2, 0 }, push { MELODEE_SCREEN_PUSH, 0, 0 };
            CHECK (screen->input (a.plugin, &wheel) && screen->input (a.plugin, &push));
            const auto got = run (a, 24000, false, {});
            CHECK (loudest (got) > 0.05f && got == run (panel, 24000, false, { { 0, value ("Advance", 1.0) }, { 0, value ("Advance", 1.0) }, { 0, value ("Trigger", 1.0) } }));
        }
    }
    // The same output in blocks of 128, 37, 1 and 4096 frames, to the bit: pads between and on steps, two on one frame,
    // on/on/off, MIDI pads, a silence storm, Run switched off (keys), Notes: Steps, Run on again.
    {
        const Timeline busy { { 3000, noteOn (38, 0.9) }, { 13000, noteOn (37, 1.0) }, { 13000, noteOn (44, 0.7) }, { 20000, noteOn (40, 1.0) },
                              { 20000, noteOn (40, 1.0) }, { 24000, value ("Cutoff", 0.5) }, { 24000, noteOn (39, 1.0) }, { 26000, noteOff (40) }, { 30001, midi (0x90, 41, 100) }, { 36000, storm (41) },
                              { 42000, noteOn (51, 1.0) }, { 48000, value ("Run", 0.0) }, { 48000, noteOn (72, 1.0) }, { 50017, noteOn (49, 0.4) },
                              { 50017, noteOn (61, 0.8) }, { 60000, value ("Notes", 1.0) }, { 60005, noteOn (45, 1.0) }, { 70000, value ("Run", 1.0) },
                              { 70000, noteOn (36, 1.0) }, { 84001, noteOn (47, 0.6) } };
        std::vector<float> first;
        for (uint32_t block : { 128u, 37u, 1u, 4096u })
        {
            Instance a;
            a.setup (padPatch (a, padRoutes (a) + Values { { "Chance", 0.6 }, { "Ratchet", 2.0 }, { "VCA Decay", 0.4 } }));
            const auto l = run (a, 96000, true, busy, block);
            if (block == 128)
                first = l;
            else
                CHECK (l == first);
        }
        CHECK (loudest (first) > 0.05f);
    }
}

// 0.6.1: a choke, and MIDI's All Sound Off (CC 120 at 0), stop the hit that sounds: a choke of the key that played it,
// or (key -1, and All Sound Off) whatever sounds, on any channel; the hit fades out in 1.5 ms, no click, and the
// Ratchet hits still to come in its step do not play. A note-off, Reset All Controllers (121) and All Notes Off (123)
// stop nothing (no key is held and there is no pedal), and no controller is a parameter. A sequencer hit, Trigger's and
// the page's, has no key: a key's choke leaves it, key -1 and All Sound Off stop it. The sequence's place and Run do
// not move: the next step plays as ever, and a hit after a choke on the same frame plays (it is no second step there).
namespace
{
Act chokeKey (int key) { return [=] (Instance&, Events& e, uint32_t t) { e.note (t, CLAP_EVENT_NOTE_CHOKE, (int16_t) key, 0.0); }; }
float peakIn (const std::vector<float>& x, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = from; i < std::min (to, x.size()); ++i) m = std::max (m, std::fabs (x[i]));
    return m;
}
bool sameTo (const std::vector<float>& a, const std::vector<float>& b, size_t n)
{
    return a.size() >= n && b.size() >= n && std::equal (a.begin(), a.begin() + (long) n, b.begin());
}
} // namespace

void testChokes()
{
    const uint64_t hit = 1280, cut = 12800, done = cut + 480; // 10 ms after the choke: the 1.5 ms fade is over
    // A key's hit: the choke of that key stops it (silence after the fade), and the sound up to the choke is the same
    // to the bit as without it. A choke of another key, a note-off, Reset All Controllers and All Notes Off stop nothing.
    {
        Instance a, b;
        a.setup (padPatch (a));
        b.setup (padPatch (b));
        const auto plain = run (a, 48000, false, { { hit, noteOn (38, 1.0) } });
        const auto choked = run (b, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) } });
        CHECK (peakIn (plain, done, plain.size()) > 0.01f && peakIn (choked, done, choked.size()) < 1e-4f);
        CHECK (sameTo (choked, plain, cut));
        for (auto act : { chokeKey (40), noteOff (38), midi (0xb0, 121, 0), midi (0xb0, 123, 0), midi (0xb0, 120, 64) })
        {
            Instance c, d;
            c.setup (padPatch (c));
            d.setup (padPatch (d));
            CHECK (run (c, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, act } }) == run (d, 48000, false, { { hit, noteOn (38, 1.0) } }));
        }
    }
    // All Sound Off, on any channel, stops it as a choke of its key does; and key -1, a host's choke of everything.
    {
        Instance a, b;
        a.setup (padPatch (a));
        b.setup (padPatch (b));
        const auto midiOff = run (a, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, midi (0xb5, 120, 0) } });
        const auto all = run (b, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (-1) } });
        CHECK (midiOff == all && peakIn (midiOff, done, midiOff.size()) < 1e-4f);
    }
    // A sequencer's hit has no key: a key's choke leaves it (only the keys' hits are held to the silence, as the host
    // makes one choke for each key played); key -1 and All Sound Off stop it.
    {
        Instance a, b;
        a.setup (padPatch (a));
        b.setup (padPatch (b));
        const auto plain = run (a, 48000, false, { { hit, value ("Trigger", 1.0) } });
        CHECK (run (b, 48000, false, { { hit, value ("Trigger", 1.0) }, { cut, chokeKey (38) } }) == plain);
        Instance c, d;
        c.setup (padPatch (c));
        d.setup (padPatch (d));
        CHECK (peakIn (run (c, 48000, false, { { hit, value ("Trigger", 1.0) }, { cut, chokeKey (-1) } }), done, 48000) < 1e-4f);
        CHECK (peakIn (run (d, 48000, false, { { hit, value ("Trigger", 1.0) }, { cut, storm (38) } }), done, 48000) < 1e-4f);
    }
    // The Ratchet hits still to come in the step do not play: three a step (the click patch, quarters at 120 BPM: a
    // hit every 4,000 frames), the first step's cut by a choke after its first hit leaves one where it would have three,
    // and the steps after it play as ever.
    {
        Instance a, b;
        for (Instance* x : { &a, &b })
        {
            auto p = clickPatch (*x);
            p.push_back ({ "Ratchet", 3.0 });
            p.push_back ({ "VCA Decay", 0.0 });
            x->setup (p);
        }
        const auto all = onsets (run (a, 24000, true, {}), 1e-3f, 1e-4f, 96);
        const auto one = onsets (run (b, 24000, true, { { 1000, chokeKey (-1) } }), 1e-3f, 1e-4f, 96);
        if (std::getenv ("ANVIL_DEBUG")) std::printf ("debug ratchet: %zu onsets plain, %zu choked\n", all.size(), one.size());
        CHECK (all.size() == 6 && one.size() == 4 && one[0] == all[0]);
        bool spaced = one.size() == 4;
        for (size_t i = 1; spaced && i < one.size(); ++i) // the first step's three are now one (frames 0, 2000, 4000)
            spaced = one[i] == all[i + 2];
        CHECK (spaced);
    }
    // A choke and a hit on one frame: in event order. The choke first, the hit plays; the hit first, the choke fades
    // what it just started (and the hit after it on that frame plays).
    {
        Instance a, b, c;
        a.setup (padPatch (a));
        b.setup (padPatch (b));
        c.setup (padPatch (c));
        CHECK (run (a, 24000, false, { { hit, chokeKey (38) }, { hit, noteOn (38, 1.0) } })
               == run (c, 24000, false, { { hit, noteOn (38, 1.0) } }));
        CHECK (peakIn (run (b, 24000, false, { { hit, noteOn (38, 1.0) }, { hit, chokeKey (38) } }), hit + 480, 24000) < 1e-4f);
    }
    // A hit while the fade runs: it plays once the fade is out (a new hit, not a second step); a choke of its key
    // before then stops it before it sounds.
    {
        Instance a, b, c;
        a.setup (padPatch (a)), b.setup (padPatch (b)), c.setup (padPatch (c));
        const auto again = run (a, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) }, { cut + 16, noteOn (40, 1.0) } });
        CHECK (peakIn (again, done, again.size()) > 0.01f);
        CHECK (peakIn (run (b, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) } }), done, 48000) < 1e-4f);
        const auto stopped = run (c, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) }, { cut + 16, noteOn (40, 1.0) },
                                                    { cut + 40, chokeKey (40) } });
        CHECK (peakIn (stopped, cut + 80, stopped.size()) == 0.0f);
    }
    // When the fade is out, nothing of the choked hit is left: to 0.6.6 the 47-tap decimator, before the fade, still
    // held about a dozen frames of it, which then played at full gain (here 0.02 on the pad, 0.15 on noise, 72 to 84
    // frames after the choke). A choke and All Sound Off, a tone and noise.
    {
        const Values noise { { "VCO 1 Level", 0.0 }, { "Noise Level", 1.0 }, { "Volume", 0.8 } };
        for (int patch = 0; patch < 2; ++patch)
            for (int how = 0; how < 2; ++how)
            {
                Instance a;
                a.setup (padPatch (a, patch == 1 ? noise : Values {}));
                const auto out = run (a, 24000, false, { { hit, noteOn (38, 1.0) }, { cut, how == 0 ? chokeKey (38) : midi (0xb0, 120, 0) } });
                CHECK (peakIn (out, cut - 200, cut) > 0.05f);
                if (peakIn (out, cut + 74, cut + 480) >= 1e-4f)
                    std::printf ("  %s, %s: %.4f after the fade\n", patch == 1 ? "noise" : "the pad", how == 0 ? "a choke" : "CC 120",
                                 peakIn (out, cut + 74, cut + 480));
                CHECK (peakIn (out, cut + 74, cut + 480) < 1e-4f);
            }
    }
    // A hit inside a choke's fade waits for the fade's end (1.5 ms at most) and plays whole, from silence, so the gain
    // never steps. To 0.6.5 the fade went on over the new hit and its end set the gain from 0 back to 1 in one sample
    // (a step of 0.10 to 0.18 here, where the hit alone moves at most 0.014 a sample). At 0 frames the choke and the
    // note share a frame, as hdslaveOS sends a block's events.
    {
        Instance x, y;
        x.setup (padPatch (x)), y.setup (padPatch (y));
        auto largestStep = [] (const std::vector<float>& v, size_t from, size_t to) {
            float m = 0.0f;
            for (size_t i = std::max<size_t> (from, 1); i < std::min (to, v.size()); ++i) m = std::max (m, std::fabs (v[i] - v[i - 1]));
            return m;
        };
        const float ringing = largestStep (run (x, 48000, false, { { hit, noteOn (38, 1.0) } }), cut, cut + 400);
        const float alone = largestStep (run (y, 48000, false, { { cut, noteOn (40, 1.0) } }), cut, cut + 400);
        for (uint64_t gap : { 0, 16, 40, 71 })
        {
            Instance a;
            a.setup (padPatch (a));
            const auto out = run (a, 48000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) }, { cut + gap, noteOn (40, 1.0) } });
            const float worst = largestStep (out, cut, cut + 400);
            if (worst >= 2.0f * std::max (ringing, alone))
                std::printf ("  a hit %d frames into a choke's fade: the output steps by %.4f (the ringing hit %.4f, the hit alone %.4f)\n", (int) gap,
                             worst, ringing, alone);
            CHECK (worst < 2.0f * std::max (ringing, alone));
            CHECK (peakIn (out, done, out.size()) > 0.01f);
        }
    }
    // A softer hit than the tail it lands on, with the choke on its frame or inside the fade: the choke still ends the
    // choked hit, and 10 to 20 ms on the level is the soft hit's, as when it is struck alone 72 frames later. 0.6.6
    // turned the fade back up and the soft hit's envelopes kept the louder tail falling: the choked hit rang on.
    {
        const auto rmsIn = [] (const std::vector<float>& v, size_t from, size_t to) {
            double e = 0.0;
            for (size_t i = from; i < to; ++i) e += (double) v[i] * v[i];
            return std::sqrt (e / (double) (to - from));
        };
        Instance x, y;
        x.setup (padPatch (x)), y.setup (padPatch (y));
        const auto loud = run (x, 24000, false, { { hit, noteOn (38, 1.0) } });
        const auto soft = run (y, 24000, false, { { cut + 72, noteOn (40, 0.2) } });
        const double ringing = rmsIn (loud, cut + 480, cut + 960), alone = rmsIn (soft, cut + 480, cut + 960);
        CHECK (alone > 1e-3 && ringing > 3.0 * alone);
        for (uint64_t gap : { 0, 16, 60 })
        {
            Instance a;
            a.setup (padPatch (a));
            const auto out = run (a, 24000, false, { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) }, { cut + gap, noteOn (40, 0.2) } });
            const double level = rmsIn (out, cut + 480, cut + 960);
            if (std::fabs (level - alone) >= 0.1 * alone)
                std::printf ("  a soft hit %d frames into a choke's fade: %.4f where it alone is %.4f (the choked hit %.4f)\n", (int) gap, level,
                             alone, ringing);
            CHECK (std::fabs (level - alone) < 0.1 * alone);
        }
    }
    // A choke when nothing is heard (the VCA closed, the VCO's envelope still running) is done at once: a hit just
    // after it plays whole, as one 200 frames later. 0.6.6 faded over the silence and a hit inside it faded up.
    {
        const Values longVco { { "VCA Decay", 0.0 }, { "VCO Decay", 1.0 }, { "VCO 1 EG Amt", 0.6 } };
        Instance a, b;
        a.setup (padPatch (a, longVco)), b.setup (padPatch (b, longVco));
        const auto soon = run (a, 36000, false, { { 100, noteOn (38, 1.0) }, { 20000, chokeKey (38) }, { 20016, noteOn (40, 1.0) } });
        const auto later = run (b, 36000, false, { { 100, noteOn (38, 1.0) }, { 20000, chokeKey (38) }, { 20216, noteOn (40, 1.0) } });
        CHECK (peakIn (soon, 19000, 20000) == 0.0f && peakIn (soon, 20016, 24000) > 0.01f);
        const bool same = std::equal (soon.begin() + 20016, soon.begin() + 30000, later.begin() + 20216);
        if (! same) std::printf ("  a hit just after a choke of a silent hit is not whole\n");
        CHECK (same);
    }
    // A choke or All Sound Off that reaches Anvil while nothing sounds (hdslaveOS silences every plug-in this way) leaves
    // the next hit as it would be. To 0.6.5 the fade waited, and the next hit, however much later, was faded down for
    // 1.5 ms and then stepped back up.
    {
        const Values shortHit { { "VCA Decay", 0.0 } };
        for (int how = 0; how < 3; ++how)
        {
            Instance a, b;
            a.setup (padPatch (a, shortHit)), b.setup (padPatch (b, shortHit));
            Timeline with { { 24000, noteOn (40, 1.0) } }, without = with;
            if (how == 0) with.push_back ({ 100, midi (0xb0, 120, 0) });
            if (how == 1) with.push_back ({ 100, chokeKey (-1) });
            if (how == 2) // a hit rung out, then All Sound Off
            {
                with.push_back ({ 100, noteOn (38, 1.0) }), without.push_back ({ 100, noteOn (38, 1.0) });
                with.push_back ({ 20000, midi (0xb0, 120, 0) });
            }
            const auto out = run (a, 48000, false, with), ref = run (b, 48000, false, without);
            CHECK (peakIn (ref, 23000, 24000) == 0.0f && peakIn (ref, 24000, 30000) > 0.01f);
            if (out != ref) std::printf ("  case %d: the hit after a silent choke is not the hit as it would be\n", how);
            CHECK (out == ref);
        }
    }
    // The same with a knob turned on the choke's frame and the hit 16 frames later, inside the decimator's length (as
    // the sequencer's step lands in the block after hdslaveOS's silence): nothing sounds, so the knob is taken at once,
    // as without the choke. To 0.6.7's first build the choke emptied the decimator, which then counted as sounding for
    // 25 frames, and the knob eased in under the hit instead (Volume off by up to 0.10).
    {
        const Values shortHit { { "VCA Decay", 0.0 } };
        for (const char* knob : { "Volume", "Resonance", "VCO 2 Level" })
            for (bool rungOut : { false, true }) // never hit, or a hit rung out before
            {
                Instance a, b;
                a.setup (padPatch (a, shortHit)), b.setup (padPatch (b, shortHit));
                Timeline with, without;
                if (rungOut) with.push_back ({ 100, noteOn (38, 1.0) }), without.push_back ({ 100, noteOn (38, 1.0) });
                with.push_back ({ 24000, midi (0xb0, 120, 0) });
                for (Timeline* t : { &with, &without })
                    t->push_back ({ 24000, value (knob, 0.9) }), t->push_back ({ 24016, noteOn (40, 1.0) });
                const auto out = run (a, 48000, false, with), ref = run (b, 48000, false, without);
                CHECK (peakIn (ref, 23000, 24000) == 0.0f && peakIn (ref, 24016, 30000) > 0.01f);
                if (out != ref)
                {
                    double worst = 0.0;
                    for (size_t i = 0; i < out.size(); ++i) worst = std::max (worst, (double) std::fabs (out[i] - ref[i]));
                    std::printf ("  %s turned on a silent choke's frame%s: the hit after it off by up to %.4f\n", knob,
                                 rungOut ? " (a hit rung out)" : "", worst);
                }
                CHECK (out == ref);
            }
    }
    // A hit the sequencer itself played (the host playing, no key ever pressed): All Sound Off and a choke of key -1
    // stop it and the next step plays as ever; a named key's choke leaves it ringing. (The pad patch's 10 s decay
    // rings through the step at frame 12000, so the windows sit inside one step.)
    {
        Instance a, b, c;
        a.setup (padPatch (a));
        b.setup (padPatch (b));
        c.setup (padPatch (c));
        const auto stormed = run (a, 24000, true, { { 6000, storm (38) } });
        CHECK (peakIn (stormed, 6480, 11900) < 1e-4f && peakIn (stormed, 12100, 23900) > 0.01f);
        const auto all = run (b, 24000, true, { { 6000, chokeKey (-1) } });
        CHECK (peakIn (all, 6480, 11900) < 1e-4f);
        const auto left = run (c, 24000, true, { { 6000, chokeKey (38) } });
        CHECK (peakIn (left, 6480, 11900) > 0.01f);
    }
    // The same output in blocks of 128, 37, 1 and 4096 frames with chokes, All Sound Off and a hit in a fade.
    {
        const Timeline busy { { hit, noteOn (38, 1.0) }, { cut, chokeKey (38) }, { cut + 16, noteOn (40, 1.0) }, { 20000, midi (0xb3, 120, 0) },
                              { 24000, noteOn (39, 1.0) }, { 30001, value ("Trigger", 1.0) }, { 36000, chokeKey (-1) },
                              { 42000, noteOn (41, 1.0) }, { 42000, storm (41) }, { 48000, noteOn (44, 1.0) } };
        std::vector<float> first;
        for (uint32_t block : { 128u, 37u, 1u, 4096u })
        {
            Instance a;
            a.setup (padPatch (a, padRoutes (a)));
            const auto l = run (a, 72000, false, busy, block);
            if (block == 128)
                first = l;
            else
                CHECK (l == first);
        }
        CHECK (loudest (first) > 0.05f);
    }
}

// 0.6.2: what a hostile or careless host can send — a reset must leave a fresh engine, a state loaded over a ringing
// hit settles in one block, text and states that are not finite or not this version are refused, and no tempo (nor
// the absence of one) can hang the clock.
void testRobustness()
{
    std::vector<float> l, r, fresh;
    const auto stopped = [] (uint64_t) { return Transport { false, 120.0, 0.0 }; };
    // reset() after a loud hit on the very first frame: the next, softer hit on frame 0 is no "second hit of that
    // frame", and sounds exactly as a fresh instance's does.
    {
        Instance a, b;
        a.render (128, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) a.set (e, "Trigger", 1.0);
        });
        a.plugin->reset (a.plugin);
        const auto softer = [&] (Instance& x, uint64_t at, uint32_t, Events& e) {
            if (at == 0) x.set (e, "Velocity 1", 0.4), x.set (e, "Trigger", 1.0);
        };
        a.render (4800, 128, fresh, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) { softer (a, at, n, e); });
        b.render (4800, 128, l, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) { softer (b, at, n, e); });
        CHECK (l == fresh && peakIn (l, 0, l.size()) > 0.01f);
    }
    // A state loaded while a hit rings settles at the next block (no glide across it): Volume 0 there is silence
    // there, once the decimator's own dozen frames are out.
    {
        Instance a, quiet;
        quiet.setup ({ { "Volume", 0.0 } });
        std::vector<uint8_t> saved;
        clap_ostream os { &saved, [] (const clap_ostream* s, const void* d, uint64_t n) -> int64_t {
            auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
            v->insert (v->end(), (const uint8_t*) d, (const uint8_t*) d + n);
            return (int64_t) n;
        } };
        CHECK (quiet.state->save (quiet.plugin, &os));
        a.render (4800, 128, l, r, stopped, [&] (uint64_t at, uint32_t, Events& e) {
            if (at == 0) a.set (e, "Trigger", 1.0);
        });
        CHECK (peakIn (l, 0, l.size()) > 0.01f); // it rings
        struct Reader { const std::vector<uint8_t>* v; size_t at; } reader { &saved, 0 };
        clap_istream is { &reader, [] (const clap_istream* s, void* to, uint64_t n) -> int64_t {
            auto* rd = static_cast<Reader*> (s->ctx);
            const size_t k = std::min<size_t> ((size_t) n, rd->v->size() - rd->at);
            std::memcpy (to, rd->v->data() + rd->at, k);
            rd->at += k;
            return (int64_t) k;
        } };
        CHECK (a.state->load (a.plugin, &is));
        a.render (128, 128, l, r, stopped);
        CHECK (peakIn (l, 64, 128) == 0.0f);
        // A state from a newer version is refused outright.
        std::vector<uint8_t> future (saved.begin(), saved.begin() + 12);
        future[4] = 2; // version 2, no values
        future[8] = 0;
        Reader again { &future, 0 };
        clap_istream is2 { &again, is.read };
        CHECK (! a.state->load (a.plugin, &is2));
    }
    // Text that is not a finite number is refused, not clamped to the range's end.
    {
        Instance a;
        double v = 0.0;
        for (const char* bad : { "nan", "inf", "-inf", "1e999", "1e308", "-5 Hz" })
            CHECK (! a.params->text_to_value (a.plugin, a.id ("Cutoff"), bad, &v));
        CHECK (a.params->text_to_value (a.plugin, a.id ("Cutoff"), "1.00 kHz", &v));
    }
    // Tempos at and past sense: the clock clamps to 1..1000 BPM, a missing or non-finite one reads as 120, and every
    // block comes back (an infinite tempo once clocked forever without a frame passing).
    for (double bpm : { 0.0, 2000.0, 1.0e308, (double) INFINITY, (double) NAN })
    {
        Instance a;
        a.setup (clickPatch (a));
        a.render (4800, 128, l, r, [bpm] (uint64_t at) { return Transport { true, bpm, (double) at / 24000.0 }; });
        for (float s : l)
            CHECK (std::isfinite (s));
    }
    // An infinite tempo sounds exactly as 120 BPM does.
    {
        Instance a, b;
        a.setup (clickPatch (a));
        b.setup (clickPatch (b));
        a.render (48000, 128, fresh, r, [] (uint64_t at) { return Transport { true, INFINITY, (double) at / 24000.0 }; });
        b.render (48000, 128, l, r, [] (uint64_t at) { return playingFrom (0.0, 120.0, 48000.0, at); });
        CHECK (l == fresh && peakIn (l, 0, l.size()) > 0.01f);
    }
    // A sample rate that is no rate at all (0, NaN): activation holds it to 48 kHz rather than dividing by it —
    // a hit sounds exactly as it does activated at 48 kHz.
    {
        const auto trigger = [] (Instance& x, uint64_t at, uint32_t, Events& e) {
            if (at == 0) x.set (e, "Trigger", 1.0);
        };
        Instance ref;
        ref.render (4800, 128, fresh, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) { trigger (ref, at, n, e); });
        for (double badRate : { 0.0, (double) NAN })
        {
            Instance a (badRate);
            a.render (4800, 128, l, r, stopped, [&] (uint64_t at, uint32_t n, Events& e) { trigger (a, at, n, e); });
            CHECK (l == fresh && peakIn (l, 0, l.size()) > 0.01f);
        }
    }
}

// The six-encoder page (org.sixteen.screen/1): knob banks, tab presses, a step drag, and a picture.
void testSixteenScreen (const std::string& folder)
{
    Instance a;
    const auto* ext = static_cast<const sixteen_plugin_screen_t*> (a.plugin->get_extension (a.plugin, SIXTEEN_EXT_SCREEN));
    CHECK (ext != nullptr && ext->render != nullptr && ext->knobs != nullptr && ext->touch != nullptr
           && ext->selector_turn != nullptr && ext->selector_push != nullptr);
    if (ext == nullptr)
        return;
    uint32_t ids[6] = {};
    char tab[SIXTEEN_TAB_NAME_SIZE] = {};
    auto bank = [&] { CHECK (ext->knobs (a.plugin, ids, tab)); };
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - STEP 01") == 0);
    CHECK (ids[0] == a.id ("Pitch 1") && ids[1] == a.id ("Velocity 1") && ids[2] == a.id ("Cutoff"));
    CHECK (ids[3] == a.id ("Resonance") && ids[4] == a.id ("VCF Decay") && ids[5] == a.id ("VCA Decay"));
    CHECK (ext->selector_turn (a.plugin, 1));
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - STEP 02") == 0 && ids[0] == a.id ("Pitch 2") && ids[1] == a.id ("Velocity 2"));
    CHECK (ext->selector_turn (a.plugin, -1));

    std::vector<uint32_t> pixels ((size_t) SIXTEEN_SCREEN_WIDTH * SIXTEEN_SCREEN_HEIGHT);
    sixteen_screen_surface_t surf {};
    surf.pixels = pixels.data ();
    surf.width = SIXTEEN_SCREEN_WIDTH;
    surf.height = SIXTEEN_SCREEN_HEIGHT;
    surf.stride = SIXTEEN_SCREEN_WIDTH;
    surf.now_ms = 1000;
    auto shot = [&] (const char* name) {
        surf.now_ms += 40;
        CHECK (ext->render (a.plugin, &surf, true));
        CHECK (writePng ((folder + "/screen-sp16-" + name + ".png").c_str (), pixels.data (), SIXTEEN_SCREEN_WIDTH,
                         SIXTEEN_SCREEN_HEIGHT, SIXTEEN_SCREEN_WIDTH, 1));
    };
    shot ("steps");
    int varied = 0;
    for (int y = 0; y < 60; ++y)
        for (int x = 0; x < 320; ++x)
            varied += pixels[(size_t) y * SIXTEEN_SCREEN_WIDTH + (size_t) x] != pixels[0];
    CHECK (varied > 200);

    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 450, 40)); // VCO
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - VCO 1/2") == 0);
    CHECK (ids[0] == a.id ("VCO 1 Freq") && ids[2] == a.id ("VCO 1 Wave") && ids[3] == a.id ("VCO 2 Freq") && ids[5] == a.id ("VCO 2 Wave"));
    shot ("vco");
    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 450, 40)); // re-tap flips the bank
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - VCO 2/2") == 0 && ids[0] == a.id ("VCO Decay") && ids[4] == a.id ("Hard Sync") && ids[5] == CLAP_INVALID_ID);
    shot ("vco-fm");

    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 522, 40)); // MIX
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - MIX") == 0 && ids[0] == a.id ("VCO 1 Level") && ids[3] == a.id ("Noise Color"));
    CHECK (ids[4] == CLAP_INVALID_ID && ids[5] == CLAP_INVALID_ID);
    shot ("mix");

    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 594, 40)); // VCF
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - VCF") == 0 && ids[0] == a.id ("Cutoff") && ids[5] == a.id ("Noise/VCF Mod"));
    shot ("vcf");
    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 666, 40)); // VCA / PATCH
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - VCA/PATCH") == 0 && ids[0] == a.id ("VCA EG") && ids[2] == a.id ("Volume") && ids[5] == a.id ("VCF EG > VCO 2"));
    shot ("vca");
    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 738, 40)); // SEQ
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - SEQ 1/2") == 0 && ids[0] == a.id ("Clock Div") && ids[5] == a.id ("Ratchet"));
    shot ("seq");
    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 738, 40));
    bank ();
    CHECK (std::strcmp (tab, "ANVIL - SEQ 2/2") == 0 && ids[0] == a.id ("Quantize") && ids[2] == a.id ("Run") && ids[3] == CLAP_INVALID_ID);
    shot ("seq-notes");

    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 378, 40)); // back to STEPS
    const int before = processRequests;
    CHECK (ext->touch (a.plugin, SIXTEEN_TOUCH_DOWN, 111, 120)); // step 1 pitch, above the centre line
    double pitched = 0;
    CHECK (a.params->get_value (a.plugin, a.id ("Pitch 1"), &pitched) && pitched > 0.4);
    CHECK (ext->selector_push (a.plugin));
    CHECK (processRequests > before);
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: host Anvil.clap OUT_FOLDER\n");
        return 2;
    }
    void* library = dlopen (argv[1], RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr)
    {
        std::printf ("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    entry = static_cast<const clap_plugin_entry*> (dlsym (library, "clap_entry"));
    if (entry == nullptr || ! entry->init (argv[1]))
    {
        std::printf ("FAIL no clap_entry\n");
        return 1;
    }
    factory = static_cast<const clap_plugin_factory*> (entry->get_factory (CLAP_PLUGIN_FACTORY_ID));
    testDescription();
    testParameters();
    testState();
    testPresets();
    testTiming();
    testStopRestartJump();
    testLoopsAndClockChanges();
    testPositionJitter();
    testJumpEdges();
    testBlockSizes();
    testButtonsAndNotes();
    testFirstPlay();
    testLengthAndSwing();
    testNotesAndDirection();
    testChanceAndRatchet();
    testPatchRoutings();
    testVelocity();
    testSofterStepInAttack();
    testPitchAndFm();
    testQuantize();
    testScreen (argv[2]);
    testSixteenScreen (argv[2]);
    testPads();
    testChokes();
    testRobustness();
    testExtremes();
    demos (argv[2]);
    entry->deinit();
    std::printf ("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
