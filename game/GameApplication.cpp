#include "GameApplication.h"

#include "assets/ModelAsset.h"
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
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

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
        SDL_Log("Failed to decode app icon from pak entry '%s': %s", app_icon_path_.c_str(), stbi_failure_reason());
        return false;
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
        const std::uint8_t* source_data = pixels + source_offset;

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
    const std::uint64_t init_start_ticks = SDL_GetPerformanceCounter();
    const std::uint64_t perf_freq = SDL_GetPerformanceFrequency();
    auto ms_since = [perf_freq](std::uint64_t start) -> double
    {
        if (perf_freq == 0)
        {
            return 0.0;
        }
        return static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 / static_cast<double>(perf_freq);
    };
    auto stage = [&](const char* name, std::uint64_t& mark)
    {
        SDL_Log("Game init [%s]: %.2f ms (cumulative %.2f ms)",
                name, ms_since(mark), ms_since(init_start_ticks));
        mark = SDL_GetPerformanceCounter();
    };
    std::uint64_t stage_mark = init_start_ticks;

    const std::filesystem::path exe_dir = ResolveExeDirectory(argc > 0 ? argv[0] : nullptr);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    stage("SDL_Init", stage_mark);

    if (!LoadConfig(exe_dir))
    {
        return false;
    }
    stage("LoadConfig", stage_mark);

    if (!InitializeAssetStreaming(exe_dir))
    {
        return false;
    }
    stage("InitializeAssetStreaming", stage_mark);

    // Parse the scene now (cheap — ~5 ms) so we can immediately dispatch
    // per-model Assimp parses on a background thread. They run sequentially
    // there (the pak reader holds a single file handle and is not safe to
    // call concurrently), but the whole batch overlaps with
    // VulkanContext::Initialize (~1.2 s of driver work), which collapses the
    // dominant startup cost.
    SceneMetadata preloaded_scene = LoadSceneMetadata(startup_scene_path_);
    stage("LoadSceneMetadata (early)", stage_mark);

    struct PreloadedModel
    {
        std::filesystem::path path;
        std::filesystem::file_time_type write_time = std::filesystem::file_time_type::min();
        ModelAsset asset;
    };

    std::vector<std::filesystem::path> model_paths;
    std::vector<std::string> audio_paths;
    std::vector<std::string> video_paths;
    {
        std::unordered_set<std::string> seen_models;
        std::unordered_set<std::string> seen_audio;
        std::unordered_set<std::string> seen_video;
        model_paths.reserve(preloaded_scene.objects.size());
        for (const SceneObjectMetadata& object : preloaded_scene.objects)
        {
            // Builtin shapes are packed under "Shapes/<filename>" — resolve the pak
            // key from the Shape3D attribute when available so preloading matches
            // the key used by RuntimeRenderer at runtime.
            std::string model_key;
            for (const SceneObjectAttribute& attr : object.attributes)
            {
                if (attr.kind == SceneObjectAttributeKind::Shape3D)
                {
                    if (!attr.shape_3d.shape_path.empty())
                    {
                        model_key = "Shapes/" + attr.shape_3d.shape_path;
                    }
                    else if (!object.model_path.empty())
                    {
                        model_key = "Shapes/" + std::filesystem::path(object.model_path).filename().string();
                    }
                    break;
                }
            }
            if (model_key.empty() && !object.model_path.empty())
            {
                model_key = object.model_path;
            }

            if (!model_key.empty() && seen_models.insert(model_key).second)
            {
                model_paths.emplace_back(model_key);
            }
            for (const SceneObjectAttribute& attr : object.attributes)
            {
                if (!attr.audio.clip_path.empty() && seen_audio.insert(attr.audio.clip_path).second)
                {
                    audio_paths.push_back(attr.audio.clip_path);
                }
                if (!attr.video_2d.video_path.empty() && seen_video.insert(attr.video_2d.video_path).second)
                {
                    video_paths.push_back(attr.video_2d.video_path);
                }
            }
        }
    }

    // All asset I/O runs on a single background thread because g_asset_reader
    // owns a non-thread-safe shared pak handle. While the main thread sets up
    // SDL/Vulkan/ImGui this worker fans out model parsing + raw byte reads
    // for audio clips and video files.
    struct PreloadedAssets
    {
        std::vector<PreloadedModel> models;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> audio_bytes;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> video_bytes;
    };
    std::future<PreloadedAssets> assets_future = std::async(
        std::launch::async,
        [model_paths, audio_paths, video_paths]() -> PreloadedAssets
        {
            PreloadedAssets out;
            out.models.reserve(model_paths.size());
            for (const std::filesystem::path& model_path : model_paths)
            {
                PreloadedModel result;
                result.path = model_path;
                std::error_code ec;
                const auto wt = std::filesystem::last_write_time(model_path, ec);
                result.write_time = ec ? std::filesystem::file_time_type::min() : wt;
                result.asset = LoadModelAsset(model_path);
                out.models.push_back(std::move(result));
            }
            out.audio_bytes.reserve(audio_paths.size());
            for (const std::string& clip_path : audio_paths)
            {
                std::vector<std::uint8_t> bytes;
                if (g_asset_reader)
                {
                    bytes = g_asset_reader->ReadFile(clip_path);
                }
                if (!bytes.empty())
                {
                    out.audio_bytes.emplace_back(clip_path, std::move(bytes));
                }
            }
            out.video_bytes.reserve(video_paths.size());
            for (const std::string& video_path : video_paths)
            {
                std::vector<std::uint8_t> bytes;
                if (g_asset_reader)
                {
                    bytes = g_asset_reader->ReadFile(video_path);
                }
                if (!bytes.empty())
                {
                    out.video_bytes.emplace_back(video_path, std::move(bytes));
                }
            }
            return out;
        });
    SDL_Log("Game init: dispatched preload of %zu model(s) + %zu audio + %zu video on background thread",
            model_paths.size(), audio_paths.size(), video_paths.size());

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

    // Use FIFO (vsync) for the standalone game. With a single swapchain there is
    // no editor back-pressure to worry about, and FIFO blocks at the hardware
    // vblank, giving deterministic 60 Hz pacing that pairs cleanly with the
    // 1/120 Hz physics fixed step (exactly 2 deterministic steps per frame, alpha
    // near 0). Running uncapped MAILBOX at several hundred FPS cycles the TAA
    // jitter sequence fast enough to read as shimmer trailing fast movers
    // (especially when the camera is parented to the player).
    if (!vulkan_context_.Initialize(window_, /*prefer_low_latency=*/false))
    {
        SDL_Log("VulkanContext::Initialize failed");
        return false;
    }
    stage("VulkanContext::Initialize", stage_mark);

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
    stage("ImGui_ImplVulkan_Init", stage_mark);

    if (!renderer_.Initialize(&vulkan_context_))
    {
        SDL_Log("RuntimeRenderer::Initialize failed");
        return false;
    }
    stage("RuntimeRenderer::Initialize", stage_mark);

    const std::filesystem::path scene_path = startup_scene_path_;
    SceneMetadata scene_metadata = std::move(preloaded_scene);
    const ActiveSceneCameraSelection camera = FindActiveSceneCamera(scene_metadata);

    std::string start_error;
    if (!renderer_.StartSession({}, scene_path, camera, &start_error))
    {
        SDL_Log("RuntimeRenderer::StartSession failed: %s", start_error.c_str());
        return false;
    }
    stage("RuntimeRenderer::StartSession", stage_mark);

    // Hand the seeded scene + preloaded models to the renderer so the first
    // RenderFrame skips Assimp parsing and metadata I/O entirely.
    renderer_.SeedSceneMetadata(scene_path, scene_metadata);
    PreloadedAssets preloaded = assets_future.get();
    std::size_t seeded_models = 0;
    for (PreloadedModel& model : preloaded.models)
    {
        if (!model.asset.loaded)
        {
            continue;
        }
        renderer_.SeedModelAsset(model.path, model.write_time, std::move(model.asset));
        ++seeded_models;
    }
    for (auto& [clip_path, bytes] : preloaded.audio_bytes)
    {
        renderer_.SeedAudioClipBytes(clip_path, std::move(bytes));
    }
    for (auto& [video_path, bytes] : preloaded.video_bytes)
    {
        renderer_.SeedVideoBytes(video_path, std::move(bytes));
    }
    stage("Wait+seed preloaded assets", stage_mark);
    SDL_Log("Game init: seeded %zu model(s) + %zu audio + %zu video into runtime cache",
            seeded_models, preloaded.audio_bytes.size(), preloaded.video_bytes.size());

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    running_ = true;
    SDL_Log("Game init total: %.2f ms (window now visible; first RenderFrame next)", ms_since(init_start_ticks));
    return true;
}

void GameApplication::RunLoop()
{
    // Frame pacing is owned by the swapchain present mode (MAILBOX — see
    // Initialize). Adding a software SDL_DelayNS deadline on top makes the
    // submission phase drift across the hardware vblank because the sleep has
    // ~1 ms of OS scheduler jitter, which is what produced the slow-rotating
    // "smooth → stuttery → smooth" motion users reported. Run the loop free
    // and let MAILBOX present the freshest image at each vblank.

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
