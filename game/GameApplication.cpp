#include "GameApplication.h"

#include "assets/SceneMetadata.h"
#include "vfs/PakArchive.h"
#include "vfs/AssetVFS.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>

#include <stb_image.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objidl.h>
#include <wincodec.h>
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
class ScopedComInitialization
{
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result_))
        {
            CoUninitialize();
        }
    }

    HRESULT Result() const
    {
        return result_;
    }

private:
    HRESULT result_;
};

bool DecodeImageWithWic(
    const std::vector<std::uint8_t>& bytes,
    std::vector<std::uint8_t>& out_rgba,
    int& out_width,
    int& out_height)
{
    out_rgba.clear();
    out_width = 0;
    out_height = 0;

    if (bytes.empty())
    {
        return false;
    }

    ScopedComInitialization com;
    if (FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE)
    {
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(hr) || factory == nullptr)
    {
        return false;
    }

    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr) || stream == nullptr)
    {
        factory->Release();
        return false;
    }

    std::vector<std::uint8_t> stream_bytes = bytes;
    hr = stream->InitializeFromMemory(stream_bytes.data(), static_cast<DWORD>(stream_bytes.size()));
    if (FAILED(hr))
    {
        stream->Release();
        factory->Release();
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || decoder == nullptr)
    {
        stream->Release();
        factory->Release();
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || frame == nullptr)
    {
        decoder->Release();
        stream->Release();
        factory->Release();
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0)
    {
        frame->Release();
        decoder->Release();
        stream->Release();
        factory->Release();
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr) || converter == nullptr)
    {
        frame->Release();
        decoder->Release();
        stream->Release();
        factory->Release();
        return false;
    }

    hr = converter->Initialize(
        frame,
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        converter->Release();
        frame->Release();
        decoder->Release();
        stream->Release();
        factory->Release();
        return false;
    }

    const UINT stride = width * 4;
    const UINT data_size = stride * height;
    out_rgba.resize(data_size);

    hr = converter->CopyPixels(nullptr, stride, data_size, out_rgba.data());

    converter->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
    factory->Release();

    if (FAILED(hr))
    {
        out_rgba.clear();
        return false;
    }

    out_width = static_cast<int>(width);
    out_height = static_cast<int>(height);
    return true;
}
#endif

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::unordered_map<std::string, std::string> ParseConfig(const std::filesystem::path& path)
{
    std::unordered_map<std::string, std::string> result;
    std::ifstream file(path);
    if (!file)
    {
        return result;
    }

    std::string line;
    while (std::getline(file, line))
    {
        const auto pos = line.find('=');
        if (pos == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        const auto trim = [](std::string& s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\n'))
            {
                s.erase(s.begin());
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
            {
                s.pop_back();
            }
        };

        trim(key);
        trim(value);
        result[key] = value;
    }

    return result;
}
} // namespace

std::filesystem::path GameApplication::ResolveExeDirectory(const char* argv0) const
{
    const char* base_path = SDL_GetBasePath();
    if (base_path != nullptr)
    {
        return std::filesystem::path(base_path);
    }

    if (argv0 != nullptr)
    {
        const std::filesystem::path exe_path(argv0);
        if (exe_path.has_parent_path())
        {
            return exe_path.parent_path();
        }
    }

    std::error_code ec;
    return std::filesystem::current_path(ec);
}

bool GameApplication::LoadConfig(const std::filesystem::path& exe_dir)
{
    const std::filesystem::path config_path = exe_dir / "config.ini";
    const auto config = ParseConfig(config_path);

    const auto title_it = config.find("windowTitle");
    window_title_ = title_it != config.end() ? title_it->second : "Game";

    const auto content_it = config.find("contentRoot");
    const std::string content_root_rel = content_it != config.end() ? content_it->second : "Content";
    content_root_ = exe_dir / content_root_rel;

    const auto scene_it = config.find("startupScene");
    if (scene_it == config.end() || scene_it->second.empty())
    {
        SDL_Log("config.ini: startupScene is missing or empty");
        return false;
    }

    startup_scene_path_ = std::filesystem::path(scene_it->second);

    const auto icon_it = config.find("appIcon");
    app_icon_path_ = icon_it != config.end() ? icon_it->second : std::string();
    return true;
}

bool GameApplication::InitializeAssetStreaming(const std::filesystem::path& exe_dir)
{
    // Runtime game builds stream from assets.pak; no extraction step.
    std::error_code ec;
    const std::filesystem::path pak_path = exe_dir / "assets.pak";
    if (!std::filesystem::exists(pak_path, ec))
    {
        SDL_Log("assets.pak not found at: %s", pak_path.string().c_str());
        return false;
    }

    pak_archive_ = std::make_unique<PakArchive>();
    if (!pak_archive_->Open(pak_path))
    {
        SDL_Log("Failed to open assets.pak at: %s", pak_path.string().c_str());
        return false;
    }

    SetGlobalAssetReader(std::make_shared<PakAssetReader>(*pak_archive_));

    if (!g_asset_reader || !g_asset_reader->FileExists(startup_scene_path_.generic_string()))
    {
        SDL_Log("Startup scene is missing in assets.pak: %s", startup_scene_path_.generic_string().c_str());
        return false;
    }

    if (!app_icon_path_.empty() && !g_asset_reader->FileExists(app_icon_path_))
    {
        SDL_Log("Configured app icon is missing in assets.pak: %s", app_icon_path_.c_str());
        return false;
    }

    SDL_Log("Opened assets.pak for streaming: %s", pak_path.string().c_str());
    return true;
}

bool GameApplication::ApplyWindowIconFromPak()
{
    if (window_ == nullptr || app_icon_path_.empty())
    {
        return true;
    }

    const std::vector<std::uint8_t> icon_bytes = ReadAssetFileAsBytes(app_icon_path_);
    if (icon_bytes.empty())
    {
        SDL_Log("App icon not found in assets.pak: %s", app_icon_path_.c_str());
        return false;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> decoded_rgba;
    stbi_uc* pixels = stbi_load_from_memory(
        icon_bytes.data(),
        static_cast<int>(icon_bytes.size()),
        &width,
        &height,
        &channels,
        4);

    const bool stb_ok = pixels != nullptr && width > 0 && height > 0;
    if (!stb_ok)
    {
#ifdef _WIN32
        // stb_image does not reliably decode AVIF in this project; use WIC fallback on Windows.
        const std::string extension = ToLowerCopy(std::filesystem::path(app_icon_path_).extension().string());
        if (extension == ".avif" && DecodeImageWithWic(icon_bytes, decoded_rgba, width, height))
        {
            SDL_Log("Decoded AVIF app icon with WIC fallback");
        }
        else
        {
            SDL_Log("Failed to decode app icon from pak entry '%s': %s", app_icon_path_.c_str(), stbi_failure_reason());
            return false;
        }
#else
        SDL_Log("Failed to decode app icon from pak entry '%s': %s", app_icon_path_.c_str(), stbi_failure_reason());
        return false;
#endif
    }

    SDL_Surface* icon_surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (icon_surface == nullptr)
    {
        stbi_image_free(pixels);
        SDL_Log("Failed to create SDL surface for app icon: %s", SDL_GetError());
        return false;
    }

    for (int row = 0; row < height; ++row)
    {
        const std::size_t source_offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4;
        std::uint8_t* destination_row = static_cast<std::uint8_t*>(icon_surface->pixels) + static_cast<std::size_t>(row) * static_cast<std::size_t>(icon_surface->pitch);
        const std::uint8_t* source_data = nullptr;
        if (stb_ok)
        {
            source_data = pixels + source_offset;
        }
        else
        {
            source_data = decoded_rgba.data() + source_offset;
        }

        std::memcpy(destination_row, source_data, static_cast<std::size_t>(width) * 4);
    }

    SDL_SetWindowIcon(window_, icon_surface);

    SDL_DestroySurface(icon_surface);
    if (pixels != nullptr)
    {
        stbi_image_free(pixels);
    }
    SDL_Log("Applied app icon from assets.pak: %s", app_icon_path_.c_str());
    return true;
}

bool GameApplication::Init(int argc, char* argv[])
{
    const std::filesystem::path exe_dir = ResolveExeDirectory(argc > 0 ? argv[0] : nullptr);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    if (!LoadConfig(exe_dir))
    {
        return false;
    }

    if (!InitializeAssetStreaming(exe_dir))
    {
        return false;
    }

    const SDL_WindowFlags window_flags =
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(window_title_.c_str(), 1280, 720, window_flags);
    if (window_ == nullptr)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    if (!ApplyWindowIconFromPak())
    {
        SDL_Log("Continuing without custom app icon");
    }

    if (!vulkan_context_.Initialize(window_))
    {
        SDL_Log("VulkanContext::Initialize failed");
        return false;
    }

    // Minimal ImGui init — required because VulkanContext uses ImGui data
    // structures for swapchain resource management. No ImGui widgets are shown.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForVulkan(window_);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = vulkan_context_.GetInstance();
    init_info.PhysicalDevice = vulkan_context_.GetPhysicalDevice();
    init_info.Device = vulkan_context_.GetDevice();
    init_info.QueueFamily = vulkan_context_.GetQueueFamily();
    init_info.Queue = vulkan_context_.GetQueue();
    init_info.PipelineCache = vulkan_context_.GetPipelineCache();
    init_info.DescriptorPool = vulkan_context_.GetDescriptorPool();
    init_info.MinImageCount = vulkan_context_.GetMinImageCount();
    init_info.ImageCount = vulkan_context_.GetImageCount();
    init_info.Allocator = vulkan_context_.GetAllocator();
    init_info.PipelineInfoMain.RenderPass = vulkan_context_.GetRenderPass();
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = VulkanContext::CheckVkResult;
    ImGui_ImplVulkan_Init(&init_info);

    if (!renderer_.Initialize(&vulkan_context_))
    {
        SDL_Log("RuntimeRenderer::Initialize failed");
        return false;
    }

    const std::filesystem::path scene_path = startup_scene_path_;
    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    const ActiveSceneCameraSelection camera = FindActiveSceneCamera(scene_metadata);

    std::string start_error;
    if (!renderer_.StartSession({}, scene_path, camera, &start_error))
    {
        SDL_Log("RuntimeRenderer::StartSession failed: %s", start_error.c_str());
        return false;
    }

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    running_ = true;
    return true;
}

void GameApplication::RunLoop()
{
    while (running_)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT)
            {
                running_ = false;
            }
            else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     event.window.windowID == SDL_GetWindowID(window_))
            {
                running_ = false;
            }
        }

        if (!running_)
        {
            break;
        }

        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
        {
            SDL_Delay(10);
            continue;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        if (width <= 0 || height <= 0)
        {
            SDL_Delay(10);
            continue;
        }

        std::string render_error;
        if (!renderer_.RenderFrame(
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                &render_error))
        {
            SDL_Log("RenderFrame failed: %s", render_error.c_str());
            running_ = false;
            break;
        }

        if (!vulkan_context_.PresentImageToMainWindow(
                window_,
                renderer_.GetOutputImage(),
                renderer_.GetOutputLayout(),
                renderer_.GetOutputWidth(),
                renderer_.GetOutputHeight()))
        {
            SDL_Log("PresentImageToMainWindow failed");
        }
    }
}

void GameApplication::Shutdown()
{
    SetGlobalAssetReader(nullptr);
    pak_archive_.reset();

    renderer_.Shutdown();

    vulkan_context_.WaitIdle();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    vulkan_context_.Shutdown();

    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}
