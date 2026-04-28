#include "render/PhysicsWorld.h"

// Jolt headers — order matters
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>

#include <algorithm>
#include <cmath>

JPH_SUPPRESS_WARNINGS

// ---------------------------------------------------------------------------
// Object layers
// ---------------------------------------------------------------------------
namespace Layers
{
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::uint NUM_LAYERS        = 2;
}

namespace BroadPhaseLayers
{
    static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
    static constexpr JPH::BroadPhaseLayer MOVING(1);
    static constexpr JPH::uint NUM_LAYERS = 2;
}

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        switch (a)
        {
        case Layers::NON_MOVING: return b == Layers::MOVING;
        case Layers::MOVING:     return true;
        default:                 return false;
        }
    }
};

class BroadPhaseLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    BroadPhaseLayerInterfaceImpl()
    {
        object_to_broad_phase_[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        object_to_broad_phase_[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }

    JPH::uint GetNumBroadPhaseLayers() const override
    {
        return BroadPhaseLayers::NUM_LAYERS;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return object_to_broad_phase_[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return (static_cast<JPH::uint>(layer) < BroadPhaseLayers::NUM_LAYERS)
            ? (layer == BroadPhaseLayers::NON_MOVING ? "NON_MOVING" : "MOVING")
            : "INVALID";
    }
#endif

private:
    JPH::BroadPhaseLayer object_to_broad_phase_[Layers::NUM_LAYERS]{};
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp_layer) const override
    {
        switch (layer)
        {
        case Layers::NON_MOVING: return bp_layer == BroadPhaseLayers::MOVING;
        case Layers::MOVING:     return true;
        default:                 return false;
        }
    }
};

class PhysicsWorldContactListener final : public JPH::ContactListener
{
public:
    explicit PhysicsWorldContactListener(PhysicsWorld* world)
        : world_(world)
    {
    }

    JPH::ValidateResult OnContactValidate(
        const JPH::Body& body1,
        const JPH::Body& body2,
        JPH::RVec3Arg base_offset,
        const JPH::CollideShapeResult& collision_result) override
    {
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(
        const JPH::Body& body1,
        const JPH::Body& body2,
        const JPH::ContactManifold& manifold,
        JPH::ContactSettings& settings) override
    {
        if (world_ != nullptr)
        {
            world_->QueueCollisionEvent(
                body1.GetID().GetIndexAndSequenceNumber(),
                body2.GetID().GetIndexAndSequenceNumber(),
                "enter");
        }
    }

    void OnContactPersisted(
        const JPH::Body& body1,
        const JPH::Body& body2,
        const JPH::ContactManifold& manifold,
        JPH::ContactSettings& settings) override
    {
        if (world_ != nullptr)
        {
            world_->QueueCollisionEvent(
                body1.GetID().GetIndexAndSequenceNumber(),
                body2.GetID().GetIndexAndSequenceNumber(),
                "stay");
        }
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
    {
        if (world_ != nullptr)
        {
            world_->QueueCollisionEvent(
                pair.GetBody1ID().GetIndexAndSequenceNumber(),
                pair.GetBody2ID().GetIndexAndSequenceNumber(),
                "exit");
        }
    }

private:
    PhysicsWorld* world_ = nullptr;
};

// ---------------------------------------------------------------------------
// Global Jolt resources shared across all PhysicsWorld instances
// ---------------------------------------------------------------------------
static int g_jolt_ref_count = 0;
static BroadPhaseLayerInterfaceImpl      g_broad_phase_layer_interface;
static ObjectVsBroadPhaseLayerFilterImpl g_object_vs_broad_phase_filter;
static ObjectLayerPairFilterImpl         g_object_layer_pair_filter;

// ---------------------------------------------------------------------------
// PhysicsWorld
// ---------------------------------------------------------------------------
PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld()
{
    Shutdown();
}

bool PhysicsWorld::Initialize(std::string* error_message)
{
    if (initialized_)
    {
        return true;
    }

    if (g_jolt_ref_count == 0)
    {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ++g_jolt_ref_count;

    temp_allocator_ = new JPH::TempAllocatorImpl(10 * 1024 * 1024); // 10 MB
    job_system_ = new JPH::JobSystemThreadPool(
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        static_cast<int>(std::thread::hardware_concurrency()) - 1);

    constexpr JPH::uint max_bodies             = 4096;
    constexpr JPH::uint num_body_mutexes       = 0;  // auto
    constexpr JPH::uint max_body_pairs         = 65536;
    constexpr JPH::uint max_contact_constraints = 10240;

    physics_system_ = new JPH::PhysicsSystem();
    physics_system_->Init(
        max_bodies,
        num_body_mutexes,
        max_body_pairs,
        max_contact_constraints,
        g_broad_phase_layer_interface,
        g_object_vs_broad_phase_filter,
        g_object_layer_pair_filter);

    contact_listener_ = new PhysicsWorldContactListener(this);
    physics_system_->SetContactListener(contact_listener_);

    initialized_ = true;
    return true;
}

void PhysicsWorld::Shutdown()
{
    if (!initialized_)
    {
        return;
    }

    body_records_.clear();
    body_id_to_name_.clear();

    {
        std::lock_guard<std::mutex> lock(collision_events_mutex_);
        collision_events_.clear();
    }

    delete contact_listener_;
    contact_listener_ = nullptr;

    delete physics_system_;
    physics_system_ = nullptr;

    delete job_system_;
    job_system_ = nullptr;

    delete temp_allocator_;
    temp_allocator_ = nullptr;

    --g_jolt_ref_count;
    if (g_jolt_ref_count == 0)
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    initialized_ = false;
}

void PhysicsWorld::BuildFromScene(
    const SceneMetadata& scene_metadata,
    const std::unordered_map<std::string, std::array<float, 16>>& world_matrices)
{
    if (!initialized_)
    {
        return;
    }

    // Remove all existing bodies
    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    for (auto& [name, record] : body_records_)
    {
        const JPH::BodyID id(record.body_id_value);
        if (!id.IsInvalid())
        {
            body_interface.RemoveBody(id);
            body_interface.DestroyBody(id);
        }
    }
    body_records_.clear();
    body_id_to_name_.clear();

    {
        std::lock_guard<std::mutex> lock(collision_events_mutex_);
        collision_events_.clear();
    }

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (object.physics_shape == SceneObjectPhysicsShape::None)
        {
            continue;
        }

        // Extract translation from world matrix or fall back to object position
        SceneVector3 position = object.position;
        const auto mat_it = world_matrices.find(object.name);
        if (mat_it != world_matrices.end())
        {
            position = {mat_it->second[12], mat_it->second[13], mat_it->second[14]};
        }

        JPH::RefConst<JPH::Shape> shape;
        if (object.physics_shape == SceneObjectPhysicsShape::Box)
        {
            const JPH::Vec3 half_extent(
                std::max(0.01f, object.physics_half_extent[0]),
                std::max(0.01f, object.physics_half_extent[1]),
                std::max(0.01f, object.physics_half_extent[2]));
            shape = new JPH::BoxShape(half_extent);
        }
        else if (object.physics_shape == SceneObjectPhysicsShape::Sphere)
        {
            shape = new JPH::SphereShape(std::max(0.01f, object.physics_radius));
        }
        else
        {
            continue;
        }

        const bool is_dynamic = object.physics_is_dynamic;
        const JPH::ObjectLayer layer = is_dynamic ? Layers::MOVING : Layers::NON_MOVING;
        const JPH::EMotionType motion = is_dynamic
            ? JPH::EMotionType::Dynamic
            : JPH::EMotionType::Static;

        JPH::BodyCreationSettings settings(
            shape,
            JPH::RVec3(position[0], position[1], position[2]),
            JPH::Quat::sIdentity(),
            motion,
            layer);
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = std::max(0.001f, object.physics_mass);
        settings.mLinearDamping  = object.physics_linear_damping;
        settings.mAngularDamping = object.physics_angular_damping;
        settings.mGravityFactor  = 1.0f;
        settings.mFriction = (std::max)(0.0f, object.physics_friction);
        settings.mRestitution = 0.0f;

        const JPH::BodyID id = body_interface.CreateAndAddBody(
            settings,
            is_dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

        if (id.IsInvalid())
        {
            continue;
        }

        PhysicsBodyRecord record;
        record.object_name   = object.name;
        record.body_id_value = id.GetIndexAndSequenceNumber();
        record.is_dynamic    = is_dynamic;

        body_records_[object.name]           = record;
        body_id_to_name_[record.body_id_value] = object.name;
    }

    physics_system_->OptimizeBroadPhase();
}

void PhysicsWorld::Step(float delta_time)
{
    if (!initialized_ || delta_time <= 0.0f)
    {
        return;
    }

    const float clamped = std::min(delta_time, 0.1f);
    constexpr int collision_steps = 1;
    physics_system_->Update(clamped, collision_steps, temp_allocator_, job_system_);
}

PhysicsRaycastHit PhysicsWorld::Raycast(
    const SceneVector3& origin,
    const SceneVector3& direction,
    float max_distance) const
{
    PhysicsRaycastHit result;
    if (!initialized_)
    {
        return result;
    }

    const JPH::Vec3 dir(direction[0], direction[1], direction[2]);
    const float dir_len = dir.Length();
    if (dir_len < 1e-6f)
    {
        return result;
    }

    const JPH::RRayCast ray(
        JPH::RVec3(origin[0], origin[1], origin[2]),
        (dir / dir_len) * max_distance);

    JPH::RayCastResult hit_result;
    if (!physics_system_->GetNarrowPhaseQuery().CastRay(ray, hit_result))
    {
        return result;
    }

    const std::uint32_t body_id_val = hit_result.mBodyID.GetIndexAndSequenceNumber();
    const auto name_it = body_id_to_name_.find(body_id_val);
    if (name_it == body_id_to_name_.end())
    {
        return result;
    }

    result.hit = true;
    result.object_name = name_it->second;
    result.distance    = hit_result.mFraction * max_distance;

    const JPH::Vec3 hit_pos = ray.GetPointOnRay(hit_result.mFraction);
    result.position = {hit_pos.GetX(), hit_pos.GetY(), hit_pos.GetZ()};

    // Surface normal from body
    JPH::BodyLockRead lock(physics_system_->GetBodyLockInterface(), hit_result.mBodyID);
    if (lock.Succeeded())
    {
        const JPH::Body& body = lock.GetBody();
        const JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(
            hit_result.mSubShapeID2,
            hit_pos);
        result.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
    }

    return result;
}

void PhysicsWorld::SetLinearVelocity(const std::string& object_name, const SceneVector3& velocity)
{
    if (!initialized_)
    {
        return;
    }

    const auto it = body_records_.find(object_name);
    if (it == body_records_.end() || !it->second.is_dynamic)
    {
        return;
    }

    const JPH::BodyID id(it->second.body_id_value);
    physics_system_->GetBodyInterface().ActivateBody(id);
    physics_system_->GetBodyInterface().SetLinearVelocity(
        id,
        JPH::Vec3(velocity[0], velocity[1], velocity[2]));
}

bool PhysicsWorld::GetLinearVelocity(const std::string& object_name, SceneVector3& velocity) const
{
    if (!initialized_)
    {
        return false;
    }

    const auto it = body_records_.find(object_name);
    if (it == body_records_.end())
    {
        return false;
    }

    const JPH::BodyID id(it->second.body_id_value);
    const JPH::Vec3 v = physics_system_->GetBodyInterface().GetLinearVelocity(id);
    velocity = {v.GetX(), v.GetY(), v.GetZ()};
    return true;
}

void PhysicsWorld::AddImpulse(const std::string& object_name, const SceneVector3& impulse)
{
    if (!initialized_)
    {
        return;
    }

    const auto it = body_records_.find(object_name);
    if (it == body_records_.end() || !it->second.is_dynamic)
    {
        return;
    }

    const JPH::BodyID id(it->second.body_id_value);
    physics_system_->GetBodyInterface().ActivateBody(id);
    physics_system_->GetBodyInterface().AddImpulse(
        id,
        JPH::Vec3(impulse[0], impulse[1], impulse[2]));
}

void PhysicsWorld::AddForce(const std::string& object_name, const SceneVector3& force)
{
    if (!initialized_)
    {
        return;
    }

    const auto it = body_records_.find(object_name);
    if (it == body_records_.end() || !it->second.is_dynamic)
    {
        return;
    }

    const JPH::BodyID id(it->second.body_id_value);
    physics_system_->GetBodyInterface().ActivateBody(id);
    physics_system_->GetBodyInterface().AddForce(
        id,
        JPH::Vec3(force[0], force[1], force[2]));
}

std::unordered_map<std::string, SceneVector3> PhysicsWorld::GetSimulatedPositions() const
{
    std::unordered_map<std::string, SceneVector3> result;
    if (!initialized_)
    {
        return result;
    }

    const JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    for (const auto& [name, record] : body_records_)
    {
        if (!record.is_dynamic)
        {
            continue;
        }

        const JPH::BodyID id(record.body_id_value);
        if (id.IsInvalid())
        {
            continue;
        }

        const JPH::RVec3 pos = body_interface.GetPosition(id);
        result[name] = {
            static_cast<float>(pos.GetX()),
            static_cast<float>(pos.GetY()),
            static_cast<float>(pos.GetZ())};
    }
    return result;
}

std::vector<PhysicsCollisionEvent> PhysicsWorld::ConsumeCollisionEvents()
{
    std::lock_guard<std::mutex> lock(collision_events_mutex_);
    std::vector<PhysicsCollisionEvent> events;
    events.swap(collision_events_);
    return events;
}

void PhysicsWorld::QueueCollisionEvent(std::uint32_t body_a, std::uint32_t body_b, const char* phase)
{
    std::string name_a;
    std::string name_b;
    if (!TryGetObjectNameByBodyId(body_a, name_a) || !TryGetObjectNameByBodyId(body_b, name_b))
    {
        return;
    }

    PhysicsCollisionEvent event;
    event.object_a = name_a;
    event.object_b = name_b;
    event.phase = phase != nullptr ? phase : "stay";

    std::lock_guard<std::mutex> lock(collision_events_mutex_);
    collision_events_.push_back(std::move(event));
}

bool PhysicsWorld::TryGetObjectNameByBodyId(std::uint32_t body_id, std::string& object_name) const
{
    const auto it = body_id_to_name_.find(body_id);
    if (it == body_id_to_name_.end())
    {
        return false;
    }

    object_name = it->second;
    return true;
}
