#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Shared controller-mapping model used by both the editor's Mapping panel and
// the runtime renderer. The panel captures controller inputs against keyboard
// keys; the runtime loads those mappings so scripts/nodes that already poll a
// keyboard key (e.g. Input.IsKeyDown("Space")) also respond to the mapped
// controller input.
namespace input
{

// Virtual input codes for non-keyboard mapping targets. Negative so they never
// collide with SDL scancodes (which are >= 0). Mouse-move directions feed the
// synthetic mouse delta returned by Input.MouseDelta(), letting a controller
// stick drive any existing mouse-look script.
enum VirtualInputCode : int
{
    kMouseMoveRight = -1,
    kMouseMoveLeft  = -2,
    kMouseMoveUp    = -3,
    kMouseMoveDown  = -4,
};

// One canonical mappable input: the label shown in the Mapping panel plus the
// code identifying it. The code is either an SDL scancode (>= 0) or a
// VirtualInputCode (< 0). It is the shared identity between the editor UI
// (rows) and the runtime. Several panel labels do not match SDL's key names
// verbatim, so the code is stored explicitly rather than re-derived.
struct KeyDef
{
    std::string_view label;
    int              code;
};

// The fixed list of mappable keyboard/mouse inputs (single source of truth for
// the Mapping panel rows and for serialization ordering).
const std::vector<KeyDef>& StandardKeys();

// A single controller input bound to a keyboard key.
struct ControllerBinding
{
    enum class Kind
    {
        Button,
        Axis,
    };

    Kind kind     = Kind::Button;
    int  index    = 0;     // SDL_GamepadButton or SDL_GamepadAxis value.
    bool positive = true;  // Axis direction (+/-); ignored for buttons.
};

// Stick/trigger activation threshold, shared with the mapping-capture UI so a
// binding "fires" at the same deflection it was recorded at.
inline constexpr Sint16 kAxisThreshold = 16000;

// Parse a single stored token ("a", "dpup", "leftx+", "lefttrigger+") into a
// binding. Returns false if the token names no known button/axis.
bool ParseBindingToken(std::string_view token, ControllerBinding& out);

// Parse a comma-separated controller string ("a, leftx+") into bindings,
// skipping any tokens that fail to parse.
std::vector<ControllerBinding> ParseBindingList(std::string_view csv);

// scancode (int) -> controller inputs bound to that key.
using BindingMap = std::unordered_map<int, std::vector<ControllerBinding>>;

// Build the on-disk file contents from the Mapping panel's row data.
//   standard_rows : parallel to StandardKeys(); each value is a controller CSV.
//   custom_rows   : (typed key name, controller CSV) pairs.
// Rows whose key resolves to no scancode, or whose controller value is empty,
// are omitted.
std::string SerializeMappings(
    const std::vector<std::string>& standard_rows,
    const std::vector<std::pair<std::string, std::string>>& custom_rows);

// Parse file contents into a runtime lookup map keyed by SDL scancode. Tokens
// that fail to parse are skipped; duplicate scancodes are merged.
BindingMap ParseMappings(std::string_view file_contents);

// Parse file contents back into editor row data (raw CSV strings preserved).
//   out_standard_rows : resized to StandardKeys() and filled by scancode match.
//   out_custom_rows   : scancodes outside StandardKeys() become labelled rows.
void DeserializeMappings(
    std::string_view file_contents,
    std::vector<std::string>& out_standard_rows,
    std::vector<std::pair<std::string, std::string>>& out_custom_rows);

} // namespace input
