#include "render/RuntimeRenderer.h"

#include "assets/PrefabAsset.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

namespace
{
RuntimeRenderer* GetRuntimeRenderer(lua_State* lua_state)
{
    return static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
}

int GetAttributeAccessorId(lua_State* lua_state)
{
    return static_cast<int>(lua_tointeger(lua_state, lua_upvalueindex(2)));
}

std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

const char* ToScriptPhysicsShapeName(SceneObjectPhysicsShape shape)
{
    switch (shape)
    {
    case SceneObjectPhysicsShape::None:
        return "None";
    case SceneObjectPhysicsShape::Box:
        return "Box";
    case SceneObjectPhysicsShape::Sphere:
        return "Sphere";
    case SceneObjectPhysicsShape::Capsule:
        return "Capsule";
    case SceneObjectPhysicsShape::Mesh:
        return "Mesh";
    default:
        return "None";
    }
}

bool TryParsePhysicsShape(std::string value, SceneObjectPhysicsShape& shape)
{
    value = ToLowerAscii(std::move(value));
    if (value == "none")
    {
        shape = SceneObjectPhysicsShape::None;
        return true;
    }
    if (value == "box")
    {
        shape = SceneObjectPhysicsShape::Box;
        return true;
    }
    if (value == "sphere")
    {
        shape = SceneObjectPhysicsShape::Sphere;
        return true;
    }
    if (value == "capsule")
    {
        shape = SceneObjectPhysicsShape::Capsule;
        return true;
    }
    if (value == "mesh")
    {
        shape = SceneObjectPhysicsShape::Mesh;
        return true;
    }

    return false;
}

int PushFilteredCollisionsToLua(
    lua_State* lua_state,
    const std::vector<PhysicsCollisionEvent>& collisions,
    const std::string& object_filter,
    const std::string& phase_filter)
{
    lua_newtable(lua_state);
    int lua_index = 1;
    for (const PhysicsCollisionEvent& collision : collisions)
    {
        if (!phase_filter.empty() && collision.phase != phase_filter)
        {
            continue;
        }

        const bool matches_object_filter = object_filter.empty()
            || collision.object_a == object_filter
            || collision.object_b == object_filter;
        if (!matches_object_filter)
        {
            continue;
        }

        lua_newtable(lua_state);

        lua_pushstring(lua_state, collision.object_a.c_str());
        lua_setfield(lua_state, -2, "a");

        lua_pushstring(lua_state, collision.object_b.c_str());
        lua_setfield(lua_state, -2, "b");

        lua_pushstring(lua_state, collision.phase.c_str());
        lua_setfield(lua_state, -2, "phase");

        if (!object_filter.empty())
        {
            const char* other = nullptr;
            if (collision.object_a == object_filter)
            {
                other = collision.object_b.c_str();
            }
            else if (collision.object_b == object_filter)
            {
                other = collision.object_a.c_str();
            }

            if (other != nullptr)
            {
                lua_pushstring(lua_state, other);
                lua_setfield(lua_state, -2, "other");
            }
        }

        lua_rawseti(lua_state, -2, lua_index++);
    }

    return 1;
}
}

// ============================================================
// Physics API callbacks
// ============================================================

int RuntimeRenderer::LuaPhysicsRaycast(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const float ox = static_cast<float>(luaL_checknumber(lua_state, 1));
    const float oy = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float oz = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float dx = static_cast<float>(luaL_checknumber(lua_state, 4));
    const float dy = static_cast<float>(luaL_checknumber(lua_state, 5));
    const float dz = static_cast<float>(luaL_checknumber(lua_state, 6));
    const float max_dist = static_cast<float>(luaL_optnumber(lua_state, 7, 1000.0));

    const PhysicsRaycastHit hit = renderer->physics_world_.Raycast({ox, oy, oz}, {dx, dy, dz}, max_dist);
    if (!hit.hit)
    {
        lua_pushnil(lua_state);
        return 1;
    }

    lua_newtable(lua_state);
    lua_pushstring(lua_state, hit.object_name.c_str());
    lua_setfield(lua_state, -2, "object");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.distance));
    lua_setfield(lua_state, -2, "distance");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.position[0]));
    lua_setfield(lua_state, -2, "x");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.position[1]));
    lua_setfield(lua_state, -2, "y");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.position[2]));
    lua_setfield(lua_state, -2, "z");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.normal[0]));
    lua_setfield(lua_state, -2, "nx");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.normal[1]));
    lua_setfield(lua_state, -2, "ny");
    lua_pushnumber(lua_state, static_cast<lua_Number>(hit.normal[2]));
    lua_setfield(lua_state, -2, "nz");
    return 1;
}

int RuntimeRenderer::LuaPhysicsSetVelocity(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
    renderer->physics_world_.SetLinearVelocity(object_name, {x, y, z});
    return 0;
}

int RuntimeRenderer::LuaPhysicsGetVelocity(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneVector3 velocity = {};
    if (!renderer->physics_world_.GetLinearVelocity(object_name, velocity))
    {
        lua_pushnil(lua_state);
        return 1;
    }

    lua_pushnumber(lua_state, static_cast<lua_Number>(velocity[0]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(velocity[1]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(velocity[2]));
    return 3;
}

int RuntimeRenderer::LuaPhysicsAddImpulse(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
    renderer->physics_world_.AddImpulse(object_name, {x, y, z});
    return 0;
}

int RuntimeRenderer::LuaPhysicsAddForce(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
    renderer->physics_world_.AddForce(object_name, {x, y, z});
    return 0;
}

int RuntimeRenderer::LuaLog(lua_State* lua_state)
{
    const char* message = luaL_optstring(lua_state, 1, "");
    SDL_Log("[Lua] %s", message);
    return 0;
}

int RuntimeRenderer::LuaSetObjectPosition(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
    renderer->SetScriptObjectPosition(object_name, {x, y, z});
    return 0;
}

int RuntimeRenderer::LuaGetObjectPosition(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneVector3 position = {};
    if (!renderer->TryGetScriptObjectPosition(object_name, position))
    {
        lua_pushnil(lua_state);
        return 1;
    }

    lua_pushnumber(lua_state, static_cast<lua_Number>(position[0]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(position[1]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(position[2]));
    return 3;
}

int RuntimeRenderer::LuaSetObjectRotation(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
    renderer->SetScriptObjectRotation(object_name, {x, y, z});
    return 0;
}

int RuntimeRenderer::LuaGetObjectRotation(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneVector3 rotation = {};
    if (!renderer->TryGetScriptObjectRotation(object_name, rotation))
    {
        lua_pushnil(lua_state);
        return 1;
    }

    lua_pushnumber(lua_state, static_cast<lua_Number>(rotation[0]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(rotation[1]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(rotation[2]));
    return 3;
}

int RuntimeRenderer::LuaSetObjectScale(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
    const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
    const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
    renderer->SetScriptObjectScale(object_name, {x, y, z});
    return 0;
}

int RuntimeRenderer::LuaGetObjectScale(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneVector3 scale = {};
    if (!renderer->TryGetScriptObjectScale(object_name, scale))
    {
        lua_pushnil(lua_state);
        return 1;
    }

    lua_pushnumber(lua_state, static_cast<lua_Number>(scale[0]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(scale[1]));
    lua_pushnumber(lua_state, static_cast<lua_Number>(scale[2]));
    return 3;
}

int RuntimeRenderer::LuaSetObjectEnabled(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const bool enabled = lua_toboolean(lua_state, 2) != 0;
    lua_pushboolean(lua_state, renderer->SetScriptObjectEnabled(object_name, enabled) ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaGetObjectEnabled(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    bool enabled = false;
    if (!renderer->TryGetScriptObjectEnabled(object_name, enabled))
    {
        lua_pushnil(lua_state);
        return 1;
    }

    lua_pushboolean(lua_state, enabled ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaSetCameraActive(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const bool active = lua_toboolean(lua_state, 2) != 0;
    lua_pushboolean(lua_state, renderer->SetScriptCameraActive(object_name, active) ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaAttributeAccessor(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = GetRuntimeRenderer(lua_state);
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const ScriptAttributeAccessorId accessor_id = static_cast<ScriptAttributeAccessorId>(GetAttributeAccessorId(lua_state));
    const bool is_setter = lua_gettop(lua_state) >= 2;

    const auto access_string = [&](SceneObjectAttributeKind kind, auto getter, auto setter) -> int
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
            if (attribute == nullptr)
            {
                return 0;
            }

            const char* value = luaL_optstring(lua_state, 2, "");
            setter(*attribute, value != nullptr ? value : "");
            renderer->HandleScriptAttributeMutation(kind, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        const std::string value = getter(*attribute);
        lua_pushstring(lua_state, value.c_str());
        return 1;
    };

    const auto access_float = [&](SceneObjectAttributeKind kind, auto getter, auto setter) -> int
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
            if (attribute == nullptr)
            {
                return 0;
            }

            setter(*attribute, static_cast<float>(luaL_checknumber(lua_state, 2)));
            renderer->HandleScriptAttributeMutation(kind, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushnumber(lua_state, static_cast<lua_Number>(getter(*attribute)));
        return 1;
    };

    const auto access_bool = [&](SceneObjectAttributeKind kind, auto getter, auto setter) -> int
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
            if (attribute == nullptr)
            {
                return 0;
            }

            setter(*attribute, lua_toboolean(lua_state, 2) != 0);
            renderer->HandleScriptAttributeMutation(kind, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushboolean(lua_state, getter(*attribute) ? 1 : 0);
        return 1;
    };

    const auto access_int = [&](SceneObjectAttributeKind kind, auto getter, auto setter) -> int
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
            if (attribute == nullptr)
            {
                return 0;
            }

            setter(*attribute, static_cast<int>(luaL_checkinteger(lua_state, 2)));
            renderer->HandleScriptAttributeMutation(kind, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushinteger(lua_state, static_cast<lua_Integer>(getter(*attribute)));
        return 1;
    };

    const auto access_vec2 = [&](SceneObjectAttributeKind kind, auto getter, auto setter) -> int
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
            if (attribute == nullptr)
            {
                return 0;
            }

            setter(
                *attribute,
                static_cast<float>(luaL_checknumber(lua_state, 2)),
                static_cast<float>(luaL_checknumber(lua_state, 3)));
            renderer->HandleScriptAttributeMutation(kind, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        const std::array<float, 2> value = getter(*attribute);
        lua_pushnumber(lua_state, static_cast<lua_Number>(value[0]));
        lua_pushnumber(lua_state, static_cast<lua_Number>(value[1]));
        return 2;
    };

    const auto access_vec3 = [&](SceneObjectAttributeKind kind, auto getter, auto setter) -> int
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
            if (attribute == nullptr)
            {
                return 0;
            }

            setter(
                *attribute,
                static_cast<float>(luaL_checknumber(lua_state, 2)),
                static_cast<float>(luaL_checknumber(lua_state, 3)),
                static_cast<float>(luaL_checknumber(lua_state, 4)));
            renderer->HandleScriptAttributeMutation(kind, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, kind);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        const SceneVector3 value = getter(*attribute);
        lua_pushnumber(lua_state, static_cast<lua_Number>(value[0]));
        lua_pushnumber(lua_state, static_cast<lua_Number>(value[1]));
        lua_pushnumber(lua_state, static_cast<lua_Number>(value[2]));
        return 3;
    };

    switch (accessor_id)
    {
    case ScriptAttributeAccessorId::EnvironmentLightColor:
        return access_vec3(SceneObjectAttributeKind::EnvironmentLight,
            [](const SceneObjectAttribute& attribute) { return attribute.environment_light.color; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.environment_light.color = {x, y, z}; });
    case ScriptAttributeAccessorId::EnvironmentLightIntensity:
        return access_float(SceneObjectAttributeKind::EnvironmentLight,
            [](const SceneObjectAttribute& attribute) { return attribute.environment_light.intensity; },
            [](SceneObjectAttribute& attribute, float value) { attribute.environment_light.intensity = value; });
    case ScriptAttributeAccessorId::DirectionalLightColor:
        return access_vec3(SceneObjectAttributeKind::DirectionalLight,
            [](const SceneObjectAttribute& attribute) { return attribute.directional_light.color; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.directional_light.color = {x, y, z}; });
    case ScriptAttributeAccessorId::DirectionalLightIntensity:
        return access_float(SceneObjectAttributeKind::DirectionalLight,
            [](const SceneObjectAttribute& attribute) { return attribute.directional_light.intensity; },
            [](SceneObjectAttribute& attribute, float value) { attribute.directional_light.intensity = value; });
    case ScriptAttributeAccessorId::PointLightColor:
        return access_vec3(SceneObjectAttributeKind::PointLight,
            [](const SceneObjectAttribute& attribute) { return attribute.point_light.color; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.point_light.color = {x, y, z}; });
    case ScriptAttributeAccessorId::PointLightIntensity:
        return access_float(SceneObjectAttributeKind::PointLight,
            [](const SceneObjectAttribute& attribute) { return attribute.point_light.intensity; },
            [](SceneObjectAttribute& attribute, float value) { attribute.point_light.intensity = value; });
    case ScriptAttributeAccessorId::PointLightRange:
        return access_float(SceneObjectAttributeKind::PointLight,
            [](const SceneObjectAttribute& attribute) { return attribute.point_light.range; },
            [](SceneObjectAttribute& attribute, float value) { attribute.point_light.range = (std::max)(0.01f, value); });
    case ScriptAttributeAccessorId::PointLightRadius:
        return access_float(SceneObjectAttributeKind::PointLight,
            [](const SceneObjectAttribute& attribute) { return attribute.point_light.source_radius; },
            [](SceneObjectAttribute& attribute, float value) { attribute.point_light.source_radius = (std::max)(0.01f, value); });
    case ScriptAttributeAccessorId::PointLightHaloIntensity:
        return access_float(SceneObjectAttributeKind::PointLight,
            [](const SceneObjectAttribute& attribute) { return attribute.point_light.halo_intensity; },
            [](SceneObjectAttribute& attribute, float value) { attribute.point_light.halo_intensity = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::PointLightHaloRadius:
        return access_float(SceneObjectAttributeKind::PointLight,
            [](const SceneObjectAttribute& attribute) { return attribute.point_light.halo_radius; },
            [](SceneObjectAttribute& attribute, float value) { attribute.point_light.halo_radius = (std::max)(0.01f, value); });
    case ScriptAttributeAccessorId::SpotLightColor:
        return access_vec3(SceneObjectAttributeKind::SpotLight,
            [](const SceneObjectAttribute& attribute) { return attribute.spot_light.color; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.spot_light.color = {x, y, z}; });
    case ScriptAttributeAccessorId::SpotLightIntensity:
        return access_float(SceneObjectAttributeKind::SpotLight,
            [](const SceneObjectAttribute& attribute) { return attribute.spot_light.intensity; },
            [](SceneObjectAttribute& attribute, float value) { attribute.spot_light.intensity = value; });
    case ScriptAttributeAccessorId::SpotLightRange:
        return access_float(SceneObjectAttributeKind::SpotLight,
            [](const SceneObjectAttribute& attribute) { return attribute.spot_light.range; },
            [](SceneObjectAttribute& attribute, float value) { attribute.spot_light.range = (std::max)(0.01f, value); });
    case ScriptAttributeAccessorId::SpotLightInnerCone:
        return access_float(SceneObjectAttributeKind::SpotLight,
            [](const SceneObjectAttribute& attribute) { return attribute.spot_light.inner_cone_degrees; },
            [](SceneObjectAttribute& attribute, float value) { attribute.spot_light.inner_cone_degrees = std::clamp(value, 0.1f, 89.0f); });
    case ScriptAttributeAccessorId::SpotLightOuterCone:
        return access_float(SceneObjectAttributeKind::SpotLight,
            [](const SceneObjectAttribute& attribute) { return attribute.spot_light.outer_cone_degrees; },
            [](SceneObjectAttribute& attribute, float value) { attribute.spot_light.outer_cone_degrees = std::clamp(value, 0.1f, 89.0f); });
    case ScriptAttributeAccessorId::CameraFieldOfView:
        return access_float(SceneObjectAttributeKind::Camera,
            [](const SceneObjectAttribute& attribute) { return attribute.camera.field_of_view_degrees; },
            [](SceneObjectAttribute& attribute, float value) { attribute.camera.field_of_view_degrees = std::clamp(value, 1.0f, 179.0f); });
    case ScriptAttributeAccessorId::CameraNearClip:
        return access_float(SceneObjectAttributeKind::Camera,
            [](const SceneObjectAttribute& attribute) { return attribute.camera.near_clip; },
            [](SceneObjectAttribute& attribute, float value) { attribute.camera.near_clip = (std::max)(0.001f, value); });
    case ScriptAttributeAccessorId::CameraFarClip:
        return access_float(SceneObjectAttributeKind::Camera,
            [](const SceneObjectAttribute& attribute) { return attribute.camera.far_clip; },
            [](SceneObjectAttribute& attribute, float value) { attribute.camera.far_clip = (std::max)(0.1f, value); });
    case ScriptAttributeAccessorId::CameraActive:
        return access_bool(SceneObjectAttributeKind::Camera,
            [](const SceneObjectAttribute& attribute) { return attribute.camera.active; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.camera.active = value; });
    case ScriptAttributeAccessorId::RigidbodyShape:
    {
        if (is_setter)
        {
            SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Rigidbody);
            if (attribute == nullptr)
            {
                return 0;
            }

            SceneObjectPhysicsShape shape = SceneObjectPhysicsShape::None;
            if (!TryParsePhysicsShape(luaL_checkstring(lua_state, 2), shape))
            {
                return luaL_error(lua_state, "Unknown rigidbody shape. Expected None, Box, Sphere, Capsule, or Mesh");
            }

            attribute->rigidbody.shape = shape;
            if (shape == SceneObjectPhysicsShape::Mesh)
            {
                attribute->rigidbody.is_dynamic = false;
            }
            renderer->HandleScriptAttributeMutation(SceneObjectAttributeKind::Rigidbody, accessor_id);
            return 0;
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Rigidbody);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushstring(lua_state, ToScriptPhysicsShapeName(attribute->rigidbody.shape));
        return 1;
    }
    case ScriptAttributeAccessorId::RigidbodyDynamic:
        return access_bool(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.is_dynamic; },
            [](SceneObjectAttribute& attribute, bool value)
            {
                attribute.rigidbody.is_dynamic = attribute.rigidbody.shape == SceneObjectPhysicsShape::Mesh ? false : value;
            });
    case ScriptAttributeAccessorId::RigidbodyLockRotationX:
        return access_bool(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.lock_rotation_x; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.rigidbody.lock_rotation_x = value; });
    case ScriptAttributeAccessorId::RigidbodyLockRotationY:
        return access_bool(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.lock_rotation_y; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.rigidbody.lock_rotation_y = value; });
    case ScriptAttributeAccessorId::RigidbodyLockRotationZ:
        return access_bool(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.lock_rotation_z; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.rigidbody.lock_rotation_z = value; });
    case ScriptAttributeAccessorId::RigidbodyMass:
        return access_float(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.mass; },
            [](SceneObjectAttribute& attribute, float value) { attribute.rigidbody.mass = (std::max)(0.001f, value); });
    case ScriptAttributeAccessorId::RigidbodyFriction:
        return access_float(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.friction; },
            [](SceneObjectAttribute& attribute, float value) { attribute.rigidbody.friction = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::RigidbodyRadius:
        return access_float(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.radius; },
            [](SceneObjectAttribute& attribute, float value) { attribute.rigidbody.radius = (std::max)(0.01f, value); });
    case ScriptAttributeAccessorId::RigidbodyCapsuleHalfHeight:
        return access_float(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.capsule_half_height; },
            [](SceneObjectAttribute& attribute, float value) { attribute.rigidbody.capsule_half_height = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::RigidbodyHalfExtent:
        return access_vec3(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.half_extent; },
            [](SceneObjectAttribute& attribute, float x, float y, float z)
            {
                attribute.rigidbody.half_extent = {(std::max)(0.01f, x), (std::max)(0.01f, y), (std::max)(0.01f, z)};
            });
    case ScriptAttributeAccessorId::RigidbodyLinearDamping:
        return access_float(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.linear_damping; },
            [](SceneObjectAttribute& attribute, float value) { attribute.rigidbody.linear_damping = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::RigidbodyAngularDamping:
        return access_float(SceneObjectAttributeKind::Rigidbody,
            [](const SceneObjectAttribute& attribute) { return attribute.rigidbody.angular_damping; },
            [](SceneObjectAttribute& attribute, float value) { attribute.rigidbody.angular_damping = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::TriggerVolumeHalfExtent:
        return access_vec3(SceneObjectAttributeKind::TriggerVolume,
            [](const SceneObjectAttribute& attribute) { return attribute.trigger_box.half_extent; },
            [](SceneObjectAttribute& attribute, float x, float y, float z)
            {
                attribute.trigger_box.half_extent = {(std::max)(0.01f, x), (std::max)(0.01f, y), (std::max)(0.01f, z)};
            });
    case ScriptAttributeAccessorId::Text2DFontPath:
        return access_string(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.font_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.text_2d.font_path = value; });
    case ScriptAttributeAccessorId::Text2DText:
        return access_string(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.text; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.text_2d.text = value; });
    case ScriptAttributeAccessorId::Text2DPosition:
        return access_vec2(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.text_2d.x, attribute.text_2d.y}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.text_2d.x = x;
                attribute.text_2d.y = y;
            });
    case ScriptAttributeAccessorId::Text2DSize:
        return access_vec2(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.text_2d.width, attribute.text_2d.height}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.text_2d.width = (std::max)(1.0f, x);
                attribute.text_2d.height = (std::max)(1.0f, y);
            });
    case ScriptAttributeAccessorId::Text2DLockAspectRatio:
        return access_bool(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.lock_aspect_ratio; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.text_2d.lock_aspect_ratio = value; });
    case ScriptAttributeAccessorId::Text2DFontSize:
        return access_float(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.font_size; },
            [](SceneObjectAttribute& attribute, float value) { attribute.text_2d.font_size = (std::max)(1.0f, value); });
    case ScriptAttributeAccessorId::Text2DColor:
        return access_vec3(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.color; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.text_2d.color = {x, y, z}; });
    case ScriptAttributeAccessorId::Text2DAlpha:
        return access_float(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.alpha; },
            [](SceneObjectAttribute& attribute, float value) { attribute.text_2d.alpha = std::clamp(value, 0.0f, 1.0f); });
    case ScriptAttributeAccessorId::Text2DPriority:
        return access_int(SceneObjectAttributeKind::Text2D,
            [](const SceneObjectAttribute& attribute) { return attribute.text_2d.priority; },
            [](SceneObjectAttribute& attribute, int value) { attribute.text_2d.priority = value; });
    case ScriptAttributeAccessorId::Image2DImagePath:
        return access_string(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return attribute.image_2d.image_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.image_2d.image_path = value; });
    case ScriptAttributeAccessorId::Image2DPosition:
        return access_vec2(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.image_2d.x, attribute.image_2d.y}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.image_2d.x = x;
                attribute.image_2d.y = y;
            });
    case ScriptAttributeAccessorId::Image2DSize:
        return access_vec2(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.image_2d.width, attribute.image_2d.height}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.image_2d.width = (std::max)(1.0f, x);
                attribute.image_2d.height = (std::max)(1.0f, y);
            });
    case ScriptAttributeAccessorId::Image2DLockAspectRatio:
        return access_bool(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return attribute.image_2d.lock_aspect_ratio; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.image_2d.lock_aspect_ratio = value; });
    case ScriptAttributeAccessorId::Image2DStretchToScreen:
        return access_bool(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return attribute.image_2d.stretch_to_screen; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.image_2d.stretch_to_screen = value; });
    case ScriptAttributeAccessorId::Image2DPlayMode:
        return access_string(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) -> std::string {
                switch (attribute.image_2d.play_mode)
                {
                case SceneObjectImagePlayMode::Loop: return "Loop";
                case SceneObjectImagePlayMode::PlayOnce: return "PlayOnce";
                case SceneObjectImagePlayMode::Off: default: return "Off";
                }
            },
            [](SceneObjectAttribute& attribute, const std::string& value) {
                if (value == "Loop" || value == "loop")
                {
                    attribute.image_2d.play_mode = SceneObjectImagePlayMode::Loop;
                }
                else if (value == "PlayOnce" || value == "playonce" || value == "Once" || value == "once")
                {
                    attribute.image_2d.play_mode = SceneObjectImagePlayMode::PlayOnce;
                }
                else
                {
                    attribute.image_2d.play_mode = SceneObjectImagePlayMode::Off;
                }
            });
    case ScriptAttributeAccessorId::Image2DTint:
        return access_vec3(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return attribute.image_2d.tint; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.image_2d.tint = {x, y, z}; });
    case ScriptAttributeAccessorId::Image2DAlpha:
        return access_float(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return attribute.image_2d.alpha; },
            [](SceneObjectAttribute& attribute, float value) { attribute.image_2d.alpha = std::clamp(value, 0.0f, 1.0f); });
    case ScriptAttributeAccessorId::Image2DPriority:
        return access_int(SceneObjectAttributeKind::Image2D,
            [](const SceneObjectAttribute& attribute) { return attribute.image_2d.priority; },
            [](SceneObjectAttribute& attribute, int value) { attribute.image_2d.priority = value; });
    case ScriptAttributeAccessorId::Color2DPosition:
        return access_vec2(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.color_2d.x, attribute.color_2d.y}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.color_2d.x = x;
                attribute.color_2d.y = y;
            });
    case ScriptAttributeAccessorId::Color2DSize:
        return access_vec2(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.color_2d.width, attribute.color_2d.height}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.color_2d.width = (std::max)(1.0f, x);
                attribute.color_2d.height = (std::max)(1.0f, y);
            });
    case ScriptAttributeAccessorId::Color2DLockAspectRatio:
        return access_bool(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return attribute.color_2d.lock_aspect_ratio; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.color_2d.lock_aspect_ratio = value; });
    case ScriptAttributeAccessorId::Color2DStretchToScreen:
        return access_bool(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return attribute.color_2d.stretch_to_screen; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.color_2d.stretch_to_screen = value; });
    case ScriptAttributeAccessorId::Color2DColor:
        return access_vec3(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return attribute.color_2d.color; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.color_2d.color = {x, y, z}; });
    case ScriptAttributeAccessorId::Color2DAlpha:
        return access_float(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return attribute.color_2d.alpha; },
            [](SceneObjectAttribute& attribute, float value) { attribute.color_2d.alpha = std::clamp(value, 0.0f, 1.0f); });
    case ScriptAttributeAccessorId::Color2DPriority:
        return access_int(SceneObjectAttributeKind::Color2D,
            [](const SceneObjectAttribute& attribute) { return attribute.color_2d.priority; },
            [](SceneObjectAttribute& attribute, int value) { attribute.color_2d.priority = value; });
    case ScriptAttributeAccessorId::SkyboxImagePath:
        return access_string(SceneObjectAttributeKind::Skybox,
            [](const SceneObjectAttribute& attribute) { return attribute.skybox.image_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.skybox.image_path = value; });
    case ScriptAttributeAccessorId::SkyboxRotation:
        return access_float(SceneObjectAttributeKind::Skybox,
            [](const SceneObjectAttribute& attribute) { return attribute.skybox.rotation_degrees; },
            [](SceneObjectAttribute& attribute, float value) { attribute.skybox.rotation_degrees = value; });
    case ScriptAttributeAccessorId::AnimatorControllerPath:
        return access_string(SceneObjectAttributeKind::Animator,
            [](const SceneObjectAttribute& attribute) { return attribute.animator.controller_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.animator.controller_path = value; });
    case ScriptAttributeAccessorId::AnimatorInitialState:
        return access_string(SceneObjectAttributeKind::Animator,
            [](const SceneObjectAttribute& attribute) { return attribute.animator.initial_state; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.animator.initial_state = value; });
    case ScriptAttributeAccessorId::AnimatorPlaybackSpeed:
        return access_float(SceneObjectAttributeKind::Animator,
            [](const SceneObjectAttribute& attribute) { return attribute.animator.playback_speed; },
            [](SceneObjectAttribute& attribute, float value) { attribute.animator.playback_speed = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::AnimatorAutoPlay:
        return access_bool(SceneObjectAttributeKind::Animator,
            [](const SceneObjectAttribute& attribute) { return attribute.animator.auto_play; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.animator.auto_play = value; });
    case ScriptAttributeAccessorId::AnimatorActiveState:
    {
        if (is_setter)
        {
            return luaL_error(lua_state, "Animator.GetState is read-only");
        }

        const RuntimeRenderer::RuntimeAnimatorState* const runtime_state = renderer->FindRuntimeAnimatorState(object_name);
        if (runtime_state == nullptr || runtime_state->active_state.empty())
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushstring(lua_state, runtime_state->active_state.c_str());
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorStateTime:
    {
        if (is_setter)
        {
            return luaL_error(lua_state, "Animator.StateTime is read-only");
        }

        const RuntimeRenderer::RuntimeAnimatorState* const runtime_state = renderer->FindRuntimeAnimatorState(object_name);
        if (runtime_state == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushnumber(lua_state, static_cast<lua_Number>(runtime_state->state_time_seconds));
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorSetBool:
    {
        const char* parameter_name = luaL_checkstring(lua_state, 2);
        const bool set_ok = renderer->SetRuntimeAnimatorBoolParameter(object_name, parameter_name, lua_toboolean(lua_state, 3) != 0);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorGetBool:
    {
        const char* parameter_name = luaL_checkstring(lua_state, 2);
        float value = 0.0f;
        bool is_bool = false;
        if (!renderer->TryGetRuntimeAnimatorParameter(object_name, parameter_name, value, is_bool))
        {
            lua_pushboolean(lua_state, 0);
            return 1;
        }

        lua_pushboolean(lua_state, is_bool && value != 0.0f ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorSetTrigger:
    {
        const char* trigger_name = luaL_checkstring(lua_state, 2);
        const bool set_ok = renderer->SetRuntimeAnimatorTrigger(object_name, trigger_name);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorSetState:
    {
        const char* state_name = luaL_checkstring(lua_state, 2);
        const bool set_ok = renderer->SetRuntimeAnimatorState(object_name, state_name);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorSetFacePose:
    {
        // (name, poseName [, weight] [, speed]). Empty/absent poseName reverts
        // to the controller's default pose. weight (0..1) is the intensity
        // (default 1); speed eases the transition (default 0 = snap).
        const char* pose_name = luaL_optstring(lua_state, 2, "");
        const float weight = static_cast<float>(luaL_optnumber(lua_state, 3, 1.0));
        const float speed = static_cast<float>(luaL_optnumber(lua_state, 4, 0.0));
        const bool set_ok = renderer->SetRuntimeAnimatorFacePose(object_name, pose_name, weight, speed);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorPlayLipSync:
    {
        const char* clip_name = luaL_checkstring(lua_state, 2);
        const bool loop = lua_toboolean(lua_state, 3) != 0; // optional 2nd arg
        const bool set_ok = renderer->PlayRuntimeAnimatorLipSync(object_name, clip_name, loop);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorStopLipSync:
    {
        const bool set_ok = renderer->StopRuntimeAnimatorLipSync(object_name);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorIsLipSyncPlaying:
    {
        lua_pushboolean(lua_state, renderer->IsRuntimeAnimatorLipSyncPlaying(object_name) ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorSetEyeTarget:
    {
        const float x = static_cast<float>(luaL_checknumber(lua_state, 2));
        const float y = static_cast<float>(luaL_checknumber(lua_state, 3));
        const float z = static_cast<float>(luaL_checknumber(lua_state, 4));
        const bool set_ok = renderer->SetRuntimeAnimatorEyeTarget(object_name, x, y, z);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorLookAt:
    {
        const char* target_name = luaL_checkstring(lua_state, 2);
        const bool set_ok = renderer->LookAtRuntimeAnimator(object_name, target_name);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorClearEyeTarget:
    {
        const bool set_ok = renderer->ClearRuntimeAnimatorEyeTarget(object_name);
        lua_pushboolean(lua_state, set_ok ? 1 : 0);
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorGetState:
    {
        if (is_setter)
        {
            return luaL_error(lua_state, "Animator.GetState is read-only");
        }

        const RuntimeRenderer::RuntimeAnimatorState* const runtime_state = renderer->FindRuntimeAnimatorState(object_name);
        if (runtime_state == nullptr || runtime_state->active_state.empty())
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushstring(lua_state, runtime_state->active_state.c_str());
        return 1;
    }
    case ScriptAttributeAccessorId::AnimatorSetDefaultState:
        return access_string(SceneObjectAttributeKind::Animator,
            [](const SceneObjectAttribute& attribute) { return attribute.animator.initial_state; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.animator.initial_state = value; });
    case ScriptAttributeAccessorId::AnimatorGetDefaultState:
    {
        if (is_setter)
        {
            return luaL_error(lua_state, "Animator.GetDefaultState is read-only");
        }

        const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Animator);
        if (attribute == nullptr)
        {
            lua_pushnil(lua_state);
            return 1;
        }

        lua_pushstring(lua_state, attribute->animator.initial_state.c_str());
        return 1;
    }
    case ScriptAttributeAccessorId::AudioClipPath:
        return access_string(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.clip_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.audio.clip_path = value; });
    case ScriptAttributeAccessorId::AudioPlayMode:
        return access_string(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) -> std::string {
                return attribute.audio.play_mode == SceneObjectAudioPlayMode::On ? "On" : "Off";
            },
            [](SceneObjectAttribute& attribute, const std::string& value) {
                attribute.audio.play_mode = (value == "On" || value == "on" || value == "Autoplay")
                    ? SceneObjectAudioPlayMode::On
                    : SceneObjectAudioPlayMode::Off;
            });
    case ScriptAttributeAccessorId::AudioVolume:
        return access_float(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.volume; },
            [](SceneObjectAttribute& attribute, float value) { attribute.audio.volume = std::clamp(value, 0.0f, 20.0f); });
    case ScriptAttributeAccessorId::AudioLoop:
        return access_bool(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.loop; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.audio.loop = value; });
    case ScriptAttributeAccessorId::AudioSpatialize3D:
        return access_bool(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.spatialize_3d; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.audio.spatialize_3d = value; });
    case ScriptAttributeAccessorId::AudioPitch:
        return access_float(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.pitch; },
            [](SceneObjectAttribute& attribute, float value) { attribute.audio.pitch = std::clamp(value, 0.1f, 4.0f); });
    case ScriptAttributeAccessorId::AudioMinDistance:
        return access_float(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.min_distance; },
            [](SceneObjectAttribute& attribute, float value) { attribute.audio.min_distance = (std::max)(0.01f, value); });
    case ScriptAttributeAccessorId::AudioMaxDistance:
        return access_float(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.max_distance; },
            [](SceneObjectAttribute& attribute, float value) { attribute.audio.max_distance = (std::max)(0.02f, value); });
    case ScriptAttributeAccessorId::AudioDopplerFactor:
        return access_float(SceneObjectAttributeKind::Audio,
            [](const SceneObjectAttribute& attribute) { return attribute.audio.doppler_factor; },
            [](SceneObjectAttribute& attribute, float value) { attribute.audio.doppler_factor = (std::max)(0.0f, value); });
    case ScriptAttributeAccessorId::Video2DVideoPath:
        return access_string(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.video_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.video_2d.video_path = value; });
    case ScriptAttributeAccessorId::Video2DPosition:
        return access_vec2(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.video_2d.x, attribute.video_2d.y}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.video_2d.x = x;
                attribute.video_2d.y = y;
            });
    case ScriptAttributeAccessorId::Video2DSize:
        return access_vec2(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return std::array<float, 2>{attribute.video_2d.width, attribute.video_2d.height}; },
            [](SceneObjectAttribute& attribute, float x, float y)
            {
                attribute.video_2d.width = (std::max)(1.0f, x);
                attribute.video_2d.height = (std::max)(1.0f, y);
            });
    case ScriptAttributeAccessorId::Video2DLockAspectRatio:
        return access_bool(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.lock_aspect_ratio; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.video_2d.lock_aspect_ratio = value; });
    case ScriptAttributeAccessorId::Video2DStretchToScreen:
        return access_bool(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.stretch_to_screen; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.video_2d.stretch_to_screen = value; });
    case ScriptAttributeAccessorId::Video2DTint:
        return access_vec3(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.tint; },
            [](SceneObjectAttribute& attribute, float x, float y, float z) { attribute.video_2d.tint = {x, y, z}; });
    case ScriptAttributeAccessorId::Video2DAlpha:
        return access_float(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.alpha; },
            [](SceneObjectAttribute& attribute, float value) { attribute.video_2d.alpha = std::clamp(value, 0.0f, 1.0f); });
    case ScriptAttributeAccessorId::Video2DPriority:
        return access_int(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.priority; },
            [](SceneObjectAttribute& attribute, int value) { attribute.video_2d.priority = value; });
    case ScriptAttributeAccessorId::Video2DPlayMode:
        return access_string(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) -> std::string {
                switch (attribute.video_2d.play_mode)
                {
                case SceneObjectVideoPlayMode::Loop: return "Loop";
                case SceneObjectVideoPlayMode::PlayOnce: return "PlayOnce";
                case SceneObjectVideoPlayMode::Off: default: return "Off";
                }
            },
            [](SceneObjectAttribute& attribute, const std::string& value) {
                if (value == "Loop" || value == "loop")
                {
                    attribute.video_2d.play_mode = SceneObjectVideoPlayMode::Loop;
                }
                else if (value == "PlayOnce" || value == "playonce" || value == "Once" || value == "once")
                {
                    attribute.video_2d.play_mode = SceneObjectVideoPlayMode::PlayOnce;
                }
                else
                {
                    attribute.video_2d.play_mode = SceneObjectVideoPlayMode::Off;
                }
            });
    case ScriptAttributeAccessorId::Video2DVolume:
        return access_float(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.volume; },
            [](SceneObjectAttribute& attribute, float value) { attribute.video_2d.volume = std::clamp(value, 0.0f, 20.0f); });
    case ScriptAttributeAccessorId::Video2DMuted:
        return access_bool(SceneObjectAttributeKind::Video2D,
            [](const SceneObjectAttribute& attribute) { return attribute.video_2d.muted; },
            [](SceneObjectAttribute& attribute, bool value) { attribute.video_2d.muted = value; });
    case ScriptAttributeAccessorId::EffectsEffectPath:
        return access_string(SceneObjectAttributeKind::Effects,
            [](const SceneObjectAttribute& attribute) { return attribute.effects.effect_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.effects.effect_path = value; });
    case ScriptAttributeAccessorId::EffectsPlayMode:
    {
        if (!is_setter)
        {
            SceneObjectAttribute* const attr = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Effects);
            if (attr == nullptr) { lua_pushnil(lua_state); return 1; }
            const SceneObjectEffectsPlayMode mode = attr->effects.trigger_mode;
            lua_pushstring(lua_state, (mode == SceneObjectEffectsPlayMode::PlayOnce) ? "PlayOnce" : "Loop");
            return 1;
        }
        const char* mode_str = luaL_checkstring(lua_state, 2);
        SceneObjectAttribute* const attr = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Effects);
        if (attr == nullptr) { lua_pushnil(lua_state); return 1; }
        attr->effects.trigger_mode = (std::strcmp(mode_str, "PlayOnce") == 0)
            ? SceneObjectEffectsPlayMode::PlayOnce
            : SceneObjectEffectsPlayMode::Loop;
        renderer->HandleScriptAttributeMutation(SceneObjectAttributeKind::Effects, accessor_id);
        lua_pushboolean(lua_state, 1);
        return 1;
    }
    default:
        return luaL_error(lua_state, "Unknown attribute accessor");
    }
}

bool RuntimeRenderer::IsControllerBindingActive(const input::ControllerBinding& binding) const
{
    for (SDL_Gamepad* pad : open_gamepads_)
    {
        if (pad == nullptr)
        {
            continue;
        }
        if (binding.kind == input::ControllerBinding::Kind::Button)
        {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(binding.index)))
            {
                return true;
            }
        }
        else
        {
            const Sint16 value = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(binding.index));
            const bool active = binding.positive
                ? (value > input::kAxisThreshold)
                : (value < -input::kAxisThreshold);
            if (active)
            {
                return true;
            }
        }
    }
    return false;
}

bool RuntimeRenderer::EffectiveKeyDown(SDL_Scancode scancode) const
{
    int num_keys = 0;
    const bool* keys = SDL_GetKeyboardState(&num_keys);
    if (static_cast<int>(scancode) < num_keys && keys[scancode])
    {
        return true;
    }

    const auto it = controller_mappings_.find(static_cast<int>(scancode));
    if (it != controller_mappings_.end())
    {
        for (const input::ControllerBinding& binding : it->second)
        {
            if (IsControllerBindingActive(binding))
            {
                return true;
            }
        }
    }
    return false;
}

int RuntimeRenderer::LuaInputIsKeyDown(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));

    const char* key_name = luaL_checkstring(lua_state, 1);
    const SDL_Scancode scancode = SDL_GetScancodeFromName(key_name);
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }

    bool is_down = false;
    if (renderer != nullptr)
    {
        is_down = renderer->EffectiveKeyDown(scancode);
    }
    else
    {
        int num_keys = 0;
        const bool* keys = SDL_GetKeyboardState(&num_keys);
        is_down = (static_cast<int>(scancode) < num_keys) && keys[scancode];
    }
    lua_pushboolean(lua_state, is_down ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaWorldLoadScene(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* scene_name = luaL_checkstring(lua_state, 1);
    if (scene_name == nullptr || scene_name[0] == '\0')
    {
        return luaL_error(lua_state, "World.LoadScene requires a non-empty scene name");
    }

    // Optional second argument: an options table. Currently supports
    //   loadingScene = "Name"  -- a lightweight scene shown while the target
    //                             streams its assets on a background thread.
    std::string loading_scene;
    if (lua_gettop(lua_state) >= 2 && lua_istable(lua_state, 2))
    {
        lua_getfield(lua_state, 2, "loadingScene");
        if (lua_isstring(lua_state, -1))
        {
            loading_scene = lua_tostring(lua_state, -1);
        }
        lua_pop(lua_state, 1);
    }

    renderer->pending_scene_load_path_ = scene_name;
    renderer->pending_scene_load_loading_scene_ = std::move(loading_scene);
    return 0;
}

int RuntimeRenderer::LuaWorldGetSceneLoadProgress(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    // 0..1 while a load is in flight; 1 when idle (nothing loading).
    const float progress = renderer->scene_load_in_progress_ ? renderer->scene_load_progress_ : 1.0f;
    lua_pushnumber(lua_state, static_cast<lua_Number>(progress));
    return 1;
}

int RuntimeRenderer::LuaWorldIsSceneLoading(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    lua_pushboolean(lua_state, renderer->scene_load_in_progress_ ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaInputWasKeyPressed(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* key_name = luaL_checkstring(lua_state, 1);
    const SDL_Scancode scancode = SDL_GetScancodeFromName(key_name);
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }

    const int idx = static_cast<int>(scancode);
    const bool is_down_now = renderer->EffectiveKeyDown(scancode);
    const bool was_down = (idx < static_cast<int>(renderer->script_prev_keys_down_.size())) && renderer->script_prev_keys_down_[idx];
    lua_pushboolean(lua_state, (is_down_now && !was_down) ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaInputMousePosition(lua_State* lua_state)
{
    float x = 0.0f;
    float y = 0.0f;
    SDL_GetMouseState(&x, &y);
    lua_pushnumber(lua_state, static_cast<lua_Number>(x));
    lua_pushnumber(lua_state, static_cast<lua_Number>(y));
    return 2;
}

float RuntimeRenderer::ControllerBindingMagnitude(const input::ControllerBinding& binding) const
{
    float magnitude = 0.0f;
    for (SDL_Gamepad* pad : open_gamepads_)
    {
        if (pad == nullptr)
        {
            continue;
        }
        if (binding.kind == input::ControllerBinding::Kind::Button)
        {
            if (SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(binding.index)))
            {
                magnitude = 1.0f;
            }
        }
        else
        {
            const Sint16 raw = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(binding.index));
            const int directional = binding.positive ? raw : -raw;
            if (directional > 0)
            {
                const float normalized = static_cast<float>(directional) / 32767.0f;
                magnitude = (normalized > magnitude) ? normalized : magnitude;
            }
        }
    }
    return magnitude;
}

void RuntimeRenderer::AddControllerMouseDelta(float& dx, float& dy) const
{
    if (controller_mappings_.empty() || open_gamepads_.empty())
    {
        return;
    }

    // Pixels of mouse motion per frame at full stick deflection.
    constexpr float kMouseLookSpeed = 18.0f;
    // Radial deadzone to stop a resting stick from drifting the cursor.
    constexpr float kDeadzone = 0.15f;

    const auto direction_amount = [this](int code) -> float
    {
        const auto it = controller_mappings_.find(code);
        if (it == controller_mappings_.end())
        {
            return 0.0f;
        }
        float amount = 0.0f;
        for (const input::ControllerBinding& binding : it->second)
        {
            const float m = ControllerBindingMagnitude(binding);
            amount = (m > amount) ? m : amount;
        }
        if (amount < kDeadzone)
        {
            return 0.0f;
        }
        return (amount - kDeadzone) / (1.0f - kDeadzone);
    };

    const float right = direction_amount(input::kMouseMoveRight);
    const float left  = direction_amount(input::kMouseMoveLeft);
    const float up    = direction_amount(input::kMouseMoveUp);
    const float down  = direction_amount(input::kMouseMoveDown);

    dx += (right - left) * kMouseLookSpeed;
    dy += (down - up) * kMouseLookSpeed;
}

int RuntimeRenderer::LuaInputMouseDelta(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));

    float dx = 0.0f;
    float dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
    if (renderer != nullptr)
    {
        renderer->AddControllerMouseDelta(dx, dy);
    }
    lua_pushnumber(lua_state, static_cast<lua_Number>(dx));
    lua_pushnumber(lua_state, static_cast<lua_Number>(dy));
    return 2;
}

int RuntimeRenderer::LuaWorldSubscribe(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    if (renderer->script_active_instance_key_.empty())
    {
        return luaL_error(lua_state, "World.Subscribe can only be called from a script callback");
    }

    const char* event_name = luaL_checkstring(lua_state, 1);
    luaL_checktype(lua_state, 2, LUA_TFUNCTION);

    lua_pushvalue(lua_state, 2);
    const int handler_ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);

    ScriptEventSubscription subscription;
    subscription.instance_key = renderer->script_active_instance_key_;
    subscription.handler_ref = handler_ref;
    renderer->script_event_subscriptions_[event_name].push_back(subscription);
    return 0;
}

int RuntimeRenderer::LuaWorldEmit(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* event_name_cstr = luaL_checkstring(lua_state, 1);
    const std::string event_name(event_name_cstr != nullptr ? event_name_cstr : "");
    const char* payload = luaL_optstring(lua_state, 2, nullptr);

    const auto event_it = renderer->script_event_subscriptions_.find(event_name);
    if (event_it == renderer->script_event_subscriptions_.end())
    {
        return 0;
    }

    const std::vector<ScriptEventSubscription> subscriptions = event_it->second;
    const std::string sender_name = renderer->script_active_object_name_;

    for (const ScriptEventSubscription& subscription : subscriptions)
    {
        const auto instance_it = renderer->script_instances_.find(subscription.instance_key);
        if (instance_it == renderer->script_instances_.end())
        {
            continue;
        }

        const RuntimeScriptInstance& instance = instance_it->second;
        lua_rawgeti(lua_state, LUA_REGISTRYINDEX, instance.table_ref);
        if (!lua_istable(lua_state, -1))
        {
            lua_pop(lua_state, 1);
            continue;
        }

        lua_rawgeti(lua_state, LUA_REGISTRYINDEX, subscription.handler_ref);
        if (!lua_isfunction(lua_state, -1))
        {
            lua_pop(lua_state, 2);
            continue;
        }

        lua_pushvalue(lua_state, -2);
        lua_pushstring(lua_state, sender_name.c_str());
        if (payload != nullptr)
        {
            lua_pushstring(lua_state, payload);
        }
        else
        {
            lua_pushnil(lua_state);
        }

        const std::string previous_instance_key = renderer->script_active_instance_key_;
        const std::string previous_object_name = renderer->script_active_object_name_;
        renderer->script_active_instance_key_ = instance.instance_key;
        renderer->script_active_object_name_ = instance.object_name;

        const int call_result = lua_pcall(lua_state, 3, 0, 0);
        renderer->script_active_instance_key_ = previous_instance_key;
        renderer->script_active_object_name_ = previous_object_name;

        if (call_result != LUA_OK)
        {
            const char* message = lua_tostring(lua_state, -1);
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "[Lua] World.Emit handler failed for event '%s': %s",
                event_name.c_str(),
                message != nullptr ? message : "unknown error");
            lua_pop(lua_state, 1);
        }

        lua_pop(lua_state, 1);
    }

    return 0;
}

int RuntimeRenderer::LuaWorldSpawn(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    const char* model_path = luaL_optstring(lua_state, 2, nullptr);
    const float x = static_cast<float>(luaL_optnumber(lua_state, 3, 0.0));
    const float y = static_cast<float>(luaL_optnumber(lua_state, 4, 0.0));
    const float z = static_cast<float>(luaL_optnumber(lua_state, 5, 0.0));
    const char* script_path = luaL_optstring(lua_state, 6, nullptr);

    RuntimeSpawnedObject object;
    object.name = object_name != nullptr ? object_name : "";
    object.model_path = model_path != nullptr ? model_path : "";
    object.script_path = script_path != nullptr ? script_path : "";
    object.position = {x, y, z};

    std::string error_message;
    if (!renderer->SpawnRuntimeObject(object, &error_message))
    {
        return luaL_error(lua_state, "%s", error_message.c_str());
    }

    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaWorldSpawnFromObject(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* source_name_cstr = luaL_checkstring(lua_state, 1);
    const char* object_name_cstr = luaL_checkstring(lua_state, 2);
    const float x = static_cast<float>(luaL_optnumber(lua_state, 3, 0.0));
    const float y = static_cast<float>(luaL_optnumber(lua_state, 4, 0.0));
    const float z = static_cast<float>(luaL_optnumber(lua_state, 5, 0.0));

    const std::string source_name = source_name_cstr != nullptr ? source_name_cstr : "";
    const std::string object_name = object_name_cstr != nullptr ? object_name_cstr : "";

    RuntimeSpawnedObject object;
    object.name = object_name;
    object.position = {x, y, z};

    bool source_found = false;

    const auto spawned_it = renderer->runtime_spawned_objects_.find(source_name);
    if (spawned_it != renderer->runtime_spawned_objects_.end())
    {
        object.model_path = spawned_it->second.model_path;
        object.script_path = spawned_it->second.script_path;
        object.model_visual_offset = spawned_it->second.model_visual_offset;
        object.rotation = spawned_it->second.rotation;
        object.scale = spawned_it->second.scale;
        source_found = true;
    }
    else
    {
        for (const SceneObjectMetadata& scene_object : renderer->cached_scene_metadata_.objects)
        {
            if (!scene_object.enabled_in_hierarchy)
            {
                continue;
            }

            if (scene_object.name != source_name)
            {
                continue;
            }

            object.model_path = scene_object.model_path;
            object.model_visual_offset = scene_object.model_visual_offset;
            object.rotation = scene_object.rotation;
            object.scale = scene_object.scale;
            if (!scene_object.script_paths.empty())
            {
                object.script_path = scene_object.script_paths.front();
            }
            source_found = true;
            break;
        }
    }

    if (!source_found)
    {
        return luaL_error(lua_state, "World.SpawnFromObject source not found: %s", source_name.c_str());
    }

    if (!lua_isnoneornil(lua_state, 6))
    {
        const char* script_override = luaL_checkstring(lua_state, 6);
        object.script_path = script_override != nullptr ? script_override : "";
    }

    std::string error_message;
    if (!renderer->SpawnRuntimeObject(object, &error_message))
    {
        return luaL_error(lua_state, "%s", error_message.c_str());
    }

    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaWorldSpawnPrefab(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* prefab_name_cstr = luaL_checkstring(lua_state, 1);
    const std::string prefab_name = prefab_name_cstr != nullptr ? prefab_name_cstr : "";

    const bool has_position =
        !lua_isnoneornil(lua_state, 2) &&
        !lua_isnoneornil(lua_state, 3) &&
        !lua_isnoneornil(lua_state, 4);

    SceneVector3 position_override = {0.0f, 0.0f, 0.0f};
    if (has_position)
    {
        position_override = {
            static_cast<float>(luaL_checknumber(lua_state, 2)),
            static_cast<float>(luaL_checknumber(lua_state, 3)),
            static_cast<float>(luaL_checknumber(lua_state, 4))};
    }

    const PrefabMetadata store = LoadPrefabMetadata(renderer->project_root_);
    if (!store.parsed)
    {
        return luaL_error(lua_state, "World.SpawnPrefab failed to load prefab store: %s",
            store.error_message.c_str());
    }

    const auto entry_it = std::find_if(store.prefabs.begin(), store.prefabs.end(),
        [&prefab_name](const PrefabEntry& e) { return e.name == prefab_name; });
    if (entry_it == store.prefabs.end())
    {
        return luaL_error(lua_state, "World.SpawnPrefab unknown prefab: %s", prefab_name.c_str());
    }

    const PrefabEntry& entry = *entry_it;

    // Parse the prefab body through the full SceneMetadata loader so every
    // attribute kind (Shader/Cloud, lights, 2D overlays, audio, rigidbodies,
    // animator, ...) is captured exactly as if the object had been authored
    // in a scene file. The runtime queue loop reads `attributes` to drive
    // procedural shader passes (e.g. is_cloud) and other attribute-based
    // rendering.
    SceneObjectMetadata root_metadata;
    std::string parse_error;
    if (!LoadPrefabRootMetadata(renderer->project_root_, prefab_name, &root_metadata, &parse_error))
    {
        return luaL_error(lua_state, "World.SpawnPrefab failed to parse prefab '%s': %s",
            prefab_name.c_str(), parse_error.c_str());
    }

    RuntimeSpawnedObject object;
    object.name                = root_metadata.name;
    object.model_path          = root_metadata.model_path;
    object.model_visual_offset = root_metadata.model_visual_offset;
    object.position            = root_metadata.position;
    object.rotation            = root_metadata.rotation;
    object.scale               = root_metadata.scale;
    object.tags                = root_metadata.tags;
    object.attributes          = root_metadata.attributes;
    if (!root_metadata.script_paths.empty())
    {
        object.script_path = root_metadata.script_paths.front();
    }

    if (has_position)
    {
        object.position = position_override;
    }

    // Uniquify the name against scene + already-spawned objects.
    const std::string base_name = entry.root_object_name.empty() ? prefab_name : entry.root_object_name;
    std::string candidate = base_name;
    int suffix = 1;
    while (renderer->RuntimeObjectExists(candidate))
    {
        candidate = base_name + "_" + std::to_string(suffix);
        ++suffix;
    }
    object.name = candidate;

    std::string error_message;
    if (!renderer->SpawnRuntimeObject(object, &error_message))
    {
        return luaL_error(lua_state, "%s", error_message.c_str());
    }

    lua_pushstring(lua_state, object.name.c_str());
    return 1;
}

int RuntimeRenderer::LuaWorldDestroy(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    renderer->DestroyRuntimeObject(object_name != nullptr ? object_name : "");
    return 0;
}

int RuntimeRenderer::LuaWorldDestroyByPrefix(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* prefix_cstr = luaL_checkstring(lua_state, 1);
    const std::string prefix = prefix_cstr != nullptr ? prefix_cstr : "";

    std::vector<std::string> to_destroy;
    to_destroy.reserve(renderer->cached_scene_metadata_.objects.size() + renderer->runtime_spawned_objects_.size());

    auto maybe_collect = [&](const std::string& name)
    {
        if (name.rfind(prefix, 0) != 0)
        {
            return;
        }

        if (renderer->runtime_destroyed_objects_.find(name) != renderer->runtime_destroyed_objects_.end())
        {
            return;
        }

        to_destroy.push_back(name);
    };

    for (const SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (!object.enabled_in_hierarchy)
        {
            continue;
        }

        maybe_collect(object.name);
    }

    for (const auto& [name, spawned] : renderer->runtime_spawned_objects_)
    {
        maybe_collect(name);
    }

    for (const std::string& name : to_destroy)
    {
        renderer->DestroyRuntimeObject(name);
    }

    lua_pushinteger(lua_state, static_cast<lua_Integer>(to_destroy.size()));
    return 1;
}

int RuntimeRenderer::LuaWorldExists(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_name = luaL_checkstring(lua_state, 1);
    lua_pushboolean(lua_state, renderer->RuntimeObjectExists(object_name != nullptr ? object_name : "") ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaWorldGetAll(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    std::vector<std::string> names;
    names.reserve(renderer->cached_scene_metadata_.objects.size() + renderer->runtime_spawned_objects_.size());

    for (const SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (!object.enabled_in_hierarchy)
        {
            continue;
        }

        if (renderer->runtime_destroyed_objects_.find(object.name) == renderer->runtime_destroyed_objects_.end())
        {
            names.push_back(object.name);
        }
    }

    for (const auto& [name, spawned] : renderer->runtime_spawned_objects_)
    {
        if (renderer->runtime_destroyed_objects_.find(name) == renderer->runtime_destroyed_objects_.end())
        {
            names.push_back(name);
        }
    }

    lua_newtable(lua_state);
    int lua_index = 1;
    for (const std::string& name : names)
    {
        lua_pushstring(lua_state, name.c_str());
        lua_rawseti(lua_state, -2, lua_index++);
    }

    return 1;
}

int RuntimeRenderer::LuaWorldFindByPrefix(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* prefix_cstr = luaL_checkstring(lua_state, 1);
    const std::string prefix = prefix_cstr != nullptr ? prefix_cstr : "";

    lua_newtable(lua_state);
    int lua_index = 1;

    auto maybe_add = [&](const std::string& name)
    {
        if (!prefix.empty() && name.rfind(prefix, 0) != 0)
        {
            return;
        }

        lua_pushstring(lua_state, name.c_str());
        lua_rawseti(lua_state, -2, lua_index++);
    };

    for (const SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (!object.enabled_in_hierarchy)
        {
            continue;
        }

        if (renderer->runtime_destroyed_objects_.find(object.name) != renderer->runtime_destroyed_objects_.end())
        {
            continue;
        }
        maybe_add(object.name);
    }

    for (const auto& [name, spawned] : renderer->runtime_spawned_objects_)
    {
        if (renderer->runtime_destroyed_objects_.find(name) != renderer->runtime_destroyed_objects_.end())
        {
            continue;
        }
        maybe_add(name);
    }

    return 1;
}

int RuntimeRenderer::LuaWorldFindByTag(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* tag_cstr = luaL_optstring(lua_state, 1, "");
    const std::string tag = tag_cstr != nullptr ? tag_cstr : "";

    lua_newtable(lua_state);
    int lua_index = 1;

    if (tag.empty())
    {
        return 1;
    }

    for (const SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (!object.enabled_in_hierarchy)
        {
            continue;
        }
        if (renderer->runtime_destroyed_objects_.find(object.name) != renderer->runtime_destroyed_objects_.end())
        {
            continue;
        }
        if (std::find(object.tags.begin(), object.tags.end(), tag) == object.tags.end())
        {
            continue;
        }
        lua_pushstring(lua_state, object.name.c_str());
        lua_rawseti(lua_state, -2, lua_index++);
    }

    for (const auto& [name, spawned] : renderer->runtime_spawned_objects_)
    {
        if (renderer->runtime_destroyed_objects_.find(name) != renderer->runtime_destroyed_objects_.end())
        {
            continue;
        }
        if (std::find(spawned.tags.begin(), spawned.tags.end(), tag) == spawned.tags.end())
        {
            continue;
        }
        lua_pushstring(lua_state, name.c_str());
        lua_rawseti(lua_state, -2, lua_index++);
    }

    return 1;
}

int RuntimeRenderer::LuaGetObjectTags(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* name_cstr = luaL_optstring(lua_state, 1, "");
    const std::string object_name = name_cstr != nullptr ? name_cstr : "";

    lua_newtable(lua_state);
    int lua_index = 1;

    if (object_name.empty())
    {
        return 1;
    }

    const std::vector<std::string>* tags_ptr = nullptr;
    for (const SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (object.name == object_name)
        {
            tags_ptr = &object.tags;
            break;
        }
    }
    if (tags_ptr == nullptr)
    {
        const auto it = renderer->runtime_spawned_objects_.find(object_name);
        if (it != renderer->runtime_spawned_objects_.end())
        {
            tags_ptr = &it->second.tags;
        }
    }
    if (tags_ptr != nullptr)
    {
        for (const std::string& tag : *tags_ptr)
        {
            lua_pushstring(lua_state, tag.c_str());
            lua_rawseti(lua_state, -2, lua_index++);
        }
    }
    return 1;
}

int RuntimeRenderer::LuaObjectHasTag(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* name_cstr = luaL_optstring(lua_state, 1, "");
    const char* tag_cstr = luaL_optstring(lua_state, 2, "");
    const std::string object_name = name_cstr != nullptr ? name_cstr : "";
    const std::string tag = tag_cstr != nullptr ? tag_cstr : "";

    if (object_name.empty() || tag.empty())
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }

    for (const SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (object.name == object_name)
        {
            lua_pushboolean(lua_state, std::find(object.tags.begin(), object.tags.end(), tag) != object.tags.end() ? 1 : 0);
            return 1;
        }
    }
    const auto it = renderer->runtime_spawned_objects_.find(object_name);
    if (it != renderer->runtime_spawned_objects_.end())
    {
        lua_pushboolean(lua_state, std::find(it->second.tags.begin(), it->second.tags.end(), tag) != it->second.tags.end() ? 1 : 0);
        return 1;
    }
    lua_pushboolean(lua_state, 0);
    return 1;
}

int RuntimeRenderer::LuaAddObjectTag(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* name_cstr = luaL_optstring(lua_state, 1, "");
    const char* tag_cstr = luaL_optstring(lua_state, 2, "");
    const std::string object_name = name_cstr != nullptr ? name_cstr : "";
    const std::string sanitized = SanitizeSceneObjectTag(tag_cstr != nullptr ? tag_cstr : "");

    if (object_name.empty() || sanitized.empty())
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }

    for (SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (object.name == object_name)
        {
            if (std::find(object.tags.begin(), object.tags.end(), sanitized) == object.tags.end())
            {
                object.tags.push_back(sanitized);
                lua_pushboolean(lua_state, 1);
                return 1;
            }
            lua_pushboolean(lua_state, 0);
            return 1;
        }
    }
    const auto it = renderer->runtime_spawned_objects_.find(object_name);
    if (it != renderer->runtime_spawned_objects_.end())
    {
        if (std::find(it->second.tags.begin(), it->second.tags.end(), sanitized) == it->second.tags.end())
        {
            it->second.tags.push_back(sanitized);
            lua_pushboolean(lua_state, 1);
            return 1;
        }
    }
    lua_pushboolean(lua_state, 0);
    return 1;
}

int RuntimeRenderer::LuaRemoveObjectTag(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* name_cstr = luaL_optstring(lua_state, 1, "");
    const char* tag_cstr = luaL_optstring(lua_state, 2, "");
    const std::string object_name = name_cstr != nullptr ? name_cstr : "";
    const std::string sanitized = SanitizeSceneObjectTag(tag_cstr != nullptr ? tag_cstr : "");

    if (object_name.empty() || sanitized.empty())
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }

    for (SceneObjectMetadata& object : renderer->cached_scene_metadata_.objects)
    {
        if (object.name == object_name)
        {
            const auto erase_it = std::remove(object.tags.begin(), object.tags.end(), sanitized);
            if (erase_it != object.tags.end())
            {
                object.tags.erase(erase_it, object.tags.end());
                lua_pushboolean(lua_state, 1);
                return 1;
            }
            lua_pushboolean(lua_state, 0);
            return 1;
        }
    }
    const auto it = renderer->runtime_spawned_objects_.find(object_name);
    if (it != renderer->runtime_spawned_objects_.end())
    {
        const auto erase_it = std::remove(it->second.tags.begin(), it->second.tags.end(), sanitized);
        if (erase_it != it->second.tags.end())
        {
            it->second.tags.erase(erase_it, it->second.tags.end());
            lua_pushboolean(lua_state, 1);
            return 1;
        }
    }
    lua_pushboolean(lua_state, 0);
    return 1;
}

int RuntimeRenderer::LuaWorldGetCollisions(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_filter_cstr = luaL_optstring(lua_state, 1, "");
    const char* phase_filter_cstr = luaL_optstring(lua_state, 2, "");
    const std::string object_filter = object_filter_cstr != nullptr ? object_filter_cstr : "";
    const std::string phase_filter = phase_filter_cstr != nullptr ? phase_filter_cstr : "";

    return PushFilteredCollisionsToLua(lua_state, renderer->script_frame_collision_events_, object_filter, phase_filter);
}

int RuntimeRenderer::LuaWorldGetCollisionsFor(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* object_filter_cstr = luaL_checkstring(lua_state, 1);
    const char* phase_filter_cstr = luaL_optstring(lua_state, 2, "");
    const std::string object_filter = object_filter_cstr != nullptr ? object_filter_cstr : "";
    const std::string phase_filter = phase_filter_cstr != nullptr ? phase_filter_cstr : "";
    return PushFilteredCollisionsToLua(lua_state, renderer->script_frame_collision_events_, object_filter, phase_filter);
}

int RuntimeRenderer::LuaWorldGetCollisionsByPhase(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const char* phase_filter_cstr = luaL_checkstring(lua_state, 1);
    const std::string phase_filter = phase_filter_cstr != nullptr ? phase_filter_cstr : "";
    return PushFilteredCollisionsToLua(lua_state, renderer->script_frame_collision_events_, "", phase_filter);
}

int RuntimeRenderer::LuaWorldSetTimeout(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    if (renderer->script_active_instance_key_.empty())
    {
        return luaL_error(lua_state, "World.SetTimeout can only be called from a script callback");
    }

    const float delay_seconds = static_cast<float>(luaL_checknumber(lua_state, 1));
    luaL_checktype(lua_state, 2, LUA_TFUNCTION);

    lua_pushvalue(lua_state, 2);
    const int callback_ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);

    ScriptTimer timer;
    timer.id = renderer->script_next_timer_id_++;
    timer.owner_instance_key = renderer->script_active_instance_key_;
    timer.callback_ref = callback_ref;
    timer.remaining_seconds = (std::max)(0.0f, delay_seconds);
    timer.interval_seconds = timer.remaining_seconds;
    timer.repeating = false;
    renderer->script_timers_.push_back(timer);

    lua_pushinteger(lua_state, static_cast<lua_Integer>(timer.id));
    return 1;
}

int RuntimeRenderer::LuaWorldSetInterval(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    if (renderer->script_active_instance_key_.empty())
    {
        return luaL_error(lua_state, "World.SetInterval can only be called from a script callback");
    }

    const float interval_seconds = static_cast<float>(luaL_checknumber(lua_state, 1));
    luaL_checktype(lua_state, 2, LUA_TFUNCTION);

    lua_pushvalue(lua_state, 2);
    const int callback_ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);

    ScriptTimer timer;
    timer.id = renderer->script_next_timer_id_++;
    timer.owner_instance_key = renderer->script_active_instance_key_;
    timer.callback_ref = callback_ref;
    timer.remaining_seconds = (std::max)(0.0f, interval_seconds);
    timer.interval_seconds = (std::max)(0.0001f, interval_seconds);
    timer.repeating = true;
    renderer->script_timers_.push_back(timer);

    lua_pushinteger(lua_state, static_cast<lua_Integer>(timer.id));
    return 1;
}

int RuntimeRenderer::LuaWorldClearTimer(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }

    const std::uint64_t timer_id = static_cast<std::uint64_t>(luaL_checkinteger(lua_state, 1));

    for (auto it = renderer->script_timers_.begin(); it != renderer->script_timers_.end(); ++it)
    {
        if (it->id != timer_id)
        {
            continue;
        }

        if (renderer->script_timer_update_in_progress_)
        {
            renderer->script_timer_pending_clear_.insert(timer_id);
            lua_pushboolean(lua_state, 1);
            return 1;
        }

        if (renderer->script_lua_state_ != nullptr && it->callback_ref != LUA_NOREF && it->callback_ref != LUA_REFNIL)
        {
            luaL_unref(renderer->script_lua_state_, LUA_REGISTRYINDEX, it->callback_ref);
        }
        renderer->script_timers_.erase(it);
        lua_pushboolean(lua_state, 1);
        return 1;
    }

    lua_pushboolean(lua_state, 0);
    return 1;
}

// (Audio API implementations follow in this file.)

int RuntimeRenderer::LuaAudioPlay(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Audio);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->audio.play_mode = SceneObjectAudioPlayMode::On;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaAudioStop(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Audio);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->audio.play_mode = SceneObjectAudioPlayMode::Off;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaAudioIsPlaying(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const std::string key = std::string(object_name) + "#0";
    const auto it = renderer->active_audio_sources_.find(key);
    const bool playing = (it != renderer->active_audio_sources_.end())
        && (it->second.handle != AudioEngine::kInvalidHandle)
        && renderer->audio_engine_.IsPlaying(it->second.handle);
    lua_pushboolean(lua_state, playing ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaAudioSetVolume(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const float volume = static_cast<float>(luaL_checknumber(lua_state, 2));
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Audio);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->audio.volume = volume;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaAudioSetPitch(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const float pitch = static_cast<float>(luaL_checknumber(lua_state, 2));
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Audio);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->audio.pitch = pitch;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaAudioSetLoop(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const bool loop = lua_toboolean(lua_state, 2) != 0;
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Audio);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->audio.loop = loop;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaVideoPlay(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Video2D);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    if (attribute->video_2d.play_mode == SceneObjectVideoPlayMode::Off)
    {
        attribute->video_2d.play_mode = SceneObjectVideoPlayMode::PlayOnce;
    }
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaVideoStop(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Video2D);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->video_2d.play_mode = SceneObjectVideoPlayMode::Off;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaVideoIsPlaying(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Video2D);
    const bool playing = (attribute != nullptr) && (attribute->video_2d.play_mode != SceneObjectVideoPlayMode::Off);
    lua_pushboolean(lua_state, playing ? 1 : 0);
    return 1;
}

int RuntimeRenderer::LuaVideoSetVolume(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const float volume = static_cast<float>(luaL_checknumber(lua_state, 2));
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Video2D);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->video_2d.volume = std::clamp(volume, 0.0f, 20.0f);
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaVideoSetMuted(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const bool muted = lua_toboolean(lua_state, 2) != 0;
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Video2D);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->video_2d.muted = muted;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaEffectPlay(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Effects);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    // Use the editor-configured trigger_mode (Loop or PlayOnce) — never autostarts
    attribute->effects.play_mode = attribute->effects.trigger_mode;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaEffectStop(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Effects);
    if (attribute == nullptr)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }
    attribute->effects.play_mode = SceneObjectEffectsPlayMode::Stop;
    lua_pushboolean(lua_state, 1);
    return 1;
}

int RuntimeRenderer::LuaEffectIsPlaying(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* object_name = luaL_checkstring(lua_state, 1);
    const SceneObjectAttribute* const attribute = renderer->FindScriptAttribute(object_name, SceneObjectAttributeKind::Effects);
    const bool playing = attribute != nullptr && attribute->effects.play_mode != SceneObjectEffectsPlayMode::Stop;
    lua_pushboolean(lua_state, playing ? 1 : 0);
    return 1;
}

// ---------------------------------------------------------------------------
// Window.* API helpers (SDL3). Free functions kept file-local; the callbacks
// below are RuntimeRenderer members so they can touch the window/settings state.
// ---------------------------------------------------------------------------
namespace
{
std::string NormalizeWindowMode(std::string mode)
{
    for (char& ch : mode)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return mode;
}

std::string CurrentWindowModeName(SDL_Window* window)
{
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    return (flags & SDL_WINDOW_FULLSCREEN) != 0 ? "borderless" : "windowed";
}

void ApplyWindowMode(SDL_Window* window, const std::string& mode)
{
    if (mode == "borderless")
    {
        // NULL fullscreen mode == borderless desktop fullscreen (stays at the
        // desktop refresh and composited, keeping the frame pacer effective).
        SDL_SetWindowFullscreenMode(window, nullptr);
        SDL_SetWindowFullscreen(window, true);
    }
    else // "windowed"
    {
        SDL_SetWindowFullscreen(window, false);
        SDL_SetWindowBordered(window, true);
    }
    SDL_SyncWindow(window);
}

bool ReadIniValue(const std::filesystem::path& path, const std::string& key, std::string& out_value)
{
    if (path.empty())
    {
        return false;
    }
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line))
    {
        const std::size_t eq = line.find('=');
        if (eq != std::string::npos && line.substr(0, eq) == key)
        {
            out_value = line.substr(eq + 1);
            return true;
        }
    }
    return false;
}
} // namespace

void RuntimeRenderer::PersistWindowSetting(const std::string& key, const std::string& value) const
{
    if (window_settings_path_.empty())
    {
        return;
    }

    std::vector<std::string> lines;
    bool replaced = false;
    {
        std::ifstream input(window_settings_path_);
        std::string line;
        while (std::getline(input, line))
        {
            const std::size_t eq = line.find('=');
            if (eq != std::string::npos && line.substr(0, eq) == key)
            {
                lines.push_back(key + "=" + value);
                replaced = true;
            }
            else
            {
                lines.push_back(line);
            }
        }
    }
    if (!replaced)
    {
        lines.push_back(key + "=" + value);
    }

    std::ofstream output(window_settings_path_, std::ios::trunc);
    if (!output)
    {
        SDL_Log("Window: failed to persist %s to %s", key.c_str(),
                window_settings_path_.string().c_str());
        return;
    }
    for (const std::string& out_line : lines)
    {
        output << out_line << '\n';
    }
}

int RuntimeRenderer::LuaWindowSetMode(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const std::string mode = NormalizeWindowMode(luaL_checkstring(lua_state, 1));
    if (mode != "windowed" && mode != "borderless")
    {
        return luaL_error(lua_state, "Window.SetMode: expected 'windowed' or 'borderless'");
    }
    if (renderer->presentation_window_ != nullptr)
    {
        ApplyWindowMode(renderer->presentation_window_, mode);
    }
    renderer->PersistWindowSetting("windowMode", mode);
    return 0;
}

int RuntimeRenderer::LuaWindowGetMode(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    if (renderer->presentation_window_ == nullptr)
    {
        lua_pushstring(lua_state, "windowed");
        return 1;
    }
    lua_pushstring(lua_state, CurrentWindowModeName(renderer->presentation_window_).c_str());
    return 1;
}

int RuntimeRenderer::LuaWindowSetSize(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const int width = static_cast<int>(luaL_checkinteger(lua_state, 1));
    const int height = static_cast<int>(luaL_checkinteger(lua_state, 2));
    if (width <= 0 || height <= 0)
    {
        return luaL_error(lua_state, "Window.SetSize: width and height must be positive");
    }
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_SetWindowSize(renderer->presentation_window_, width, height);
    }
    renderer->PersistWindowSetting("windowWidth", std::to_string(width));
    renderer->PersistWindowSetting("windowHeight", std::to_string(height));
    return 0;
}

int RuntimeRenderer::LuaWindowGetSize(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    if (renderer->presentation_window_ == nullptr)
    {
        lua_pushnil(lua_state);
        lua_pushnil(lua_state);
        return 2;
    }
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(renderer->presentation_window_, &width, &height);
    lua_pushinteger(lua_state, width);
    lua_pushinteger(lua_state, height);
    return 2;
}

int RuntimeRenderer::LuaWindowSetPosition(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const int x = static_cast<int>(luaL_checkinteger(lua_state, 1));
    const int y = static_cast<int>(luaL_checkinteger(lua_state, 2));
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_SetWindowPosition(renderer->presentation_window_, x, y);
    }
    return 0;
}

int RuntimeRenderer::LuaWindowCenter(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_SetWindowPosition(renderer->presentation_window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    return 0;
}

int RuntimeRenderer::LuaWindowMaximize(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_MaximizeWindow(renderer->presentation_window_);
    }
    return 0;
}

int RuntimeRenderer::LuaWindowMinimize(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_MinimizeWindow(renderer->presentation_window_);
    }
    return 0;
}

int RuntimeRenderer::LuaWindowRestore(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_RestoreWindow(renderer->presentation_window_);
    }
    return 0;
}

int RuntimeRenderer::LuaWindowSetResizable(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const bool resizable = lua_toboolean(lua_state, 1) != 0;
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_SetWindowResizable(renderer->presentation_window_, resizable);
    }
    return 0;
}

int RuntimeRenderer::LuaWindowSetTitle(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    const char* const title = luaL_checkstring(lua_state, 1);
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_SetWindowTitle(renderer->presentation_window_, title);
    }
    renderer->PersistWindowSetting("windowTitle", title);
    return 0;
}

int RuntimeRenderer::LuaWindowGetDesktopSize(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    SDL_DisplayID display = 0;
    if (renderer->presentation_window_ != nullptr)
    {
        display = SDL_GetDisplayForWindow(renderer->presentation_window_);
    }
    if (display == 0)
    {
        display = SDL_GetPrimaryDisplay();
    }
    const SDL_DisplayMode* const desktop = display != 0 ? SDL_GetDesktopDisplayMode(display) : nullptr;
    if (desktop == nullptr)
    {
        lua_pushnil(lua_state);
        lua_pushnil(lua_state);
        return 2;
    }
    lua_pushinteger(lua_state, desktop->w);
    lua_pushinteger(lua_state, desktop->h);
    return 2;
}

int RuntimeRenderer::LuaWindowGetDisplayCount(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    int count = 0;
    SDL_DisplayID* const displays = SDL_GetDisplays(&count);
    if (displays != nullptr)
    {
        SDL_free(displays);
    }
    lua_pushinteger(lua_state, count);
    return 1;
}

int RuntimeRenderer::LuaWindowSetDisplay(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    // 1-based index to match Lua conventions.
    const int index = static_cast<int>(luaL_checkinteger(lua_state, 1));
    int count = 0;
    SDL_DisplayID* const displays = SDL_GetDisplays(&count);
    if (displays == nullptr || index < 1 || index > count)
    {
        if (displays != nullptr)
        {
            SDL_free(displays);
        }
        return luaL_error(lua_state, "Window.SetDisplay: display index out of range");
    }
    const SDL_DisplayID id = displays[index - 1];
    if (renderer->presentation_window_ != nullptr)
    {
        SDL_SetWindowPosition(renderer->presentation_window_, SDL_WINDOWPOS_CENTERED_DISPLAY(id),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    }
    SDL_free(displays);
    // Persist 0-based index so GameApplication can index SDL_GetDisplays directly.
    renderer->PersistWindowSetting("displayIndex", std::to_string(index - 1));
    return 0;
}

int RuntimeRenderer::LuaWindowSetVsync(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    // Persist-only: present mode is chosen once at swapchain init, so this takes
    // effect on the next launch of the built game rather than live.
    const bool vsync = lua_toboolean(lua_state, 1) != 0;
    renderer->PersistWindowSetting("vsync", vsync ? "true" : "false");
    return 0;
}

int RuntimeRenderer::LuaWindowGetVsync(lua_State* lua_state)
{
    RuntimeRenderer* const renderer = static_cast<RuntimeRenderer*>(lua_touserdata(lua_state, lua_upvalueindex(1)));
    if (renderer == nullptr)
    {
        return luaL_error(lua_state, "Runtime renderer is unavailable");
    }
    // Reflects the persisted launch setting (defaults to on when unset).
    std::string value;
    bool vsync = true;
    if (ReadIniValue(renderer->window_settings_path_, "vsync", value))
    {
        vsync = value != "false" && value != "0";
    }
    lua_pushboolean(lua_state, vsync ? 1 : 0);
    return 1;
}
