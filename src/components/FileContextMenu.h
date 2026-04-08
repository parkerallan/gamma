#pragma once

#include "state/EngineState.h"

#include <array>
#include <filesystem>

class FileContextMenu
{
public:
    bool RenderItemMenu(const std::filesystem::path& target_path, bool is_directory, EngineState& state);
    bool Render(EngineState& state);

private:
    enum class PendingAction
    {
        None,
        Rename,
        Delete,
    };

    static constexpr const char* kRenamePopupName = "Rename Item";
    static constexpr const char* kDeletePopupName = "Delete Item";
    static constexpr std::size_t kNameCapacity = 160;

    std::filesystem::path clipboard_path_;
    std::filesystem::path action_target_path_;
    std::filesystem::path paste_target_directory_;
    std::array<char, kNameCapacity> name_buffer_{};
    bool action_target_is_directory_ = false;
    bool focus_name_input_ = false;
    bool open_rename_popup_ = false;
    bool open_delete_popup_ = false;

    bool HandleRename(EngineState& state);
    bool HandleDelete(EngineState& state);
    bool HandlePaste(const std::filesystem::path& destination_directory, EngineState& state);
    std::filesystem::path GetNextPastePath(const std::filesystem::path& destination_directory, const std::filesystem::path& source_path) const;
    void QueueRename(const std::filesystem::path& target_path, bool is_directory);
    void QueueDelete(const std::filesystem::path& target_path, bool is_directory);
    void ResetRename();
    void ResetDelete();
};