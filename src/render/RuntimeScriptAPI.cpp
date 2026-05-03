#include "render/RuntimeRenderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

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
    case ScriptAttributeAccessorId::SkyboxImagePath:
        return access_string(SceneObjectAttributeKind::Skybox,
            [](const SceneObjectAttribute& attribute) { return attribute.skybox.image_path; },
            [](SceneObjectAttribute& attribute, const std::string& value) { attribute.skybox.image_path = value; });
    case ScriptAttributeAccessorId::SkyboxRotation:
        return access_float(SceneObjectAttributeKind::Skybox,
            [](const SceneObjectAttribute& attribute) { return attribute.skybox.rotation_degrees; },
            [](SceneObjectAttribute& attribute, float value) { attribute.skybox.rotation_degrees = value; });
    default:
        return luaL_error(lua_state, "Unknown attribute accessor");
    }
}

int RuntimeRenderer::LuaInputIsKeyDown(lua_State* lua_state)
{
    const char* key_name = luaL_checkstring(lua_state, 1);
    const SDL_Scancode scancode = SDL_GetScancodeFromName(key_name);
    if (scancode == SDL_SCANCODE_UNKNOWN)
    {
        lua_pushboolean(lua_state, 0);
        return 1;
    }

    int num_keys = 0;
    const bool* keys = SDL_GetKeyboardState(&num_keys);
    const bool is_down = (static_cast<int>(scancode) < num_keys) && keys[scancode];
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

    renderer->pending_scene_load_path_ = scene_name;
    return 0;
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

    int num_keys = 0;
    const bool* keys = SDL_GetKeyboardState(&num_keys);
    const int idx = static_cast<int>(scancode);
    const bool is_down_now = (idx < num_keys) && keys[idx];
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

int RuntimeRenderer::LuaInputMouseDelta(lua_State* lua_state)
{
    float dx = 0.0f;
    float dy = 0.0f;
    SDL_GetRelativeMouseState(&dx, &dy);
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
