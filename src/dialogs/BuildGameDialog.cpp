#include "dialogs/BuildGameDialog.h"

#include "components/BuildSettingsComponent.h"
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

namespace
{
std::string TrimCopy(std::string value)
{
	const auto is_space = [](unsigned char character)
	{
		return std::isspace(character) != 0;
	};

	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char character)
	{
		return !is_space(character);
	}));
	value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char character)
	{
		return !is_space(character);
	}).base(), value.end());
	return value;
}

bool IsValidGameNameCharacter(char character)
{
	return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '-' || character == ' ';
}

bool IsSupportedIconExtension(const std::filesystem::path& path)
{
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	return extension == ".avif" || extension == ".png" || extension == ".ico";
}

std::filesystem::path ResolveInputPath(const EngineState& state, const std::filesystem::path& input_path)
{
	if (input_path.empty())
	{
		return {};
	}

	if (!input_path.is_relative())
	{
		return input_path.lexically_normal();
	}

	if (!state.project_root.empty())
	{
		return (state.project_root / input_path).lexically_normal();
	}

	if (!state.workspace_root.empty())
	{
		return (state.workspace_root / input_path).lexically_normal();
	}

	return input_path.lexically_normal();
}

const char* GetBuildTypeLabel(int build_type_index)
{
	return build_type_index == 0 ? "Debug" : "Final";
}
}

void BuildGameDialog::Open(const EngineState& state)
{
	if (!state.pending_build_request.game_name.empty())
	{
		build_type_index_ = state.pending_build_request.build_type == EngineBuildType::Debug ? 0 : 1;
	}
	ImGui::OpenPopup("Build Game");
}

bool BuildGameDialog::Render(EngineState& state)
{
	if (!ImGui::BeginPopupModal("Build Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return false;
	}

	ImGui::TextUnformatted("Create a separate staged output folder for the packaged game.");
	ImGui::TextUnformatted("The selected location will contain your chosen output folder name.");
	ImGui::Separator();

	BuildSettingsComponent::Render(state);

	const char* build_type_labels[] = {"Debug", "Final"};
	ImGui::Combo("Build type", &build_type_index_, build_type_labels, IM_ARRAYSIZE(build_type_labels));

	const std::string trimmed_game_name = TrimCopy(state.build_executable_name);
	const std::string trimmed_folder_name = TrimCopy(state.build_folder_name);
	std::filesystem::path stage_root;
	if (!state.build_output_root.empty() && !trimmed_folder_name.empty())
	{
		stage_root = std::filesystem::path(state.build_output_root) / trimmed_folder_name;
	}
	std::filesystem::path build_cache_root;
	if (!state.build_output_root.empty() && !trimmed_folder_name.empty())
	{
		build_cache_root = std::filesystem::path(state.build_output_root) / (trimmed_folder_name + "-build");
	}
	ImGui::Separator();
	ImGui::TextWrapped("Staged output folder: %s", stage_root.empty() ? "Pending folder name and build location" : stage_root.generic_string().c_str());
	ImGui::TextWrapped("Build cache folder: %s", build_cache_root.empty() ? "Pending folder name and build location" : build_cache_root.generic_string().c_str());
	ImGui::TextWrapped("Executable name: %s.exe", trimmed_game_name.empty() ? "<game name required>" : trimmed_game_name.c_str());
	ImGui::TextWrapped("Window title: %s", state.build_window_title.empty() ? "<defaults to executable name>" : state.build_window_title.c_str());
	ImGui::TextWrapped("App icon: %s", state.build_app_icon_path.empty() ? "None" : state.build_app_icon_path.generic_string().c_str());
	ImGui::TextWrapped("Configuration: %s", GetBuildTypeLabel(build_type_index_));

	bool submitted = false;
	if (ImGui::Button("Queue Build"))
	{
		submitted = SubmitBuildRequest(state);
		if (submitted)
		{
			ImGui::CloseCurrentPopup();
		}
	}

	ImGui::SameLine();
	if (ImGui::Button("Cancel"))
	{
		ImGui::CloseCurrentPopup();
	}

	ImGui::EndPopup();
	return submitted;
}

bool BuildGameDialog::SubmitBuildRequest(EngineState& state)
{
	const std::string game_name = TrimCopy(state.build_executable_name);
	if (game_name.empty())
	{
		state.SetBuildError("Cannot build game: enter a game name");
		return false;
	}

	for (char character : game_name)
	{
		if (!IsValidGameNameCharacter(character))
		{
			state.SetBuildError("Cannot build game: use letters, numbers, spaces, '-' or '_' in the game name");
			return false;
		}
	}

    const std::string folder_name = TrimCopy(state.build_folder_name);
    if (folder_name.empty())
    {
        state.SetBuildError("Cannot build game: enter an output folder name");
        return false;
    }

    for (char character : folder_name)
    {
        if (!IsValidGameNameCharacter(character))
        {
            state.SetBuildError("Cannot build game: use letters, numbers, spaces, '-' or '_' in the folder name");
            return false;
        }
    }

    std::string window_title = TrimCopy(state.build_window_title);
    if (window_title.empty())
    {
        window_title = game_name;
    }

	std::filesystem::path output_root = ResolveInputPath(state, state.build_output_root);
	if (output_root.empty())
	{
		state.SetBuildError("Cannot build game: choose a build output folder");
		return false;
	}
	output_root = output_root.lexically_normal();

	if (!std::filesystem::exists(output_root) || !std::filesystem::is_directory(output_root))
	{
		state.SetBuildError("Cannot build game: output folder does not exist");
		return false;
	}

    std::filesystem::path app_icon_path = ResolveInputPath(state, state.build_app_icon_path);
    if (!app_icon_path.empty())
    {
        if (!std::filesystem::exists(app_icon_path) || !std::filesystem::is_regular_file(app_icon_path))
        {
            state.SetBuildError("Cannot build game: app icon file does not exist");
            return false;
        }

        if (!IsSupportedIconExtension(app_icon_path))
        {
            state.SetBuildError("Cannot build game: app icon must be .avif, .png, or .ico");
            return false;
        }

        app_icon_path = app_icon_path.lexically_normal();
    }

    state.build_executable_name = game_name;
    state.build_folder_name = folder_name;
    state.build_window_title = window_title;
    state.build_output_root = output_root;
    state.build_app_icon_path = app_icon_path;
    state.SaveBuildSettingsToProject();

	EngineBuildRequest request;
	request.game_name = game_name;
	request.folder_name = folder_name;
	request.window_title = window_title;
	request.output_root = output_root;
	request.app_icon_path = app_icon_path;
	request.build_type = build_type_index_ == 0 ? EngineBuildType::Debug : EngineBuildType::Final;
	state.QueueBuildRequest(std::move(request));
	return true;
}
