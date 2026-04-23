#pragma once

#include "vfs/PakArchive.h"

#include <memory>
#include <string>
#include <vector>

class IAssetReader
{
public:
    virtual ~IAssetReader() = default;

    virtual std::vector<std::uint8_t> ReadFile(const std::string& rel_path) const = 0;
    virtual bool FileExists(const std::string& rel_path) const = 0;
};

class PakAssetReader : public IAssetReader
{
public:
    explicit PakAssetReader(const PakArchive& pak) : pak_(pak) {}

    std::vector<std::uint8_t> ReadFile(const std::string& rel_path) const override;
    bool FileExists(const std::string& rel_path) const override;

private:
    const PakArchive& pak_;
};

extern std::shared_ptr<IAssetReader> g_asset_reader;

void SetGlobalAssetReader(std::shared_ptr<IAssetReader> reader);
std::vector<std::uint8_t> ReadAssetFileAsBytes(const std::string& rel_path);
