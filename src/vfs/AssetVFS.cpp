#include "vfs/AssetVFS.h"

#include <algorithm>
#include <cctype>

namespace
{
std::string ToForwardSlashes(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

void AddCandidate(std::vector<std::string>& candidates, std::string candidate)
{
    if (candidate.empty())
    {
        return;
    }

    while (candidate.rfind("./", 0) == 0)
    {
        candidate.erase(0, 2);
    }

    if (!candidate.empty() && candidate[0] == '/')
    {
        candidate.erase(0, 1);
    }

    if (candidate.empty())
    {
        return;
    }

    if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
    {
        candidates.push_back(std::move(candidate));
    }
}

std::string StripContentPrefix(const std::string& path)
{
    const std::string lowered = ToLowerAscii(path);
    if (lowered.rfind("content/", 0) == 0)
    {
        return path.substr(8);
    }

    const std::string marker = "/content/";
    const std::size_t marker_pos = lowered.find(marker);
    if (marker_pos != std::string::npos)
    {
        return path.substr(marker_pos + marker.size());
    }

    return {};
}

std::vector<std::string> BuildPathCandidates(const std::string& raw_path)
{
    std::vector<std::string> candidates;
    const std::string normalized = ToForwardSlashes(raw_path);

    AddCandidate(candidates, normalized);
    AddCandidate(candidates, StripContentPrefix(normalized));

    if (normalized.size() > 3 && std::isalpha(static_cast<unsigned char>(normalized[0])) != 0 && normalized[1] == ':' && normalized[2] == '/')
    {
        const std::string without_drive = normalized.substr(3);
        AddCandidate(candidates, without_drive);
        AddCandidate(candidates, StripContentPrefix(without_drive));
    }

    return candidates;
}
} // namespace

std::shared_ptr<IAssetReader> g_asset_reader;

std::vector<std::uint8_t> PakAssetReader::ReadFile(const std::string& rel_path) const
{
    if (!pak_.IsOpen())
    {
        return {};
    }

    for (const std::string& candidate : BuildPathCandidates(rel_path))
    {
        if (pak_.Contains(candidate))
        {
            return pak_.ReadEntry(candidate);
        }
    }

    return {};
}

bool PakAssetReader::FileExists(const std::string& rel_path) const
{
    if (!pak_.IsOpen())
    {
        return false;
    }

    for (const std::string& candidate : BuildPathCandidates(rel_path))
    {
        if (pak_.Contains(candidate))
        {
            return true;
        }
    }

    return false;
}

void SetGlobalAssetReader(std::shared_ptr<IAssetReader> reader)
{
    g_asset_reader = std::move(reader);
}

std::vector<std::uint8_t> ReadAssetFileAsBytes(const std::string& rel_path)
{
    if (!g_asset_reader)
    {
        return {};
    }

    return g_asset_reader->ReadFile(rel_path);
}
