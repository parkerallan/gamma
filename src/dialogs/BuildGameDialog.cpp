#include "dialogs/BuildGameDialog.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ShObjIdl.h>
#include <combaseapi.h>
#include <windows.h>
#endif

namespace
{
#ifdef _WIN32
class ScopedComInitialization
{
public:
	ScopedComInitialization()
		: result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
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

std::filesystem::path ShowFolderDialog()
{
	ScopedComInitialization com;
	if (FAILED(com.Result()))
	{
		return {};
	}

	IFileDialog* dialog = nullptr;
	HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog));
	if (FAILED(result) || dialog == nullptr)
	{
		return {};
	}

	DWORD options = 0;
	result = dialog->GetOptions(&options);
	if (SUCCEEDED(result))
	{
		options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS;
		dialog->SetOptions(options);
		dialog->SetTitle(L"Choose Game Build Output Folder");
	}

	std::filesystem::path selected_path;
	result = dialog->Show(nullptr);
	if (SUCCEEDED(result))
	{
		IShellItem* shell_item = nullptr;
		result = dialog->GetResult(&shell_item);
		if (SUCCEEDED(result) && shell_item != nullptr)
		{
			PWSTR display_name = nullptr;
			result = shell_item->GetDisplayName(SIGDN_FILESYSPATH, &display_name);
			if (SUCCEEDED(result) && display_name != nullptr)
			{
				selected_path = display_name;
				CoTaskMemFree(display_name);
			}
			shell_item->Release();
		}
	}

	dialog->Release();
	return selected_path;
}
#endif

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

const char* GetBuildTypeLabel(int build_type_index)
{
	return build_type_index == 0 ? "Debug" : "Final";
}
}

void BuildGameDialog::Open(const EngineState& state)
{
	ResetFromState(state);
	ImGui::OpenPopup("Build Game");
}

bool BuildGameDialog::Render(EngineState& state)
{
	if (!ImGui::BeginPopupModal("Build Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		return false;
	}

	ImGui::TextUnformatted("Create a separate staged output folder for the packaged game.");
	ImGui::TextUnformatted("The selected folder will contain a new subfolder named after the game executable.");
	ImGui::Separator();

	ImGui::InputText("Game name", game_name_buffer_.data(), game_name_buffer_.size());

	ImGui::TextWrapped("Build location: %s", output_root_buffer_[0] == '\0' ? "None" : output_root_buffer_.data());
	if (ImGui::Button("Browse Output Folder"))
	{
		BrowseForOutputRoot(state);
	}

	const char* build_type_labels[] = {"Debug", "Final"};
	ImGui::Combo("Build type", &build_type_index_, build_type_labels, IM_ARRAYSIZE(build_type_labels));

	const std::string trimmed_game_name = TrimCopy(game_name_buffer_.data());
	std::filesystem::path stage_root;
	if (output_root_buffer_[0] != '\0' && !trimmed_game_name.empty())
	{
		stage_root = std::filesystem::path(output_root_buffer_.data()) / trimmed_game_name;
	}
	ImGui::Separator();
	ImGui::TextWrapped("Staged output folder: %s", stage_root.empty() ? "Pending game name and output folder" : stage_root.generic_string().c_str());
	ImGui::TextWrapped("Executable name: %s.exe", trimmed_game_name.empty() ? "<game name required>" : trimmed_game_name.c_str());
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

bool BuildGameDialog::BrowseForOutputRoot(EngineState& state)
{
#ifdef _WIN32
	const std::filesystem::path selected_path = ShowFolderDialog();
	if (selected_path.empty())
	{
		return false;
	}

	SetOutputRoot(selected_path);
	state.AddLog("Selected game build output folder: " + state.GetDisplayPath(selected_path));
	return true;
#else
	state.SetBuildError("Build output folder browsing is only implemented on Windows");
	return false;
#endif
}

bool BuildGameDialog::SubmitBuildRequest(EngineState& state)
{
	const std::string game_name = TrimCopy(game_name_buffer_.data());
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

	std::filesystem::path output_root(output_root_buffer_.data());
	if (output_root.empty())
	{
		state.SetBuildError("Cannot build game: choose a build output folder");
		return false;
	}

	if (output_root.is_relative() && !state.workspace_root.empty())
	{
		output_root = state.workspace_root / output_root;
	}
	output_root = output_root.lexically_normal();

	if (!std::filesystem::exists(output_root) || !std::filesystem::is_directory(output_root))
	{
		state.SetBuildError("Cannot build game: output folder does not exist");
		return false;
	}

	EngineBuildRequest request;
	request.game_name = game_name;
	request.output_root = output_root;
	request.build_type = build_type_index_ == 0 ? EngineBuildType::Debug : EngineBuildType::Final;
	state.QueueBuildRequest(std::move(request));
	return true;
}

void BuildGameDialog::ResetFromState(const EngineState& state)
{
	std::fill(game_name_buffer_.begin(), game_name_buffer_.end(), '\0');
	std::fill(output_root_buffer_.begin(), output_root_buffer_.end(), '\0');
	build_type_index_ = 0;

	std::string default_game_name;
	if (!state.pending_build_request.game_name.empty())
	{
		default_game_name = state.pending_build_request.game_name;
		build_type_index_ = state.pending_build_request.build_type == EngineBuildType::Debug ? 0 : 1;
		SetOutputRoot(state.pending_build_request.output_root);
	}
	else if (!state.project_file_path.empty())
	{
		default_game_name = state.project_file_path.stem().string();
	}
	else if (!state.project_root.empty())
	{
		default_game_name = state.project_root.filename().string();
	}

	if (default_game_name.empty())
	{
		default_game_name = "Game";
	}

	const std::size_t game_name_length = (std::min)(default_game_name.size(), game_name_buffer_.size() - 1);
	std::copy_n(default_game_name.begin(), static_cast<std::ptrdiff_t>(game_name_length), game_name_buffer_.begin());
}

void BuildGameDialog::SetOutputRoot(const std::filesystem::path& path)
{
	const std::string value = path.generic_string();
	std::fill(output_root_buffer_.begin(), output_root_buffer_.end(), '\0');
	const std::size_t length = (std::min)(value.size(), output_root_buffer_.size() - 1);
	std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(length), output_root_buffer_.begin());
}
