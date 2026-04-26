#pragma once

#include "state/EngineState.h"

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct GitFileChange
{
    std::string path;
    std::string status;
};

struct GitCommitEntry
{
    std::string short_hash;
    std::string subject;
    std::string author;
    std::string relative_time;
};

enum class VersionControlSubTab
{
    Changes,
    Settings,
};

class VersionControlPanel
{
public:
    void Render(EngineState& state);

private:
    VersionControlSubTab active_sub_tab_ = VersionControlSubTab::Changes;
    std::array<char, 512> commit_message_buffer_{};
    std::array<char, 512> remote_url_buffer_{};
    std::filesystem::path git_root_;
    std::vector<GitFileChange> changes_;
    std::vector<GitCommitEntry> commit_history_;
    std::unordered_map<std::string, bool> change_selection_;
    std::vector<std::string> branches_;
    std::vector<std::string> remotes_;
    std::string current_branch_;
    std::string status_message_;
    std::string undo_last_commit_disabled_reason_;
    bool has_repository_ = false;
    bool refresh_requested_ = true;
    bool can_undo_last_commit_ = false;
    double last_auto_refresh_time_ = 0.0;
    int selected_branch_index_ = -1;
    int selected_remote_index_ = -1;

    void RenderVersionControlTab(EngineState& state);
    void RenderChangesSubTab(EngineState& state);
    void RenderSettingsSubTab(EngineState& state);
    bool RefreshGitState(EngineState& state);
    bool RunGitCommand(const std::filesystem::path& git_root, const std::string& arguments, std::string& output, std::string* error_message = nullptr) const;
    bool CommitSelectedChanges(EngineState& state);
    bool RevertSingleChange(const GitFileChange& change, EngineState& state);
    bool RevertAllChanges(EngineState& state);
    bool CheckoutSelectedBranch(EngineState& state);
    bool SetRemote(EngineState& state);
    static std::string QuoteForShell(const std::string& value);
    static std::string DescribeGitStatusCode(const std::string& status_code);
    static std::string TrimCopy(std::string value);
    bool IsGitPathInProjectRoot(const std::filesystem::path& project_root) const;
};
