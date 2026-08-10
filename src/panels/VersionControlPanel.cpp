#include "panels/VersionControlPanel.h"

#include "imgui.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <system_error>

namespace
{
#ifdef _WIN32
#define ENGINE_POPEN _popen
#define ENGINE_PCLOSE _pclose
#else
#define ENGINE_POPEN popen
#define ENGINE_PCLOSE pclose
#endif
}

void VersionControlPanel::Render(EngineState& state)
{
    if (!state.show_version_control_panel)
    {
        return;
    }

    if (!ImGui::Begin("Version Control", &state.show_version_control_panel))
    {
        ImGui::End();
        return;
    }

    if (state.workspace_root.empty())
    {
        ImGui::TextUnformatted("Workspace root not resolved.");
        ImGui::End();
        return;
    }

    if (!state.HasOpenProject())
    {
        ImGui::TextUnformatted("No project loaded.");
        ImGui::TextWrapped("Open or create a project to use version control.");
        if (ImGui::Button("Open Project##VersionControl"))
        {
            state.request_open_project_dialog = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("New Project##VersionControl"))
        {
            state.request_new_project_dialog = true;
        }

        ImGui::End();
        return;
    }

    RenderVersionControlTab(state);

    ImGui::End();
}

void VersionControlPanel::RenderVersionControlTab(EngineState& state)
{
    if (git_root_ != state.project_root)
    {
        git_root_ = state.project_root;
        refresh_requested_ = true;
        last_auto_refresh_time_ = 0.0;
        change_selection_.clear();
        commit_message_buffer_.fill('\0');
        remote_url_buffer_.fill('\0');
        const std::size_t copy_length = (std::min)(state.version_control_remote_url.size(), remote_url_buffer_.size() - 1);
        std::copy_n(state.version_control_remote_url.begin(), static_cast<std::ptrdiff_t>(copy_length), remote_url_buffer_.begin());
    }

    const double now = ImGui::GetTime();
    const bool auto_refresh_due = (now - last_auto_refresh_time_) >= 1.0;
    if (refresh_requested_ || auto_refresh_due)
    {
        RefreshGitState(state);
        last_auto_refresh_time_ = now;
    }

    if (ImGui::BeginTabBar("##VersionControlSubTabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Changes"))
        {
            active_sub_tab_ = VersionControlSubTab::Changes;
            RenderChangesSubTab(state);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings"))
        {
            active_sub_tab_ = VersionControlSubTab::Settings;
            RenderSettingsSubTab(state);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void VersionControlPanel::RenderChangesSubTab(EngineState& state)
{
    if (!has_repository_)
    {
        ImGui::TextUnformatted("No git repository found in this project folder.");
        ImGui::TextWrapped("Version Control only targets the opened project root (.git in the project folder), not the engine workspace.");
        if (ImGui::Button("Initialize Git Here"))
        {
            std::string output;
            if (RunGitCommand(state.project_root, "init", output, &status_message_))
            {
                state.AddLog("Initialized git repository in project root: " + state.GetDisplayPath(state.project_root));
                refresh_requested_ = true;
            }
            else
            {
                state.AddError("Failed to initialize git repository: " + status_message_);
            }
        }
        if (!status_message_.empty())
        {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", status_message_.c_str());
        }
        return;
    }

    if (!current_branch_.empty())
    {
        ImGui::TextDisabled("Branch: %s", current_branch_.c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Changed Files");

    if (changes_.empty())
    {
        ImGui::TextDisabled("Working tree clean.");
    }
    else
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float discard_button_width = ImGui::CalcTextSize(ICON_CI_DISCARD).x + style.FramePadding.x * 2.0f;
        const float discard_button_x = ImGui::GetWindowContentRegionMax().x - discard_button_width;

        bool all_selected = true;
        for (const GitFileChange& change : changes_)
        {
            const auto selection_it = change_selection_.find(change.path);
            if (selection_it == change_selection_.end() || !selection_it->second)
            {
                all_selected = false;
                break;
            }
        }

        bool toggle_all = all_selected;
        if (ImGui::Checkbox("Check All", &toggle_all))
        {
            for (const GitFileChange& change : changes_)
            {
                change_selection_[change.path] = toggle_all;
            }
        }
        ImGui::SameLine();
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), discard_button_x));
        if (ImGui::Button(ICON_CI_DISCARD "##RevertAllChanges"))
        {
            if (RevertAllChanges(state))
            {
                refresh_requested_ = true;
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Revert all changes");
        }

        ImGui::Separator();
        for (const GitFileChange& change : changes_)
        {
            bool selected = change_selection_[change.path];
            ImGui::PushID(change.path.c_str());
            if (ImGui::Checkbox("##IncludeChange", &selected))
            {
                change_selection_[change.path] = selected;
            }
            ImGui::SameLine();
            ImGui::Text("[%s]", DescribeGitStatusCode(change.status).c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(change.path.c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), discard_button_x));
            if (ImGui::Button(ICON_CI_DISCARD "##RevertChange"))
            {
                if (RevertSingleChange(change, state))
                {
                    refresh_requested_ = true;
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Revert this change");
            }
            ImGui::PopID();
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Commit Message");
    ImGui::InputTextMultiline(
        "##CommitMessage",
        commit_message_buffer_.data(),
        commit_message_buffer_.size(),
        ImVec2(-FLT_MIN, ImGui::GetTextLineHeightWithSpacing() * 4.5f));

    bool has_selected_changes = false;
    for (const GitFileChange& change : changes_)
    {
        if (change_selection_[change.path])
        {
            has_selected_changes = true;
            break;
        }
    }

    const bool has_commit_message = !TrimCopy(commit_message_buffer_.data()).empty();
    const bool can_commit = has_selected_changes && has_commit_message;
    if (!can_commit)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Commit"))
    {
        if (CommitSelectedChanges(state))
        {
            commit_message_buffer_.fill('\0');
            refresh_requested_ = true;
        }
    }
    if (!can_commit)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_CI_SYNC))
    {
        refresh_requested_ = true;
    }

    ImGui::SameLine();
    if (ImGui::Button(ICON_CI_KEBAB_VERTICAL))
    {
        ImGui::OpenPopup("##VersionControlActions");
    }
    if (ImGui::BeginPopup("##VersionControlActions"))
    {
        if (ImGui::MenuItem("Pull"))
        {
            std::string output;
            if (RunGitCommand(git_root_, "pull --rebase", output, &status_message_))
            {
                state.AddLog("Git pull completed for project: " + state.GetDisplayPath(git_root_));
                refresh_requested_ = true;
            }
            else
            {
                state.AddError("Git pull failed: " + status_message_);
            }
        }

        if (ImGui::MenuItem("Push"))
        {
            std::string output;
            if (RunGitCommand(git_root_, "push", output, &status_message_))
            {
                state.AddLog("Git push completed for project: " + state.GetDisplayPath(git_root_));
                refresh_requested_ = true;
            }
            else
            {
                const bool missing_upstream = status_message_.find("has no upstream branch") != std::string::npos;
                if (missing_upstream && !current_branch_.empty())
                {
                    const std::string upstream_push = "push --set-upstream origin " + QuoteForShell(current_branch_);
                    if (RunGitCommand(git_root_, upstream_push, output, &status_message_))
                    {
                        state.AddLog("Git push completed and upstream set for branch: " + current_branch_);
                        refresh_requested_ = true;
                    }
                    else
                    {
                        state.AddError("Git push failed: " + status_message_);
                    }
                }
                else
                {
                    state.AddError("Git push failed: " + status_message_);
                }
            }
        }

        if (ImGui::MenuItem("Fetch"))
        {
            std::string output;
            if (RunGitCommand(git_root_, "fetch --all --prune", output, &status_message_))
            {
                state.AddLog("Fetched remotes for project: " + state.GetDisplayPath(git_root_));
                refresh_requested_ = true;
            }
            else
            {
                state.AddError("Git fetch failed: " + status_message_);
            }
        }

        if (ImGui::MenuItem("Undo Last Commit", nullptr, false, can_undo_last_commit_))
        {
            std::string output;
            if (RunGitCommand(git_root_, "reset --soft HEAD~1", output, &status_message_))
            {
                state.AddLog("Undid last commit (soft reset) for project: " + state.GetDisplayPath(git_root_));
                refresh_requested_ = true;
            }
            else
            {
                state.AddError("Failed to undo last commit: " + status_message_);
            }
        }
        else if (!can_undo_last_commit_ && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", undo_last_commit_disabled_reason_.empty() ? "Undo is currently unavailable." : undo_last_commit_disabled_reason_.c_str());
        }

        ImGui::EndPopup();
    }

    if (!status_message_.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", status_message_.c_str());
    }
}

void VersionControlPanel::RenderSettingsSubTab(EngineState& state)
{
    if (!has_repository_)
    {
        ImGui::TextUnformatted("Repository Setup");
        ImGui::TextWrapped("This project folder is not a git repository yet.");
        if (ImGui::Button("Initialize Git Here##Settings"))
        {
            std::string output;
            if (RunGitCommand(state.project_root, "init", output, &status_message_))
            {
                state.AddLog("Initialized git repository in project root: " + state.GetDisplayPath(state.project_root));
                refresh_requested_ = true;
            }
            else
            {
                state.AddError("Failed to initialize git repository: " + status_message_);
            }
        }

        if (!status_message_.empty())
        {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", status_message_.c_str());
        }
        return;
    }

    ImGui::TextUnformatted("Remote");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##RemoteUrl", "Remote URL", remote_url_buffer_.data(), remote_url_buffer_.size());

    if (ImGui::Button("Set Remote"))
    {
        if (SetRemote(state))
        {
            refresh_requested_ = true;
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Branch");
    if (!branches_.empty())
    {
        const char* current_preview = selected_branch_index_ >= 0 && selected_branch_index_ < static_cast<int>(branches_.size())
            ? branches_[static_cast<std::size_t>(selected_branch_index_)].c_str()
            : "Select branch";
        if (ImGui::BeginCombo("##BranchSelect", current_preview))
        {
            for (std::size_t index = 0; index < branches_.size(); ++index)
            {
                const bool selected = static_cast<int>(index) == selected_branch_index_;
                if (ImGui::Selectable(branches_[index].c_str(), selected))
                {
                    selected_branch_index_ = static_cast<int>(index);
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }
    else
    {
        ImGui::TextDisabled("No local branches found.");
    }

    const bool can_switch_branch = selected_branch_index_ >= 0;
    if (!can_switch_branch)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Switch Branch"))
    {
        if (CheckoutSelectedBranch(state))
        {
            refresh_requested_ = true;
        }
    }
    if (!can_switch_branch)
    {
        ImGui::EndDisabled();
    }

    if (!status_message_.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", status_message_.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Commit History");
    if (commit_history_.empty())
    {
        ImGui::TextDisabled("No commits found in this repository.");
    }
    else if (ImGui::BeginChild("##CommitHistoryList", ImVec2(0.0f, 180.0f), true))
    {
        for (const GitCommitEntry& commit : commit_history_)
        {
            ImGui::PushID(commit.short_hash.c_str());
            ImGui::Text("%s  %s", commit.short_hash.c_str(), commit.subject.c_str());
            if (!commit.author.empty() || !commit.relative_time.empty())
            {
                std::string meta;
                if (!commit.author.empty())
                {
                    meta += commit.author;
                }
                if (!commit.relative_time.empty())
                {
                    if (!meta.empty())
                    {
                        meta += " - ";
                    }
                    meta += commit.relative_time;
                }
                ImGui::TextDisabled("%s", meta.c_str());
            }
            ImGui::Separator();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
}

bool VersionControlPanel::RefreshGitState(EngineState& state)
{
    refresh_requested_ = false;
    status_message_.clear();
    has_repository_ = IsGitPathInProjectRoot(state.project_root);

    if (!has_repository_)
    {
        changes_.clear();
        commit_history_.clear();
        branches_.clear();
        remotes_.clear();
        current_branch_.clear();
        can_undo_last_commit_ = false;
        undo_last_commit_disabled_reason_ = "Open a git repository first.";
        selected_branch_index_ = -1;
        selected_remote_index_ = -1;
        return false;
    }

    std::string status_output;
    std::string status_error;
    if (!RunGitCommand(state.project_root, "status --porcelain -uall", status_output, &status_error))
    {
        status_message_ = status_error.empty() ? "Failed to read git status." : status_error;
        return false;
    }

    std::vector<GitFileChange> changes;
    std::istringstream status_stream(status_output);
    std::string line;
    while (std::getline(status_stream, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
        {
            line.pop_back();
        }

        if (line.size() < 3)
        {
            continue;
        }

        GitFileChange change;
        change.status = line.substr(0, 2);
        const std::size_t path_offset = (line.size() > 2 && line[2] == ' ') ? 3 : 2;
        if (line.size() <= path_offset)
        {
            continue;
        }
        change.path = TrimCopy(line.substr(path_offset));

        const std::size_t rename_arrow = change.path.find(" -> ");
        if (rename_arrow != std::string::npos)
        {
            change.path = change.path.substr(rename_arrow + 4);
        }

        if (!change.path.empty())
        {
            changes.push_back(std::move(change));
        }
    }

    std::sort(changes.begin(), changes.end(), [](const GitFileChange& left, const GitFileChange& right)
    {
        return left.path < right.path;
    });

    std::unordered_map<std::string, bool> new_selection;
    for (const GitFileChange& change : changes)
    {
        const auto selection_it = change_selection_.find(change.path);
        new_selection[change.path] = selection_it != change_selection_.end() ? selection_it->second : true;
    }

    changes_ = std::move(changes);
    change_selection_ = std::move(new_selection);

    std::string current_branch_output;
    if (RunGitCommand(state.project_root, "rev-parse --abbrev-ref HEAD", current_branch_output, nullptr))
    {
        current_branch_ = TrimCopy(std::move(current_branch_output));
    }
    else
    {
        current_branch_.clear();
    }

    std::string previous_selected_branch;
    if (selected_branch_index_ >= 0 && selected_branch_index_ < static_cast<int>(branches_.size()))
    {
        previous_selected_branch = branches_[static_cast<std::size_t>(selected_branch_index_)];
    }

    branches_.clear();
    std::string branches_output;
    if (RunGitCommand(state.project_root, "branch --list --format=\"%(refname:short)\"", branches_output, nullptr))
    {
        std::istringstream branch_stream(branches_output);
        while (std::getline(branch_stream, line))
        {
            std::string branch = TrimCopy(line);
            if (!branch.empty() && branch != "origin")
            {
                branches_.push_back(std::move(branch));
            }
        }
    }

    std::string remote_branches_output;
    if (RunGitCommand(state.project_root, "branch --remotes --format=\"%(refname:short)\"", remote_branches_output, nullptr))
    {
        std::istringstream branch_stream(remote_branches_output);
        while (std::getline(branch_stream, line))
        {
            std::string branch = TrimCopy(line);
            if (branch.empty() || branch.find("HEAD") != std::string::npos)
            {
                continue;
            }

            if (branch.rfind("origin/", 0) == 0)
            {
                branch = branch.substr(std::strlen("origin/"));
            }

            if (branch.empty() || branch == "origin")
            {
                continue;
            }

            if (std::find(branches_.begin(), branches_.end(), branch) == branches_.end())
            {
                branches_.push_back(std::move(branch));
            }
        }
    }

    selected_branch_index_ = -1;
    if (!previous_selected_branch.empty())
    {
        for (std::size_t index = 0; index < branches_.size(); ++index)
        {
            if (branches_[index] == previous_selected_branch)
            {
                selected_branch_index_ = static_cast<int>(index);
                break;
            }
        }
    }

    if (selected_branch_index_ < 0)
    {
        for (std::size_t index = 0; index < branches_.size(); ++index)
        {
            if (branches_[index] == current_branch_)
            {
                selected_branch_index_ = static_cast<int>(index);
                break;
            }
        }
    }

    remotes_.clear();
    std::string remotes_output;
    if (RunGitCommand(state.project_root, "remote", remotes_output, nullptr))
    {
        std::istringstream remotes_stream(remotes_output);
        while (std::getline(remotes_stream, line))
        {
            std::string remote = TrimCopy(line);
            if (!remote.empty())
            {
                remotes_.push_back(std::move(remote));
            }
        }
    }

    selected_remote_index_ = -1;
    if (!remotes_.empty())
    {
        selected_remote_index_ = 0;
    }

    commit_history_.clear();
    std::string log_output;
    if (RunGitCommand(state.project_root, "log --format=\"%h%x09%s%x09%an%x09%cr\" -10", log_output, nullptr))
    {
        std::istringstream log_stream(log_output);
        while (std::getline(log_stream, line))
        {
            if (line.empty())
            {
                continue;
            }

            GitCommitEntry entry;
            std::istringstream line_stream(line);
            std::getline(line_stream, entry.short_hash, '\t');
            std::getline(line_stream, entry.subject, '\t');
            std::getline(line_stream, entry.author, '\t');
            std::getline(line_stream, entry.relative_time);

            entry.short_hash = TrimCopy(std::move(entry.short_hash));
            entry.subject = TrimCopy(std::move(entry.subject));
            entry.author = TrimCopy(std::move(entry.author));
            entry.relative_time = TrimCopy(std::move(entry.relative_time));

            if (!entry.short_hash.empty())
            {
                commit_history_.push_back(std::move(entry));
            }
        }
    }

    can_undo_last_commit_ = false;
    undo_last_commit_disabled_reason_ = "Undo requires at least two commits in history.";
    if (commit_history_.size() > 1)
    {
        std::string upstream_output;
        if (!RunGitCommand(state.project_root, "rev-parse --abbrev-ref --symbolic-full-name @{u}", upstream_output, nullptr))
        {
            // No upstream configured means commits are local-only and safe to undo.
            can_undo_last_commit_ = true;
            undo_last_commit_disabled_reason_.clear();
        }
        else
        {
            std::string ahead_output;
            if (RunGitCommand(state.project_root, "rev-list --count @{u}..HEAD", ahead_output, nullptr))
            {
                int ahead_count = 0;
                try
                {
                    ahead_count = std::stoi(TrimCopy(ahead_output));
                }
                catch (...)
                {
                    ahead_count = 0;
                }

                if (ahead_count > 0)
                {
                    can_undo_last_commit_ = true;
                    undo_last_commit_disabled_reason_.clear();
                }
                else
                {
                    undo_last_commit_disabled_reason_ = "Latest commit is already pushed to upstream.";
                }
            }
            else
            {
                undo_last_commit_disabled_reason_ = "Unable to determine upstream sync state.";
            }
        }
    }

    return true;
}

bool VersionControlPanel::RunGitCommand(const std::filesystem::path& git_root, const std::string& arguments, std::string& output, std::string* error_message) const
{
    output.clear();
    std::error_code error;
    const std::filesystem::path previous_directory = std::filesystem::current_path(error);
    if (error)
    {
        if (error_message != nullptr)
        {
            *error_message = "Unable to read current working directory.";
        }
        return false;
    }

    std::filesystem::current_path(git_root, error);
    if (error)
    {
        if (error_message != nullptr)
        {
            *error_message = "Unable to access project git root.";
        }
        return false;
    }

    const std::string command = "git " + arguments + " 2>&1";
    FILE* pipe = ENGINE_POPEN(command.c_str(), "r");
    if (pipe == nullptr)
    {
        std::filesystem::current_path(previous_directory, error);
        if (error_message != nullptr)
        {
            *error_message = "Failed to launch git command.";
        }
        return false;
    }

    char buffer[512];
    while (std::fgets(buffer, static_cast<int>(sizeof(buffer)), pipe) != nullptr)
    {
        output.append(buffer);
    }

    const int exit_code = ENGINE_PCLOSE(pipe);
    std::filesystem::current_path(previous_directory, error);

    if (exit_code != 0)
    {
        if (error_message != nullptr)
        {
            const std::string trimmed_output = TrimCopy(output);
            *error_message = trimmed_output.empty() ? "Git command failed." : trimmed_output;
        }
        return false;
    }

    if (error_message != nullptr)
    {
        *error_message = TrimCopy(output);
    }

    return true;
}

bool VersionControlPanel::CommitSelectedChanges(EngineState& state)
{
    std::vector<std::string> selected_paths;
    selected_paths.reserve(changes_.size());
    for (const GitFileChange& change : changes_)
    {
        if (change_selection_[change.path])
        {
            selected_paths.push_back(change.path);
        }
    }

    if (selected_paths.empty())
    {
        status_message_ = "No files selected for commit.";
        return false;
    }

    std::string add_arguments = "add --";
    for (const std::string& path : selected_paths)
    {
        add_arguments += " " + QuoteForShell(path);
    }

    std::string output;
    if (!RunGitCommand(git_root_, add_arguments, output, &status_message_))
    {
        state.AddError("Failed to stage selected files: " + status_message_);
        return false;
    }

    const std::string commit_message = TrimCopy(commit_message_buffer_.data());
    if (commit_message.empty())
    {
        status_message_ = "Commit message is required.";
        return false;
    }

    const std::string commit_arguments = "commit -m " + QuoteForShell(commit_message);
    if (!RunGitCommand(git_root_, commit_arguments, output, &status_message_))
    {
        state.AddError("Git commit failed: " + status_message_);
        return false;
    }

    state.AddLog("Committed selected changes in project: " + state.GetDisplayPath(git_root_));
    return true;
}

bool VersionControlPanel::RevertSingleChange(const GitFileChange& change, EngineState& state)
{
    std::string output;
    const bool is_untracked = change.status == "??";
    bool ok = false;

    if (is_untracked)
    {
        ok = RunGitCommand(git_root_, "clean -f -- " + QuoteForShell(change.path), output, &status_message_);
    }
    else
    {
        ok = RunGitCommand(git_root_, "restore --staged --worktree -- " + QuoteForShell(change.path), output, &status_message_);
        if (!ok)
        {
            ok = RunGitCommand(git_root_, "checkout -- " + QuoteForShell(change.path), output, &status_message_);
        }
    }

    if (!ok)
    {
        state.AddError("Failed to revert change: " + status_message_);
        return false;
    }

    state.AddLog("Reverted change: " + change.path);
    return true;
}

bool VersionControlPanel::RevertAllChanges(EngineState& state)
{
    bool any_failures = false;
    for (const GitFileChange& change : changes_)
    {
        if (!RevertSingleChange(change, state))
        {
            any_failures = true;
        }
    }

    if (any_failures)
    {
        status_message_ = "Some changes could not be reverted. See log.";
        return false;
    }

    status_message_ = "Reverted all changes.";
    return true;
}

bool VersionControlPanel::CheckoutSelectedBranch(EngineState& state)
{
    if (selected_branch_index_ < 0 || selected_branch_index_ >= static_cast<int>(branches_.size()))
    {
        status_message_ = "Select a branch to switch.";
        return false;
    }

    const std::string branch_name = branches_[static_cast<std::size_t>(selected_branch_index_)];
    std::string output;

    if (!RunGitCommand(git_root_, "checkout " + QuoteForShell(branch_name), output, &status_message_))
    {
        const bool has_origin_remote = std::find(remotes_.begin(), remotes_.end(), "origin") != remotes_.end();
        if (has_origin_remote)
        {
            const std::string tracking_ref = "origin/" + branch_name;
            if (RunGitCommand(git_root_, "checkout --track " + QuoteForShell(tracking_ref), output, &status_message_))
            {
                state.AddLog("Created and switched to tracking branch: " + branch_name);
                return true;
            }
        }

        state.AddError("Failed to switch branch: " + status_message_);
        return false;
    }

    state.AddLog("Switched branch to " + branch_name);
    return true;
}

bool VersionControlPanel::SetRemote(EngineState& state)
{
    std::string remote_url = TrimCopy(remote_url_buffer_.data());
    const std::string remote_name = "origin";

    if (remote_url.empty())
    {
        status_message_ = "Remote URL is required.";
        return false;
    }

    std::string output;
    const bool remote_exists = std::find(remotes_.begin(), remotes_.end(), remote_name) != remotes_.end();
    const std::string arguments = remote_exists
        ? "remote set-url " + QuoteForShell(remote_name) + " " + QuoteForShell(remote_url)
        : "remote add " + QuoteForShell(remote_name) + " " + QuoteForShell(remote_url);

    if (!RunGitCommand(git_root_, arguments, output, &status_message_))
    {
        state.AddError("Failed to set remote: " + status_message_);
        return false;
    }

    state.version_control_remote_url = remote_url;
    if (!state.SaveProjectSettings())
    {
        state.AddWarning("Saved remote in git, but failed to persist settings.ini");
    }

    state.AddLog(std::string(remote_exists ? "Updated remote '" : "Added remote '") + remote_name + "'");
    return true;
}

std::string VersionControlPanel::QuoteForShell(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value)
    {
        if (character == '"')
        {
            escaped += '\\';
        }
        escaped += character;
    }

    return "\"" + escaped + "\"";
}

std::string VersionControlPanel::DescribeGitStatusCode(const std::string& status_code)
{
    if (status_code == "??")
    {
        return "Untracked";
    }

    if (status_code.size() != 2)
    {
        return status_code;
    }

    const char index_status = status_code[0];
    const char worktree_status = status_code[1];
    if (index_status == 'A' || worktree_status == 'A')
    {
        return "Added";
    }
    if (index_status == 'M' || worktree_status == 'M')
    {
        return "Modified";
    }
    if (index_status == 'D' || worktree_status == 'D')
    {
        return "Deleted";
    }
    if (index_status == 'R' || worktree_status == 'R')
    {
        return "Renamed";
    }
    if (index_status == 'C' || worktree_status == 'C')
    {
        return "Copied";
    }
    if (index_status == 'U' || worktree_status == 'U')
    {
        return "Unmerged";
    }

    return TrimCopy(status_code);
}

std::string VersionControlPanel::TrimCopy(std::string value)
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

bool VersionControlPanel::IsGitPathInProjectRoot(const std::filesystem::path& project_root) const
{
    if (project_root.empty())
    {
        return false;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(project_root / ".git", error);
    return !error && exists;
}
