#pragma once

// Offline lip-sync bake: turns an audio file into a FaceClip of ARKit
// blendshape weight curves by running Rhubarb Lip Sync and mapping its mouth
// shapes through a viseme table with co-articulation. Editor-only (the shipped
// game plays the baked FaceClip; it never bakes).

#include "assets/FaceClipAsset.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Default mapping from a Rhubarb mouth shape ('A'..'H','X') to a set of ARKit
// blendshape weights. Unknown shapes (and 'X', the rest pose) map to empty.
std::vector<std::pair<std::string, float>> RhubarbShapeToArkitWeights(char shape);

// Locates the bundled Rhubarb executable by walking up from the running
// executable to the workspace root and checking tools/rhubarb/. Returns an
// empty path if not found (drop the Rhubarb distribution into tools/rhubarb/).
std::filesystem::path ResolveRhubarbExe();

// Bakes wav_path into out_clip as ARKit weight curves with co-articulation
// ramps, using the bundled Rhubarb (see ResolveRhubarbExe). Returns false and
// sets out_error on any failure (Rhubarb not bundled, bad audio, unparseable
// output).
bool BakeFaceClipFromAudio(
    const std::filesystem::path& wav_path,
    FaceClipAsset& out_clip,
    std::string& out_error);
