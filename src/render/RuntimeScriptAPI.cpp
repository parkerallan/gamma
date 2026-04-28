#include "render/RuntimeRenderer.h"

#include <SDL3/SDL.h>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

namespace
{
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
