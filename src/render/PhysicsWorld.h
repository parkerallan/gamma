#pragma once

#include "assets/SceneMetadata.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ModelAsset;

// Forward declarations — callers don't need Jolt headers
namespace JPH
{
    class PhysicsSystem;
    class TempAllocatorImpl;
    class JobSystemThreadPool;
    class ContactListener;
    class Body;
    class BodyID;
}

struct PhysicsRaycastHit
{
    bool hit = false;
    std::string object_name;
    float distance = 0.0f;
    SceneVector3 position = {};
    SceneVector3 normal = {};
};

struct PhysicsCollisionEvent
{
    std::string object_a;
    std::string object_b;
    std::string phase; // enter | stay | exit
    // When a side is an animated bone collider, the bone name; empty for
    // ordinary scene-object bodies. object_a/object_b carry the owning
    // object's name in that case so existing routing keeps working.
    std::string a_bone;
    std::string b_bone;
};

struct PhysicsBodyTransform
{
    SceneVector3 position = {};
    std::array<float, 4> rotation = {0.0f, 0.0f, 0.0f, 1.0f};
};

class PhysicsWorld
{
public:
    PhysicsWorld();
    ~PhysicsWorld();

    // Non-copyable
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool Initialize(std::string* error_message = nullptr);
    void Shutdown();

    // Called at the start of a play session — populates bodies from scene metadata.
    // model_resolver, if provided, returns a cached ModelAsset* for a given absolute
    // model path so PhysicsWorld doesn't re-parse meshes via Assimp on every rebuild.
    using ModelAssetResolver = std::function<const ModelAsset*(const std::filesystem::path&)>;
    void BuildFromScene(const SceneMetadata& scene_metadata,
                        const std::unordered_map<std::string, std::array<float, 16>>& world_matrices,
                        const std::filesystem::path& project_root = {},
                        const ModelAssetResolver& model_resolver = {});

    // ---- Animated bone colliders -----------------------------------------
    // Kinematic boxes attached to skeleton bones, driven each frame from the
    // animated pose. Trigger colliders are sensors that fire the owning
    // object's OnTrigger* script callbacks; rigidbody colliders are solid
    // kinematic bodies that push dynamic rigidbodies.
    struct BoneColliderDef
    {
        std::string key;             // unique id, e.g. "<object>#<occ>#<bone>"
        std::string owner_object;    // object that owns the animator (event routing)
        std::string bone_name;       // bone the collider rides; surfaced to scripts
        bool is_trigger = true;      // true = sensor, false = solid kinematic
        std::array<float, 3> half_extents = {0.05f, 0.05f, 0.05f};
        PhysicsBodyTransform transform; // initial world-space pose (box-centered)
    };

    // Rebuilds the bone-collider body set, destroying any previously created
    // bone bodies. Safe to call with an empty list (clears them).
    void SetBoneColliders(const std::vector<BoneColliderDef>& colliders);

    // Drives each bone collider toward its new world pose. delta_time is used
    // for velocity-based kinematic motion so rigidbody colliders impart a push;
    // a non-positive delta falls back to a teleport. Call before Step().
    void UpdateBoneColliderTransforms(
        const std::unordered_map<std::string, PhysicsBodyTransform>& transforms,
        float delta_time);

    // Removes and destroys all bone-collider bodies.
    void ClearBoneColliders();

    // Returns a world-space translation that pushes an oriented box out of any
    // overlapping STATIC (NON_MOVING) geometry, or zero if it isn't
    // penetrating. Bodies belonging to ignore_owner (its main body and its
    // bone colliders) are excluded so a bone doesn't depenetrate against its
    // own character. Used to stop Rigidbody bone colliders at walls.
    SceneVector3 ComputeBoxDepenetration(
        const SceneVector3& center,
        const std::array<float, 4>& rotation,
        const SceneVector3& half_extents,
        const std::string& ignore_owner,
        // Diagnostics (optional): whether any static surface was within the
        // query's separation distance, and its signed depth (positive =
        // penetrating, negative = separated by that distance).
        bool* out_any_hit = nullptr,
        float* out_nearest_depth = nullptr) const;

    // Sweeps an oriented box from `from` to `to` against STATIC geometry and
    // returns the furthest non-penetrating center along the way, sliding the
    // remaining motion along the hit surface. This is the tunnel-proof query
    // used to stop Rigidbody bone colliders at walls: unlike a single-frame
    // depenetration, a sweep cannot skip past a hollow mesh's surface
    // triangles. Bodies belonging to ignore_owner are excluded.
    SceneVector3 ResolveBoxSweep(
        const SceneVector3& from,
        const SceneVector3& to,
        const std::array<float, 4>& rotation,
        const SceneVector3& half_extents,
        const std::string& ignore_owner) const;

    // Step the simulation. delta_time is clamped to a sane maximum.
    void Step(float delta_time);

    // Query — returns hit info for nearest object along ray
    PhysicsRaycastHit Raycast(const SceneVector3& origin,
                              const SceneVector3& direction,
                              float max_distance = 1000.0f) const;

    // Rigidbody setters/getters (operate on dynamic bodies only)
    void SetLinearVelocity(const std::string& object_name, const SceneVector3& velocity);
    bool GetLinearVelocity(const std::string& object_name, SceneVector3& velocity) const;

    void AddImpulse(const std::string& object_name, const SceneVector3& impulse);
    void AddForce(const std::string& object_name, const SceneVector3& force);

    // Reads simulated positions back into the override maps each frame.
    // Returns a map of object_name -> new world position for dynamic bodies.
    std::unordered_map<std::string, SceneVector3> GetSimulatedPositions() const;
    std::unordered_map<std::string, PhysicsBodyTransform> GetSimulatedTransforms() const;

    // Returns queued collision events and clears the queue.
    std::vector<PhysicsCollisionEvent> ConsumeCollisionEvents();

    bool IsInitialized() const { return initialized_; }

private:
    struct PhysicsBodyRecord
    {
        std::string object_name;
        std::uint32_t body_id_value = 0xFFFFFFFF;
        bool is_dynamic = false;
    };

    bool initialized_ = false;

    JPH::TempAllocatorImpl* temp_allocator_ = nullptr;
    JPH::JobSystemThreadPool* job_system_ = nullptr;
    JPH::PhysicsSystem* physics_system_ = nullptr;
    JPH::ContactListener* contact_listener_ = nullptr;

    std::unordered_map<std::string, PhysicsBodyRecord> body_records_;
    // Reverse map: body id -> object name for raycast lookups
    std::unordered_map<std::uint32_t, std::string> body_id_to_name_;

    struct BoneColliderRecord
    {
        std::uint32_t body_id_value = 0xFFFFFFFF;
        std::string owner_object;
        std::string bone_name;
        bool is_trigger = true;
    };
    // Keyed by BoneColliderDef::key, plus a body-id reverse map so collision
    // events can resolve a bone body to its owner object + bone name.
    std::unordered_map<std::string, BoneColliderRecord> bone_collider_records_;
    std::unordered_map<std::uint32_t, std::string> bone_body_id_to_key_;

    mutable std::mutex collision_events_mutex_;
    std::vector<PhysicsCollisionEvent> collision_events_;

    void QueueCollisionEvent(std::uint32_t body_a, std::uint32_t body_b, const char* phase);
    bool TryGetObjectNameByBodyId(std::uint32_t body_id, std::string& object_name) const;

    friend class PhysicsWorldContactListener;
};
