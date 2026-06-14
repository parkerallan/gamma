#include "assets/LipSyncBake.h"

#include <nlohmann/json.hpp>

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>

#if defined(_WIN32)
#define ENGINE_POPEN _popen
#define ENGINE_PCLOSE _pclose
#else
#define ENGINE_POPEN popen
#define ENGINE_PCLOSE pclose
#endif

using json = nlohmann::json;

std::vector<std::pair<std::string, float>> RhubarbShapeToArkitWeights(char shape)
{
    // Rhubarb's Preston-Blair-derived shapes. A-F are the basic set; G/H are the
    // extended set (F/V and L). X is the rest pose. These are deliberately
    // moderate weights -- a per-character mapping override comes later.
    switch (shape)
    {
    case 'A': // Closed: P, B, M.
        return {{"mouthClose", 1.0f}, {"mouthPressLeft", 0.2f}, {"mouthPressRight", 0.2f}};
    case 'B': // Slightly open, clenched teeth: many consonants, "ee".
        return {{"jawOpen", 0.12f}, {"mouthStretchLeft", 0.35f}, {"mouthStretchRight", 0.35f}};
    case 'C': // Open: "eh", "ae".
        return {{"jawOpen", 0.35f}};
    case 'D': // Wide open: "aa".
        return {{"jawOpen", 0.6f}};
    case 'E': // Slightly rounded: "ao", "er".
        return {{"jawOpen", 0.3f}, {"mouthFunnel", 0.3f}, {"mouthPucker", 0.15f}};
    case 'F': // Puckered: "uw", "ow", "w".
        return {{"jawOpen", 0.15f}, {"mouthPucker", 0.6f}, {"mouthFunnel", 0.3f}};
    case 'G': // F, V: lower lip to upper teeth.
        return {{"jawOpen", 0.08f}, {"mouthLowerDownLeft", 0.25f}, {"mouthLowerDownRight", 0.25f}, {"mouthRollLower", 0.2f}};
    case 'H': // L.
        return {{"jawOpen", 0.25f}, {"tongueOut", 0.2f}};
    case 'X': // Idle / rest.
    default:
        return {};
    }
}

std::filesystem::path ResolveRhubarbExe()
{
#if defined(_WIN32)
    const char* exe_name = "rhubarb.exe";
#else
    const char* exe_name = "rhubarb";
#endif
    const std::array<std::filesystem::path, 2> rels = {
        std::filesystem::path("tools") / "rhubarb" / exe_name,
        std::filesystem::path("rhubarb") / exe_name,
    };

    std::error_code ec;
    // Walk up from the executable directory to the workspace root so the
    // bundled tools/rhubarb/ folder is found from a build/<cfg>/ dev layout.
    if (const char* base_raw = SDL_GetBasePath())
    {
        for (std::filesystem::path cand(base_raw); !cand.empty(); cand = cand.parent_path())
        {
            for (const std::filesystem::path& rel : rels)
            {
                const std::filesystem::path p = cand / rel;
                if (std::filesystem::exists(p, ec))
                {
                    return p;
                }
            }
            if (cand == cand.root_path())
            {
                break;
            }
        }
    }

    // Fallback: current working directory and its parents.
    std::filesystem::path cwd = std::filesystem::current_path(ec);
    for (std::filesystem::path cand = cwd; !ec && !cand.empty(); cand = cand.parent_path())
    {
        for (const std::filesystem::path& rel : rels)
        {
            const std::filesystem::path p = cand / rel;
            if (std::filesystem::exists(p, ec))
            {
                return p;
            }
        }
        if (cand == cand.root_path())
        {
            break;
        }
    }

    return {};
}

namespace
{
struct MouthCue
{
    float start = 0.0f;
    float end = 0.0f;
    char shape = 'X';
};

// Trapezoidal activation of a cue at time t: ramps 0->1 over `attack` before
// the cue starts, holds 1 across the cue, ramps 1->0 over `release` after it
// ends. Overlapping ramps of adjacent cues produce co-articulation.
float CueActivation(const MouthCue& cue, float t, float attack, float release)
{
    if (t >= cue.start && t <= cue.end)
    {
        return 1.0f;
    }
    if (t < cue.start && t >= cue.start - attack)
    {
        return (t - (cue.start - attack)) / attack;
    }
    if (t > cue.end && t <= cue.end + release)
    {
        return 1.0f - ((t - cue.end) / release);
    }
    return 0.0f;
}
} // namespace

bool BakeFaceClipFromAudio(
    const std::filesystem::path& wav_path,
    FaceClipAsset& out_clip,
    std::string& out_error)
{
    out_clip = {};
    out_error.clear();

    std::error_code ec;
    const std::filesystem::path rhubarb_exe = ResolveRhubarbExe();
    if (rhubarb_exe.empty())
    {
        out_error = "Rhubarb not found. Place the Rhubarb distribution in tools/rhubarb/ "
                    "(so tools/rhubarb/rhubarb.exe exists) and rebuild/relaunch.";
        return false;
    }
    if (wav_path.empty() || !std::filesystem::exists(wav_path, ec))
    {
        out_error = "Audio file not found: " + wav_path.string();
        return false;
    }

    // Rhubarb writes the JSON result to a temp file (-o); its progress and any
    // error messages go to stdout/stderr, which we capture (2>&1) for
    // diagnostics. Separating the result file from the noisy console output is
    // far more robust than scraping JSON out of stdout. On Windows, _popen runs
    // the string through cmd.exe, so the whole command is wrapped in an extra
    // pair of quotes to survive paths containing spaces.
    const std::filesystem::path out_json =
        std::filesystem::temp_directory_path(ec) / "engine_rhubarb_out.json";
    std::filesystem::remove(out_json, ec);

    std::string inner = "\"" + rhubarb_exe.string() + "\" -f json -o \"" +
                        out_json.string() + "\" \"" + wav_path.string() + "\" 2>&1";
#if defined(_WIN32)
    const std::string command = "\"" + inner + "\"";
#else
    const std::string command = inner;
#endif

    std::string captured;
    {
        FILE* pipe = ENGINE_POPEN(command.c_str(), "r");
        if (pipe == nullptr)
        {
            out_error = "Failed to launch Rhubarb.";
            return false;
        }
        char buffer[4096];
        while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr)
        {
            captured.append(buffer);
        }
        ENGINE_PCLOSE(pipe);
    }

    std::string json_text;
    {
        std::ifstream jf(out_json, std::ios::binary);
        if (jf)
        {
            json_text.assign(std::istreambuf_iterator<char>(jf), std::istreambuf_iterator<char>());
        }
    }
    std::filesystem::remove(out_json, ec);

    json root = json::parse(json_text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        // Surface whatever Rhubarb actually said (bad audio format, missing
        // res/ folder, etc.) so the cause is visible instead of guessed.
        std::string diag = captured;
        for (char& ch : diag) { if (ch == '\r' || ch == '\n') { ch = ' '; } }
        if (diag.size() > 300) { diag = "..." + diag.substr(diag.size() - 300); }
        if (diag.empty()) { diag = "(no console output)"; }
        out_error = "Rhubarb did not produce a clip. Output: " + diag;
        return false;
    }

    std::vector<MouthCue> cues;
    if (const auto cues_it = root.find("mouthCues"); cues_it != root.end() && cues_it->is_array())
    {
        for (const json& c : *cues_it)
        {
            if (!c.is_object()) { continue; }
            MouthCue cue;
            cue.start = c.value("start", 0.0f);
            cue.end = c.value("end", 0.0f);
            const std::string v = c.value("value", std::string("X"));
            cue.shape = v.empty() ? 'X' : v[0];
            cues.push_back(cue);
        }
    }
    if (cues.empty())
    {
        out_error = "Rhubarb returned no mouth cues for this audio.";
        return false;
    }

    float duration = 0.0f;
    if (const auto meta_it = root.find("metadata"); meta_it != root.end() && meta_it->is_object())
    {
        duration = meta_it->value("duration", 0.0f);
    }
    if (duration <= 0.0f)
    {
        duration = cues.back().end;
    }

    // Sample the trapezoidal activations at a fixed rate and accumulate the
    // per-target weight (max across overlapping cues = dominance) into curves.
    constexpr float kAttack = 0.05f;
    constexpr float kRelease = 0.07f;
    constexpr float kFps = 60.0f;
    const float dt = 1.0f / kFps;
    const int sample_count = static_cast<int>(std::ceil(duration * kFps)) + 1;

    std::map<std::string, std::vector<FaceClipKeyframe>> curves_by_target;

    for (int i = 0; i < sample_count; ++i)
    {
        const float t = static_cast<float>(i) * dt;
        std::map<std::string, float> frame_weights;
        for (const MouthCue& cue : cues)
        {
            const float act = CueActivation(cue, t, kAttack, kRelease);
            if (act <= 0.0f) { continue; }
            for (const auto& [target, w] : RhubarbShapeToArkitWeights(cue.shape))
            {
                float& slot = frame_weights[target];
                slot = (std::max)(slot, act * w);
            }
        }
        for (const auto& [target, w] : frame_weights)
        {
            curves_by_target[target].push_back({t, w});
        }
    }

    out_clip.duration_seconds = duration;
    out_clip.curves.reserve(curves_by_target.size());
    for (auto& [target, keys] : curves_by_target)
    {
        // Decimate colinear interior keys (the activations are piecewise
        // linear, so 60 Hz sampling oversamples the ramps) to keep clips small.
        std::vector<FaceClipKeyframe> decimated;
        decimated.reserve(keys.size());
        for (std::size_t k = 0; k < keys.size(); ++k)
        {
            if (k == 0 || k + 1 == keys.size())
            {
                decimated.push_back(keys[k]);
                continue;
            }
            const FaceClipKeyframe& prev = decimated.back();
            const FaceClipKeyframe& next = keys[k + 1];
            const float span = next.time_seconds - prev.time_seconds;
            float interp = keys[k].weight;
            if (span > 1e-6f)
            {
                const float a = (keys[k].time_seconds - prev.time_seconds) / span;
                interp = prev.weight + (next.weight - prev.weight) * a;
            }
            if (std::fabs(interp - keys[k].weight) > 1e-3f)
            {
                decimated.push_back(keys[k]);
            }
        }

        FaceClipCurve curve;
        curve.target = target;
        curve.keys = std::move(decimated);
        out_clip.curves.push_back(std::move(curve));
    }

    return true;
}
