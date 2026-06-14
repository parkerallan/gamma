#include "assets/FaceClipAsset.h"
#include "vfs/AssetVFS.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

float FaceClipCurve::Sample(float time_seconds) const
{
    if (keys.empty())
    {
        return 0.0f;
    }
    if (time_seconds <= keys.front().time_seconds)
    {
        return keys.front().weight;
    }
    if (time_seconds >= keys.back().time_seconds)
    {
        return keys.back().weight;
    }

    // Binary search for the first key at or after time_seconds, then lerp with
    // the previous key.
    const auto it = std::lower_bound(
        keys.begin(), keys.end(), time_seconds,
        [](const FaceClipKeyframe& key, float t) { return key.time_seconds < t; });

    const FaceClipKeyframe& hi = *it;
    const FaceClipKeyframe& lo = *(it - 1);
    const float span = hi.time_seconds - lo.time_seconds;
    if (span <= 0.0f)
    {
        return hi.weight;
    }
    const float alpha = (time_seconds - lo.time_seconds) / span;
    return lo.weight + (hi.weight - lo.weight) * alpha;
}

void FaceClipAsset::SampleInto(float time_seconds, std::vector<std::pair<std::string, float>>& out) const
{
    for (const FaceClipCurve& curve : curves)
    {
        const float w = curve.Sample(time_seconds);
        if (std::fabs(w) > 1e-4f)
        {
            out.emplace_back(curve.target, w);
        }
    }
}

bool LoadFaceClipAsset(const std::filesystem::path& path, FaceClipAsset& out_asset, std::string& out_error)
{
    out_asset = {};
    out_error.clear();

    json root;
    std::ifstream input(path, std::ios::binary);
    if (input)
    {
        root = json::parse(input, nullptr, false);
    }
    else if (g_asset_reader)
    {
        const std::vector<std::uint8_t> bytes = ReadAssetFileAsBytes(path.generic_string());
        if (!bytes.empty())
        {
            root = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        }
    }

    if (root.is_discarded() || !root.is_object())
    {
        out_error = "Failed to open face clip file or it is not valid JSON.";
        return false;
    }

    out_asset.version = root.value("version", 1);
    out_asset.name = root.value("name", std::string());
    out_asset.source_audio_path = root.value("source_audio_path", std::string());
    out_asset.duration_seconds = root.value("duration_seconds", 0.0f);

    if (const auto curves_it = root.find("curves"); curves_it != root.end() && curves_it->is_array())
    {
        for (const json& curve_json : *curves_it)
        {
            if (!curve_json.is_object())
            {
                continue;
            }
            FaceClipCurve curve;
            curve.target = curve_json.value("target", std::string());
            if (curve.target.empty())
            {
                continue;
            }
            if (const auto keys_it = curve_json.find("keys"); keys_it != curve_json.end() && keys_it->is_array())
            {
                for (const json& key_json : *keys_it)
                {
                    // Keys are stored as a flat [time, weight] pair for compactness.
                    if (key_json.is_array() && key_json.size() == 2 &&
                        key_json[0].is_number() && key_json[1].is_number())
                    {
                        FaceClipKeyframe key;
                        key.time_seconds = key_json[0].get<float>();
                        key.weight = key_json[1].get<float>();
                        curve.keys.push_back(key);
                    }
                }
            }
            // Keep keys sorted so Sample()'s binary search is valid regardless
            // of authoring order.
            std::sort(curve.keys.begin(), curve.keys.end(),
                      [](const FaceClipKeyframe& a, const FaceClipKeyframe& b)
                      { return a.time_seconds < b.time_seconds; });
            if (!curve.keys.empty())
            {
                out_asset.curves.push_back(std::move(curve));
            }
        }
    }

    return true;
}

bool SaveFaceClipAsset(const std::filesystem::path& path, const FaceClipAsset& asset, std::string& out_error)
{
    out_error.clear();

    json root;
    root["version"] = asset.version;
    root["name"] = asset.name;
    root["source_audio_path"] = asset.source_audio_path;
    root["duration_seconds"] = asset.duration_seconds;
    root["curves"] = json::array();
    for (const FaceClipCurve& curve : asset.curves)
    {
        json curve_json;
        curve_json["target"] = curve.target;
        json keys = json::array();
        for (const FaceClipKeyframe& key : curve.keys)
        {
            keys.push_back(json::array({key.time_seconds, key.weight}));
        }
        curve_json["keys"] = std::move(keys);
        root["curves"].push_back(std::move(curve_json));
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        out_error = "Failed to open face clip for writing.";
        return false;
    }
    output << root.dump(2) << "\n";
    if (!output.good())
    {
        out_error = "Failed while writing face clip file.";
        return false;
    }
    return true;
}
