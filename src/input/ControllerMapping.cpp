#include "input/ControllerMapping.h"

#include <cctype>
#include <sstream>

namespace input
{
namespace
{

std::string_view Trim(std::string_view value)
{
    const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
    std::size_t begin = 0;
    while (begin < value.size() && !not_space(static_cast<unsigned char>(value[begin])))
    {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && !not_space(static_cast<unsigned char>(value[end - 1])))
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

} // namespace

const std::vector<KeyDef>& StandardKeys()
{
    // Order and labels mirror the Mapping panel table. Scancodes are explicit
    // because labels like "Numpad 0" or "Tilde (`)" do not round-trip through
    // SDL_GetScancodeFromName.
    static const std::vector<KeyDef> keys = {
        // Letters
        {"A", SDL_SCANCODE_A}, {"B", SDL_SCANCODE_B}, {"C", SDL_SCANCODE_C}, {"D", SDL_SCANCODE_D},
        {"E", SDL_SCANCODE_E}, {"F", SDL_SCANCODE_F}, {"G", SDL_SCANCODE_G}, {"H", SDL_SCANCODE_H},
        {"I", SDL_SCANCODE_I}, {"J", SDL_SCANCODE_J}, {"K", SDL_SCANCODE_K}, {"L", SDL_SCANCODE_L},
        {"M", SDL_SCANCODE_M}, {"N", SDL_SCANCODE_N}, {"O", SDL_SCANCODE_O}, {"P", SDL_SCANCODE_P},
        {"Q", SDL_SCANCODE_Q}, {"R", SDL_SCANCODE_R}, {"S", SDL_SCANCODE_S}, {"T", SDL_SCANCODE_T},
        {"U", SDL_SCANCODE_U}, {"V", SDL_SCANCODE_V}, {"W", SDL_SCANCODE_W}, {"X", SDL_SCANCODE_X},
        {"Y", SDL_SCANCODE_Y}, {"Z", SDL_SCANCODE_Z},
        // Digits
        {"0", SDL_SCANCODE_0}, {"1", SDL_SCANCODE_1}, {"2", SDL_SCANCODE_2}, {"3", SDL_SCANCODE_3},
        {"4", SDL_SCANCODE_4}, {"5", SDL_SCANCODE_5}, {"6", SDL_SCANCODE_6}, {"7", SDL_SCANCODE_7},
        {"8", SDL_SCANCODE_8}, {"9", SDL_SCANCODE_9},
        // Function keys
        {"F1", SDL_SCANCODE_F1}, {"F2", SDL_SCANCODE_F2}, {"F3", SDL_SCANCODE_F3},
        {"F4", SDL_SCANCODE_F4}, {"F5", SDL_SCANCODE_F5}, {"F6", SDL_SCANCODE_F6},
        {"F7", SDL_SCANCODE_F7}, {"F8", SDL_SCANCODE_F8}, {"F9", SDL_SCANCODE_F9},
        {"F10", SDL_SCANCODE_F10}, {"F11", SDL_SCANCODE_F11}, {"F12", SDL_SCANCODE_F12},
        // Navigation / editing
        {"Escape", SDL_SCANCODE_ESCAPE}, {"Tab", SDL_SCANCODE_TAB}, {"Caps Lock", SDL_SCANCODE_CAPSLOCK},
        {"Left Shift", SDL_SCANCODE_LSHIFT}, {"Right Shift", SDL_SCANCODE_RSHIFT},
        {"Left Ctrl", SDL_SCANCODE_LCTRL}, {"Right Ctrl", SDL_SCANCODE_RCTRL},
        {"Left Alt", SDL_SCANCODE_LALT}, {"Right Alt", SDL_SCANCODE_RALT},
        {"Space", SDL_SCANCODE_SPACE}, {"Enter", SDL_SCANCODE_RETURN}, {"Backspace", SDL_SCANCODE_BACKSPACE},
        {"Delete", SDL_SCANCODE_DELETE}, {"Insert", SDL_SCANCODE_INSERT},
        {"Home", SDL_SCANCODE_HOME}, {"End", SDL_SCANCODE_END},
        {"Page Up", SDL_SCANCODE_PAGEUP}, {"Page Down", SDL_SCANCODE_PAGEDOWN},
        {"Print Screen", SDL_SCANCODE_PRINTSCREEN}, {"Scroll Lock", SDL_SCANCODE_SCROLLLOCK},
        {"Pause", SDL_SCANCODE_PAUSE},
        // Arrow keys
        {"Up", SDL_SCANCODE_UP}, {"Down", SDL_SCANCODE_DOWN},
        {"Left", SDL_SCANCODE_LEFT}, {"Right", SDL_SCANCODE_RIGHT},
        // Numpad
        {"Numpad 0", SDL_SCANCODE_KP_0}, {"Numpad 1", SDL_SCANCODE_KP_1}, {"Numpad 2", SDL_SCANCODE_KP_2},
        {"Numpad 3", SDL_SCANCODE_KP_3}, {"Numpad 4", SDL_SCANCODE_KP_4}, {"Numpad 5", SDL_SCANCODE_KP_5},
        {"Numpad 6", SDL_SCANCODE_KP_6}, {"Numpad 7", SDL_SCANCODE_KP_7}, {"Numpad 8", SDL_SCANCODE_KP_8},
        {"Numpad 9", SDL_SCANCODE_KP_9},
        {"Numpad +", SDL_SCANCODE_KP_PLUS}, {"Numpad -", SDL_SCANCODE_KP_MINUS},
        {"Numpad *", SDL_SCANCODE_KP_MULTIPLY}, {"Numpad /", SDL_SCANCODE_KP_DIVIDE},
        {"Numpad .", SDL_SCANCODE_KP_PERIOD}, {"Numpad Enter", SDL_SCANCODE_KP_ENTER},
        // Punctuation / symbols
        {"Tilde (`)", SDL_SCANCODE_GRAVE}, {"Minus (-)", SDL_SCANCODE_MINUS}, {"Equals (=)", SDL_SCANCODE_EQUALS},
        {"Left Bracket ([)", SDL_SCANCODE_LEFTBRACKET}, {"Right Bracket (])", SDL_SCANCODE_RIGHTBRACKET},
        {"Backslash (\\)", SDL_SCANCODE_BACKSLASH},
        {"Semicolon (;)", SDL_SCANCODE_SEMICOLON}, {"Apostrophe (')", SDL_SCANCODE_APOSTROPHE},
        {"Comma (,)", SDL_SCANCODE_COMMA}, {"Period (.)", SDL_SCANCODE_PERIOD}, {"Slash (/)", SDL_SCANCODE_SLASH},
        // Mouse movement (drives the synthetic delta from Input.MouseDelta()).
        {"Mouse Move Right", kMouseMoveRight}, {"Mouse Move Left", kMouseMoveLeft},
        {"Mouse Move Up", kMouseMoveUp}, {"Mouse Move Down", kMouseMoveDown},
    };
    return keys;
}

bool ParseBindingToken(std::string_view token, ControllerBinding& out)
{
    const std::string_view trimmed = Trim(token);
    if (trimmed.empty())
    {
        return false;
    }

    // Axis tokens carry a trailing direction sign, e.g. "leftx+", "lefttrigger+".
    const char last = trimmed.back();
    if (last == '+' || last == '-')
    {
        const std::string base(Trim(trimmed.substr(0, trimmed.size() - 1)));
        const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(base.c_str());
        if (axis != SDL_GAMEPAD_AXIS_INVALID)
        {
            out.kind     = ControllerBinding::Kind::Axis;
            out.index    = static_cast<int>(axis);
            out.positive = (last == '+');
            return true;
        }
        return false;
    }

    const std::string name(trimmed);
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID)
    {
        out.kind  = ControllerBinding::Kind::Button;
        out.index = static_cast<int>(button);
        return true;
    }

    // Fall back to treating an un-suffixed token as a positive axis.
    const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(name.c_str());
    if (axis != SDL_GAMEPAD_AXIS_INVALID)
    {
        out.kind     = ControllerBinding::Kind::Axis;
        out.index    = static_cast<int>(axis);
        out.positive = true;
        return true;
    }

    return false;
}

std::vector<ControllerBinding> ParseBindingList(std::string_view csv)
{
    std::vector<ControllerBinding> bindings;
    std::size_t start = 0;
    while (start <= csv.size())
    {
        const std::size_t comma = csv.find(',', start);
        const std::size_t end = (comma == std::string_view::npos) ? csv.size() : comma;
        ControllerBinding binding;
        if (ParseBindingToken(csv.substr(start, end - start), binding))
        {
            bindings.push_back(binding);
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    return bindings;
}

std::string SerializeMappings(
    const std::vector<std::string>& standard_rows,
    const std::vector<std::pair<std::string, std::string>>& custom_rows)
{
    // Keyed by SDL scancode (an int, which never collides with the '=' field
    // separator and is stable across SDL versions). A trailing comment records
    // the human-readable key for anyone inspecting the file.
    std::ostringstream out;
    out << "# Controller input mappings - auto-saved by the editor. Do not edit manually.\n";
    out << "# Format: <sdl_scancode>=<controller inputs>   # <key>\n";

    const std::vector<KeyDef>& keys = StandardKeys();
    for (std::size_t i = 0; i < standard_rows.size() && i < keys.size(); ++i)
    {
        const std::string_view value = Trim(standard_rows[i]);
        if (value.empty())
        {
            continue;
        }
        out << keys[i].code << '=' << value
            << "   # " << keys[i].label << '\n';
    }

    for (const auto& [key_name, controller_csv] : custom_rows)
    {
        const std::string_view value = Trim(controller_csv);
        const std::string trimmed_key(Trim(key_name));
        if (value.empty() || trimmed_key.empty())
        {
            continue;
        }
        const SDL_Scancode scancode = SDL_GetScancodeFromName(trimmed_key.c_str());
        if (scancode == SDL_SCANCODE_UNKNOWN)
        {
            continue;
        }
        out << static_cast<int>(scancode) << '=' << value
            << "   # " << trimmed_key << '\n';
    }

    return out.str();
}

namespace
{

// Pull one "<scancode>=<value>" record out of a line, stripping any trailing
// "# comment". Returns false for blank/comment lines or malformed records.
bool ParseLine(std::string_view line, int& out_scancode, std::string_view& out_value)
{
    line = Trim(line);
    if (line.empty() || line.front() == '#')
    {
        return false;
    }

    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos)
    {
        return false;
    }

    const std::string key(Trim(line.substr(0, eq)));
    if (key.empty())
    {
        return false;
    }
    try
    {
        out_scancode = std::stoi(key);
    }
    catch (...)
    {
        return false;
    }

    std::string_view value = line.substr(eq + 1);
    const std::size_t comment = value.find('#');
    if (comment != std::string_view::npos)
    {
        value = value.substr(0, comment);
    }
    out_value = Trim(value);
    return !out_value.empty();
}

} // namespace

BindingMap ParseMappings(std::string_view file_contents)
{
    BindingMap map;
    std::size_t start = 0;
    while (start <= file_contents.size())
    {
        std::size_t nl = file_contents.find('\n', start);
        const std::size_t end = (nl == std::string_view::npos) ? file_contents.size() : nl;
        int scancode = 0;
        std::string_view value;
        if (ParseLine(file_contents.substr(start, end - start), scancode, value))
        {
            std::vector<ControllerBinding> bindings = ParseBindingList(value);
            if (!bindings.empty())
            {
                std::vector<ControllerBinding>& target = map[scancode];
                target.insert(target.end(), bindings.begin(), bindings.end());
            }
        }
        if (nl == std::string_view::npos)
        {
            break;
        }
        start = nl + 1;
    }
    return map;
}

void DeserializeMappings(
    std::string_view file_contents,
    std::vector<std::string>& out_standard_rows,
    std::vector<std::pair<std::string, std::string>>& out_custom_rows)
{
    const std::vector<KeyDef>& keys = StandardKeys();
    out_standard_rows.assign(keys.size(), std::string());
    out_custom_rows.clear();

    // scancode -> index in StandardKeys for fast row matching.
    std::unordered_map<int, std::size_t> scancode_to_index;
    scancode_to_index.reserve(keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        scancode_to_index.emplace(keys[i].code, i);
    }

    std::size_t start = 0;
    while (start <= file_contents.size())
    {
        std::size_t nl = file_contents.find('\n', start);
        const std::size_t end = (nl == std::string_view::npos) ? file_contents.size() : nl;
        int scancode = 0;
        std::string_view value;
        if (ParseLine(file_contents.substr(start, end - start), scancode, value))
        {
            const auto it = scancode_to_index.find(scancode);
            if (it != scancode_to_index.end())
            {
                out_standard_rows[it->second] = std::string(value);
            }
            else
            {
                const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
                out_custom_rows.emplace_back(name ? name : std::string(), std::string(value));
            }
        }
        if (nl == std::string_view::npos)
        {
            break;
        }
        start = nl + 1;
    }
}

} // namespace input
