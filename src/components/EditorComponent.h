#pragma once

#include <filesystem>
#include <array>
#include <memory>
#include <string>

struct EngineState;
class TextEditor;

class EditorComponent
{
public:
    EditorComponent();
    ~EditorComponent();

    void Render(EngineState& state);

private:
    void LoadFile(EngineState& state);
    void ApplyLanguage(const std::filesystem::path& path);
    bool FindNextMatch();
    bool FindPreviousMatch();

    std::unique_ptr<TextEditor> editor_;
    std::filesystem::path loaded_path_;
    bool focus_editor_ = false;
    std::array<char, 256> search_query_{};
    std::string last_search_query_;
    std::size_t last_match_offset_ = std::string::npos;
    int last_search_direction_ = 0; // -1 previous, 1 next
};
