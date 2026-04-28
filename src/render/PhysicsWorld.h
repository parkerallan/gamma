#pragma once

#include "assets/SceneMetadata.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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

    // Called at the start of a play session — populates bodies from scene metadata
    void BuildFromScene(const SceneMetadata& scene_metadata,
                        const std::unordered_map<std::string, std::array<float, 16>>& world_matrices,
                        const std::filesystem::path& project_root = {});

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

    mutable std::mutex collision_events_mutex_;
    std::vector<PhysicsCollisionEvent> collision_events_;

    void QueueCollisionEvent(std::uint32_t body_a, std::uint32_t body_b, const char* phase);
    bool TryGetObjectNameByBodyId(std::uint32_t body_id, std::string& object_name) const;

    friend class PhysicsWorldContactListener;
};
