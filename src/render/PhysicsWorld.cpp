#include "render/PhysicsWorld.h"
#include "assets/ModelAsset.h"

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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
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

namespace
{
JPH::Quat BuildQuaternionFromRotationBasis(const JPH::Vec3& axis_x, const JPH::Vec3& axis_y, const JPH::Vec3& axis_z)
{
    const float m00 = axis_x.GetX();
    const float m01 = axis_y.GetX();
    const float m02 = axis_z.GetX();
    const float m10 = axis_x.GetY();
    const float m11 = axis_y.GetY();
    const float m12 = axis_z.GetY();
    const float m20 = axis_x.GetZ();
    const float m21 = axis_y.GetZ();
    const float m22 = axis_z.GetZ();

    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;

    const float trace = m00 + m11 + m22;
    if (trace > 0.0f)
    {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        qw = 0.25f * s;
        qx = (m21 - m12) / s;
        qy = (m02 - m20) / s;
        qz = (m10 - m01) / s;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        qw = (m21 - m12) / s;
        qx = 0.25f * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    }
    else if (m11 > m22)
    {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25f * s;
        qz = (m12 + m21) / s;
    }
    else
    {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25f * s;
    }

    const float length = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    if (length <= 0.000001f)
    {
        return JPH::Quat::sIdentity();
    }

    const float inv_length = 1.0f / length;
    return JPH::Quat(qx * inv_length, qy * inv_length, qz * inv_length, qw * inv_length);
}
}

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
    const std::unordered_map<std::string, std::array<float, 16>>& world_matrices,
    const std::filesystem::path& project_root)
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

        // Extract translation, rotation, and scale from world matrix when available.
        SceneVector3 position = object.position;
        JPH::Quat rotation = JPH::Quat::sIdentity();
        SceneVector3 world_scale = object.scale;
        const auto mat_it = world_matrices.find(object.name);
        if (mat_it != world_matrices.end())
        {
            const std::array<float, 16>& matrix = mat_it->second;
            position = {matrix[12], matrix[13], matrix[14]};

            const JPH::Vec3 axis_x(matrix[0], matrix[1], matrix[2]);
            const JPH::Vec3 axis_y(matrix[4], matrix[5], matrix[6]);
            const JPH::Vec3 axis_z(matrix[8], matrix[9], matrix[10]);

            const float scale_x = (std::max)(0.0001f, std::sqrt(axis_x.GetX() * axis_x.GetX() + axis_x.GetY() * axis_x.GetY() + axis_x.GetZ() * axis_x.GetZ()));
            const float scale_y = (std::max)(0.0001f, std::sqrt(axis_y.GetX() * axis_y.GetX() + axis_y.GetY() * axis_y.GetY() + axis_y.GetZ() * axis_y.GetZ()));
            const float scale_z = (std::max)(0.0001f, std::sqrt(axis_z.GetX() * axis_z.GetX() + axis_z.GetY() * axis_z.GetY() + axis_z.GetZ() * axis_z.GetZ()));

            world_scale = {scale_x, scale_y, scale_z};
            rotation = BuildQuaternionFromRotationBasis(
                JPH::Vec3(axis_x.GetX() / scale_x, axis_x.GetY() / scale_x, axis_x.GetZ() / scale_x),
                JPH::Vec3(axis_y.GetX() / scale_y, axis_y.GetY() / scale_y, axis_y.GetZ() / scale_y),
                JPH::Vec3(axis_z.GetX() / scale_z, axis_z.GetY() / scale_z, axis_z.GetZ() / scale_z));
        }

        JPH::RefConst<JPH::Shape> shape;
        if (object.physics_shape == SceneObjectPhysicsShape::Box)
        {
            const JPH::Vec3 half_extent(
                std::max(0.01f, object.physics_half_extent[0] * std::abs(world_scale[0])),
                std::max(0.01f, object.physics_half_extent[1] * std::abs(world_scale[1])),
                std::max(0.01f, object.physics_half_extent[2] * std::abs(world_scale[2])));
            shape = new JPH::BoxShape(half_extent);
        }
        else if (object.physics_shape == SceneObjectPhysicsShape::Sphere)
        {
            const float uniform_scale = (std::max)(std::abs(world_scale[0]), (std::max)(std::abs(world_scale[1]), std::abs(world_scale[2])));
            shape = new JPH::SphereShape(std::max(0.01f, object.physics_radius * uniform_scale));
        }
        else if (object.physics_shape == SceneObjectPhysicsShape::Capsule)
        {
            const float radius_scale = (std::max)(std::abs(world_scale[0]), std::abs(world_scale[2]));
            const float scaled_radius = std::max(0.01f, object.physics_radius * radius_scale);
            const float scaled_half_height = std::max(0.0f, object.physics_capsule_half_height * std::abs(world_scale[1]));
            shape = new JPH::CapsuleShape(scaled_half_height, scaled_radius);
        }
        else if (object.physics_shape == SceneObjectPhysicsShape::Mesh)
        {
            if (object.model_path.empty())
            {
                continue;
            }

            ModelAsset model_asset = LoadModelAsset(object.model_path);
            if (!model_asset.loaded)
            {
                const std::filesystem::path rooted_model_path = project_root.empty()
                    ? std::filesystem::path(object.model_path)
                    : (project_root / object.model_path);
                model_asset = LoadModelAsset(rooted_model_path);
            }
            if (!model_asset.loaded)
            {
                continue;
            }

            const float sx = std::abs(world_scale[0]);
            const float sy = std::abs(world_scale[1]);
            const float sz = std::abs(world_scale[2]);

            JPH::VertexList vertices;
            JPH::IndexedTriangleList triangles;
            std::uint32_t total_vertex_count = 0;
            std::uint32_t total_triangle_count = 0;
            for (const ModelMeshAsset& mesh_asset : model_asset.meshes)
            {
                total_vertex_count += static_cast<std::uint32_t>(mesh_asset.vertices.size());
                total_triangle_count += static_cast<std::uint32_t>(mesh_asset.indices.size() / 3);
            }
            vertices.reserve(total_vertex_count);
            triangles.reserve(total_triangle_count);

            for (const ModelMeshAsset& mesh_asset : model_asset.meshes)
            {
                const std::uint32_t base_index = static_cast<std::uint32_t>(vertices.size());

                for (const ModelVertex& vertex : mesh_asset.vertices)
                {
                    vertices.emplace_back(
                        vertex.position[0] * sx,
                        vertex.position[1] * sy,
                        vertex.position[2] * sz);
                }

                const std::size_t index_count = mesh_asset.indices.size();
                for (std::size_t index = 0; index + 2 < index_count; index += 3)
                {
                    triangles.emplace_back(
                        base_index + mesh_asset.indices[index],
                        base_index + mesh_asset.indices[index + 1],
                        base_index + mesh_asset.indices[index + 2]);
                }
            }

            if (triangles.empty())
            {
                continue;
            }

            JPH::MeshShapeSettings mesh_settings(std::move(vertices), std::move(triangles));
            JPH::Shape::ShapeResult mesh_shape_result = mesh_settings.Create();
            if (!mesh_shape_result.IsValid())
            {
                continue;
            }

            shape = mesh_shape_result.Get();
        }
        else
        {
            continue;
        }

        const bool is_mesh_shape = object.physics_shape == SceneObjectPhysicsShape::Mesh;
        const bool is_trigger = object.physics_is_trigger;
        const bool is_dynamic = object.physics_is_dynamic && !is_mesh_shape && !is_trigger;
        const JPH::ObjectLayer layer = is_dynamic ? Layers::MOVING : Layers::NON_MOVING;
        const JPH::EMotionType motion = is_dynamic
            ? JPH::EMotionType::Dynamic
            : JPH::EMotionType::Static;

        JPH::BodyCreationSettings settings(
            shape,
            JPH::RVec3(position[0], position[1], position[2]),
            rotation,
            motion,
            layer);
        settings.mIsSensor = is_trigger;
        if (is_dynamic)
        {
            settings.mAllowedDOFs =
                JPH::EAllowedDOFs::TranslationX |
                JPH::EAllowedDOFs::TranslationY |
                JPH::EAllowedDOFs::TranslationZ;
            if (!object.physics_lock_rotation_x)
            {
                settings.mAllowedDOFs |= JPH::EAllowedDOFs::RotationX;
            }
            if (!object.physics_lock_rotation_y)
            {
                settings.mAllowedDOFs |= JPH::EAllowedDOFs::RotationY;
            }
            if (!object.physics_lock_rotation_z)
            {
                settings.mAllowedDOFs |= JPH::EAllowedDOFs::RotationZ;
            }
        }
        settings.mMotionQuality = is_dynamic
            ? JPH::EMotionQuality::LinearCast
            : JPH::EMotionQuality::Discrete;
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
    constexpr float target_step = 1.0f / 120.0f;
    const int collision_steps = (std::max)(1, (std::min)(4, static_cast<int>(std::ceil(clamped / target_step))));
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

std::unordered_map<std::string, PhysicsBodyTransform> PhysicsWorld::GetSimulatedTransforms() const
{
    std::unordered_map<std::string, PhysicsBodyTransform> result;
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
        const JPH::Quat rot = body_interface.GetRotation(id);
        result[name] = PhysicsBodyTransform{
            {static_cast<float>(pos.GetX()), static_cast<float>(pos.GetY()), static_cast<float>(pos.GetZ())},
            {rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW()}};
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
