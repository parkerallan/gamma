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
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

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

    bone_collider_records_.clear();
    bone_body_id_to_key_.clear();
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
    const std::filesystem::path& project_root,
    const ModelAssetResolver& model_resolver)
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
    // Drop any bone colliders from a previous play session before rebuilding;
    // RuntimeRenderer re-registers them via SetBoneColliders afterward.
    ClearBoneColliders();

    {
        std::lock_guard<std::mutex> lock(collision_events_mutex_);
        collision_events_.clear();
    }

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (!object.enabled_in_hierarchy)
        {
            continue;
        }

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

            // Prefer a caller-supplied resolver (e.g. RuntimeRenderer's model cache)
            // to avoid reparsing the same model with Assimp on every physics rebuild.
            const std::filesystem::path rooted_model_path = project_root.empty()
                ? std::filesystem::path(object.model_path)
                : (project_root / object.model_path);

            const ModelAsset* resolved_asset = nullptr;
            ModelAsset owned_asset;
            if (model_resolver)
            {
                resolved_asset = model_resolver(rooted_model_path);
                if (resolved_asset == nullptr || !resolved_asset->loaded)
                {
                    resolved_asset = model_resolver(object.model_path);
                }
            }
            if (resolved_asset == nullptr || !resolved_asset->loaded)
            {
                owned_asset = LoadModelAsset(object.model_path);
                if (!owned_asset.loaded)
                {
                    owned_asset = LoadModelAsset(rooted_model_path);
                }
                if (!owned_asset.loaded)
                {
                    continue;
                }
                resolved_asset = &owned_asset;
            }
            const ModelAsset& model_asset = *resolved_asset;

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

void PhysicsWorld::ClearBoneColliders()
{
    if (!initialized_ || bone_collider_records_.empty())
    {
        bone_collider_records_.clear();
        bone_body_id_to_key_.clear();
        return;
    }

    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    for (const auto& [key, record] : bone_collider_records_)
    {
        const JPH::BodyID id(record.body_id_value);
        if (!id.IsInvalid())
        {
            body_interface.RemoveBody(id);
            body_interface.DestroyBody(id);
        }
        body_id_to_name_.erase(record.body_id_value);
    }
    bone_collider_records_.clear();
    bone_body_id_to_key_.clear();
}

void PhysicsWorld::SetBoneColliders(const std::vector<BoneColliderDef>& colliders)
{
    if (!initialized_)
    {
        return;
    }

    ClearBoneColliders();

    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    for (const BoneColliderDef& def : colliders)
    {
        if (def.key.empty())
        {
            continue;
        }

        const float hx = std::max(0.01f, def.half_extents[0]);
        const float hy = std::max(0.01f, def.half_extents[1]);
        const float hz = std::max(0.01f, def.half_extents[2]);
        const JPH::Vec3 half_extent(hx, hy, hz);
        // The convex radius must not exceed the smallest half extent, or the
        // box's inner hull is degenerate and collision queries read garbage
        // (Jolt asserts are compiled out in this build, so it crashes instead
        // of tripping the assert). Bone boxes are routinely thinner than the
        // default 0.05 radius on at least one axis.
        const float convex_radius = std::min(0.05f, std::min(hx, std::min(hy, hz)) * 0.5f);
        JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(half_extent, convex_radius);

        const JPH::Quat rotation(
            def.transform.rotation[0], def.transform.rotation[1],
            def.transform.rotation[2], def.transform.rotation[3]);
        JPH::BodyCreationSettings settings(
            shape,
            JPH::RVec3(def.transform.position[0], def.transform.position[1], def.transform.position[2]),
            rotation.LengthSq() > 1e-6f ? rotation.Normalized() : JPH::Quat::sIdentity(),
            JPH::EMotionType::Kinematic,
            Layers::MOVING);
        settings.mIsSensor = def.is_trigger;
        // Let triggers detect static/kinematic bodies too (e.g. other
        // characters' bone colliders, static trigger volumes), not just
        // dynamic ones.
        if (def.is_trigger)
        {
            settings.mCollideKinematicVsNonDynamic = true;
        }

        const JPH::BodyID id = body_interface.CreateAndAddBody(settings, JPH::EActivation::Activate);
        if (id.IsInvalid())
        {
            continue;
        }

        BoneColliderRecord record;
        record.body_id_value = id.GetIndexAndSequenceNumber();
        record.owner_object = def.owner_object;
        record.bone_name = def.bone_name;
        record.is_trigger = def.is_trigger;

        bone_collider_records_[def.key] = record;
        bone_body_id_to_key_[record.body_id_value] = def.key;
        // Raycasts against a bone collider report the owning object.
        body_id_to_name_[record.body_id_value] = def.owner_object;
    }
}

void PhysicsWorld::UpdateBoneColliderTransforms(
    const std::unordered_map<std::string, PhysicsBodyTransform>& transforms,
    float delta_time)
{
    if (!initialized_ || bone_collider_records_.empty())
    {
        return;
    }

    JPH::BodyInterface& body_interface = physics_system_->GetBodyInterface();
    for (const auto& [key, record] : bone_collider_records_)
    {
        const auto it = transforms.find(key);
        if (it == transforms.end())
        {
            continue;
        }

        const JPH::BodyID id(record.body_id_value);
        if (id.IsInvalid())
        {
            continue;
        }

        const PhysicsBodyTransform& t = it->second;
        const JPH::RVec3 pos(t.position[0], t.position[1], t.position[2]);
        const JPH::Quat raw_rot(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]);
        const JPH::Quat rot = raw_rot.LengthSq() > 1e-6f ? raw_rot.Normalized() : JPH::Quat::sIdentity();

        if (delta_time > 0.0f)
        {
            // Velocity-based kinematic move so rigidbody colliders impart a
            // push on dynamic bodies they sweep through.
            body_interface.MoveKinematic(id, pos, rot, delta_time);
        }
        else
        {
            body_interface.SetPositionAndRotation(id, pos, rot, JPH::EActivation::Activate);
        }
    }
}

SceneVector3 PhysicsWorld::ComputeBoxDepenetration(
    const SceneVector3& center,
    const std::array<float, 4>& rotation,
    const SceneVector3& half_extents,
    const std::string& ignore_owner,
    bool* out_any_hit,
    float* out_nearest_depth) const
{
    SceneVector3 result{0.0f, 0.0f, 0.0f};
    if (out_any_hit != nullptr) { *out_any_hit = false; }
    if (out_nearest_depth != nullptr) { *out_nearest_depth = 0.0f; }
    if (!initialized_)
    {
        return result;
    }

    // Bodies to exclude: the owner's main body and all of its bone colliders.
    std::unordered_set<std::uint32_t> ignore_ids;
    if (!ignore_owner.empty())
    {
        const auto body_it = body_records_.find(ignore_owner);
        if (body_it != body_records_.end())
        {
            ignore_ids.insert(body_it->second.body_id_value);
        }
        for (const auto& [key, rec] : bone_collider_records_)
        {
            if (rec.owner_object == ignore_owner)
            {
                ignore_ids.insert(rec.body_id_value);
            }
        }
    }

    struct OwnerBodyFilter final : public JPH::BodyFilter
    {
        const std::unordered_set<std::uint32_t>* ignore = nullptr;
        bool ShouldCollide(const JPH::BodyID& id) const override
        {
            return ignore->find(id.GetIndexAndSequenceNumber()) == ignore->end();
        }
    };
    OwnerBodyFilter body_filter;
    body_filter.ignore = &ignore_ids;

    const float hx = std::max(0.01f, half_extents[0]);
    const float hy = std::max(0.01f, half_extents[1]);
    const float hz = std::max(0.01f, half_extents[2]);
    const float convex_radius = std::min(0.05f, std::min(hx, std::min(hy, hz)) * 0.5f);
    JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3(hx, hy, hz), convex_radius);

    JPH::Quat rot(rotation[0], rotation[1], rotation[2], rotation[3]);
    rot = rot.LengthSq() > 1e-6f ? rot.Normalized() : JPH::Quat::sIdentity();
    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(
        rot, JPH::RVec3(center[0], center[1], center[2]));

    JPH::CollideShapeSettings settings;
    // The box typically sits fully inside the wall mesh, so we must collide
    // with back faces to detect the overlap at all. Collide with all edges so
    // a box resting flat against a wall still reports its face contact.
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
    settings.mMaxSeparationDistance = 0.0f;

    JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const JPH::SpecifiedObjectLayerFilter object_filter(Layers::NON_MOVING);

    physics_system_->GetNarrowPhaseQuery().CollideShape(
        shape,
        JPH::Vec3::sReplicate(1.0f),
        com,
        settings,
        JPH::RVec3::sZero(),
        collector,
        {},
        object_filter,
        body_filter,
        {});

    if (collector.HadHit())
    {
        const JPH::CollideShapeResult& hit = collector.mHit;
        if (out_any_hit != nullptr) { *out_any_hit = true; }
        if (out_nearest_depth != nullptr) { *out_nearest_depth = hit.mPenetrationDepth; }
        const float axis_len = hit.mPenetrationAxis.Length();
        if (axis_len > 1e-6f && hit.mPenetrationDepth > 0.0f)
        {
            // mPenetrationAxis moves shape 2 (the wall) out; to push our box
            // (shape 1) out we go the opposite way.
            const JPH::Vec3 push = (hit.mPenetrationAxis / axis_len) * (-hit.mPenetrationDepth);
            result = {push.GetX(), push.GetY(), push.GetZ()};
        }
    }

    return result;
}

SceneVector3 PhysicsWorld::ResolveBoxSweep(
    const SceneVector3& from,
    const SceneVector3& to,
    const std::array<float, 4>& rotation,
    const SceneVector3& half_extents,
    const std::string& ignore_owner) const
{
    if (!initialized_)
    {
        return to;
    }

    // Bodies to exclude: the owner's main body and all of its bone colliders.
    std::unordered_set<std::uint32_t> ignore_ids;
    if (!ignore_owner.empty())
    {
        const auto body_it = body_records_.find(ignore_owner);
        if (body_it != body_records_.end())
        {
            ignore_ids.insert(body_it->second.body_id_value);
        }
        for (const auto& [key, rec] : bone_collider_records_)
        {
            if (rec.owner_object == ignore_owner)
            {
                ignore_ids.insert(rec.body_id_value);
            }
        }
    }
    struct OwnerBodyFilter final : public JPH::BodyFilter
    {
        const std::unordered_set<std::uint32_t>* ignore = nullptr;
        bool ShouldCollide(const JPH::BodyID& id) const override
        {
            return ignore->find(id.GetIndexAndSequenceNumber()) == ignore->end();
        }
    };
    OwnerBodyFilter body_filter;
    body_filter.ignore = &ignore_ids;

    const float hx = std::max(0.01f, half_extents[0]);
    const float hy = std::max(0.01f, half_extents[1]);
    const float hz = std::max(0.01f, half_extents[2]);
    const float convex_radius = std::min(0.05f, std::min(hx, std::min(hy, hz)) * 0.5f);
    JPH::RefConst<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3(hx, hy, hz), convex_radius);

    JPH::Quat rot(rotation[0], rotation[1], rotation[2], rotation[3]);
    rot = rot.LengthSq() > 1e-6f ? rot.Normalized() : JPH::Quat::sIdentity();

    const JPH::SpecifiedObjectLayerFilter object_filter(Layers::NON_MOVING);

    JPH::ShapeCastSettings cast_settings;
    cast_settings.mReturnDeepestPoint = false;

    JPH::RVec3 pos(from[0], from[1], from[2]);
    JPH::Vec3 delta(to[0] - from[0], to[1] - from[1], to[2] - from[2]);

    // Up to two cast iterations: clamp at the first surface hit, then slide
    // the remaining motion along that surface and clamp once more. The 1 mm
    // skin keeps the next frame's cast starting just clear of the surface.
    constexpr float kSkin = 0.001f;
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        const float len = delta.Length();
        if (len < 1e-6f)
        {
            break;
        }

        const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(rot, pos);
        const JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), com, delta);
        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        physics_system_->GetNarrowPhaseQuery().CastShape(
            cast, cast_settings, JPH::RVec3::sZero(), collector, {}, object_filter, body_filter, {});

        if (!collector.HadHit())
        {
            pos += delta;
            break;
        }

        const JPH::ShapeCastResult& hit = collector.mHit;
        const float stop_fraction = std::max(0.0f, hit.mFraction - kSkin / len);
        pos += delta * stop_fraction;

        const float axis_len = hit.mPenetrationAxis.Length();
        if (axis_len < 1e-6f)
        {
            break;
        }
        // Contact normal facing the moving box; slide the blocked remainder
        // of the motion along the surface plane.
        const JPH::Vec3 normal = -hit.mPenetrationAxis / axis_len;
        const JPH::Vec3 remaining = delta * (1.0f - hit.mFraction);
        delta = remaining - normal * remaining.Dot(normal);
    }

    SceneVector3 resolved{
        static_cast<float>(pos.GetX()),
        static_cast<float>(pos.GetY()),
        static_cast<float>(pos.GetZ())};

    // Clean up residual penetration (start-inside cases, moving into corners).
    const SceneVector3 push = ComputeBoxDepenetration(resolved, rotation, half_extents, ignore_owner);
    resolved[0] += push[0];
    resolved[1] += push[1];
    resolved[2] += push[2];
    return resolved;
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
    // Resolve each body to an object name + (optional) bone name. Bone
    // colliders resolve to their owning object so existing routing works,
    // and additionally carry the bone name.
    auto resolve = [this](std::uint32_t body_id, std::string& out_name, std::string& out_bone) -> bool
    {
        const auto bone_it = bone_body_id_to_key_.find(body_id);
        if (bone_it != bone_body_id_to_key_.end())
        {
            const auto rec_it = bone_collider_records_.find(bone_it->second);
            if (rec_it != bone_collider_records_.end())
            {
                out_name = rec_it->second.owner_object;
                // Only trigger colliders surface a bone name (and thus script
                // callbacks). Rigidbody colliders are purely physical.
                out_bone = rec_it->second.is_trigger ? rec_it->second.bone_name : std::string();
                return true;
            }
        }
        out_bone.clear();
        return TryGetObjectNameByBodyId(body_id, out_name);
    };

    std::string name_a;
    std::string name_b;
    std::string bone_a;
    std::string bone_b;
    if (!resolve(body_a, name_a, bone_a) || !resolve(body_b, name_b, bone_b))
    {
        return;
    }

    PhysicsCollisionEvent event;
    event.object_a = name_a;
    event.object_b = name_b;
    event.phase = phase != nullptr ? phase : "stay";
    event.a_bone = bone_a;
    event.b_bone = bone_b;

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
