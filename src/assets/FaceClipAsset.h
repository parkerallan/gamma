#pragma once

// A FaceClip is a baked facial-animation artifact: per-ARKit-blendshape weight
// curves over time. It is generator-agnostic -- the Rhubarb lip-sync bake
// produces one, and the runtime / preview player consumes one, without either
// side knowing how the curves were authored. This keeps the door open to swap
// the bake generator (Rhubarb now, a neural baker later) with no format churn.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct FaceClipKeyframe
{
    float time_seconds = 0.0f;
    float weight = 0.0f;
};

// One blendshape target's weight over the clip. Keyframes are kept sorted by
// time_seconds (ascending); sampling linearly interpolates between them and
// clamps to the first/last key outside the range.
struct FaceClipCurve
{
    std::string target; // ARKit blendshape name (see ArkitBlendshapeNames())
    std::vector<FaceClipKeyframe> keys;

    float Sample(float time_seconds) const;
};

struct FaceClipAsset
{
    int version = 1;
    std::string name;
    // The audio file this clip was baked from (project-relative), for re-bake
    // and runtime audio sync. Empty for hand-authored clips.
    std::string source_audio_path;
    float duration_seconds = 0.0f;
    std::vector<FaceClipCurve> curves;

    // Samples every curve at time_seconds and appends (target, weight) pairs to
    // out for any non-zero weight. Pairs with weight ~0 are skipped so callers
    // can layer the result cheaply. Existing contents of out are preserved.
    void SampleInto(float time_seconds, std::vector<std::pair<std::string, float>>& out) const;
};

bool LoadFaceClipAsset(const std::filesystem::path& path, FaceClipAsset& out_asset, std::string& out_error);
bool SaveFaceClipAsset(const std::filesystem::path& path, const FaceClipAsset& asset, std::string& out_error);
