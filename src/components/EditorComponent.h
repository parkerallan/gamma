#pragma once

#include <filesystem>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct EngineState;
class TextEditor;

class EditorComponent
{
public:
    EditorComponent();
    ~EditorComponent();

    void Render(EngineState& state);

private:
    // One tab per open file. EngineState still carries a single "open file" —
    // scene editing keys off it — so the active tab is mirrored into it, and a
    // tab switch re-points state at that document.
    struct Document
    {
        std::filesystem::path path;
        std::unique_ptr<TextEditor> editor;
        // Last text known to be on disk, in EngineState's raw form. Used to
        // notice a reload performed outside this component.
        std::string saved_contents;
        bool dirty = false;
        std::array<char, 256> search_query{};
        std::string last_search_query;
        std::size_t last_match_offset = std::string::npos;
        int last_search_direction = 0; // -1 previous, 1 next
    };

    Document* FindDocument(const std::filesystem::path& path);
    void OpenDocument(EngineState& state);
    void ActivateDocument(Document& document, EngineState& state);
    void CloseDocument(std::size_t index, EngineState& state);
    void RenderDocument(Document& document, EngineState& state, bool is_active);
    static void ApplyLanguage(TextEditor& editor, const std::filesystem::path& path);
    static std::string NormalizeNewlines(const std::string& raw);
    static bool FindNextMatch(Document& document);
    static bool FindPreviousMatch(Document& document);

    std::vector<Document> documents_;
    std::filesystem::path active_path_;
    // ImGui applies a queued tab focus on the following frame, so the tab bar
    // shows the previous tab for one frame after a file is opened. Tracked so
    // that frame doesn't sync the wrong document against EngineState.
    std::filesystem::path pending_focus_path_;
};
