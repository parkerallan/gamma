# Developer API

This document will list every Lua function exposed by the runtime, this will be used for scripting a game rather than using node graph system. **All of these functions are subject to change**

## Packages Used

- `Lua` (`v5.4.7`): Scripting runtime used by all `Engine`, `Input`, `World`, `Time`, and `Physics` Lua calls.
- `SDL3` (`release-3.4.4`): Provides key/mouse input polling, frame timing (`Time.DeltaTime` / `Time.TotalTime`), and runtime log output.
- `Jolt Physics` (`v5.3.0`): Powers rigidbody simulation, raycasts, velocity/force APIs, and collision events exposed through `World` and `Physics`.
- `miniaudio` (`v0.11.21`): Backs the `Audio` runtime API and the `Audio` SceneObject attribute (3D spatialization, distance attenuation, doppler).

### Notes

- The developer API is a Lua layer over engine-side C++ systems; package upgrades can affect behavior and signatures.
- Current implementation focuses on runtime gameplay scripting. Editor-specific UI packages (ImGui/ImNodeFlow/ImGuizmo) are not directly part of Lua API calls.

## Globals

### `Engine`

#### `Engine.Log(message)`
Quick summary: Writes a message to the engine log.

```lua
Engine.Log("Hello from Lua")
```

#### `Engine.SetObjectPosition(name, x, y, z)`
Quick summary: Sets an object's position.

```lua
Engine.SetObjectPosition("Player", 0.0, 1.0, 0.0)
```

#### `Engine.GetObjectPosition(name)`
Quick summary: Gets an object's position. Returns `x, y, z` or `nil` if not found.

```lua
local x, y, z = Engine.GetObjectPosition("Player")
```

#### `Engine.SetObjectRotation(name, x, y, z)`
Quick summary: Sets an object's Euler rotation.

```lua
Engine.SetObjectRotation("Player", 0.0, 90.0, 0.0)
```

#### `Engine.GetObjectRotation(name)`
Quick summary: Gets an object's Euler rotation. Returns `x, y, z` or `nil` if not found.

```lua
local rx, ry, rz = Engine.GetObjectRotation("Player")
```

#### `Engine.SetObjectScale(name, x, y, z)`
Quick summary: Sets an object's scale.

```lua
Engine.SetObjectScale("Crate", 1.2, 1.2, 1.2)
```

#### `Engine.GetObjectScale(name)`
Quick summary: Gets an object's scale. Returns `x, y, z` or `nil` if not found.

```lua
local sx, sy, sz = Engine.GetObjectScale("Crate")
```

### `Engine` attribute tables

Quick summary: Attribute tables expose the first matching attribute on an object. Calling a method with only `name` reads the value. Passing additional values writes the value for the active Play session without rewriting the scene file.

#### `Engine.EnvironmentLightAttr`
Methods: `Color(name[, r, g, b])`, `Intensity(name[, value])`

#### `Engine.DirectionalLightAttr`
Methods: `Color(name[, r, g, b])`, `Intensity(name[, value])`

#### `Engine.PointLightAttr`
Methods: `Color(name[, r, g, b])`, `Intensity(name[, value])`, `Range(name[, value])`, `Radius(name[, value])`, `HaloIntensity(name[, value])`, `HaloRadius(name[, value])`

#### `Engine.SpotLightAttr`
Methods: `Color(name[, r, g, b])`, `Intensity(name[, value])`, `Range(name[, value])`, `InnerCone(name[, value])`, `OuterCone(name[, value])`

#### `Engine.CameraAttr`
Methods: `FieldOfView(name[, value])`, `NearClip(name[, value])`, `FarClip(name[, value])`, `Active(name[, enabled])`

#### `Engine.RigidbodyAttr`
Methods: `Shape(name[, value])`, `Dynamic(name[, enabled])`, `LockRotationX(name[, enabled])`, `LockRotationY(name[, enabled])`, `LockRotationZ(name[, enabled])`, `Mass(name[, value])`, `Friction(name[, value])`, `Radius(name[, value])`, `CapsuleHalfHeight(name[, value])`, `HalfExtent(name[, x, y, z])`, `LinearDamping(name[, value])`, `AngularDamping(name[, value])`

`Shape` uses the strings `"None"`, `"Box"`, `"Sphere"`, `"Capsule"`, and `"Mesh"`.

#### `Engine.TriggerVolumeAttr`
Methods: `HalfExtent(name[, x, y, z])`

#### `Engine.Text2DAttr`
Methods: `FontPath(name[, path])`, `Text(name[, text])`, `Position(name[, x, y])`, `Size(name[, width, height])`, `LockAspectRatio(name[, enabled])`, `FontSize(name[, value])`, `Color(name[, r, g, b])`, `Alpha(name[, value])`, `Priority(name[, value])`

#### `Engine.Image2DAttr`
Methods: `ImagePath(name[, path])`, `Position(name[, x, y])`, `Size(name[, width, height])`, `LockAspectRatio(name[, enabled])`, `Tint(name[, r, g, b])`, `Alpha(name[, value])`, `Priority(name[, value])`

#### `Engine.SkyboxAttr`
Methods: `ImagePath(name[, path])`, `Rotation(name[, value])`

#### `Engine.AudioAttr`
Methods: `ClipPath(name[, path])`, `PlayMode(name[, value])`, `Volume(name[, value])`, `Loop(name[, enabled])`, `Spatialize3D(name[, enabled])`, `Pitch(name[, value])`, `MinDistance(name[, value])`, `MaxDistance(name[, value])`, `DopplerFactor(name[, value])`

`PlayMode` uses the strings `"On"` and `"Off"`. Setting it to `"On"` starts playback (looping clips loop, one-shots play once); `"Off"` stops playback.

`Volume` is `0.0` (silent) to `1.0` (full) and is mapped through a perceptual cube-law curve so the slider/value feels uniform.

#### `Engine.Animator`
Methods (attribute accessors): `ControllerPath(name[, path])`, `InitialState(name[, stateName])`, `PlaybackSpeed(name[, value])`, `AutoPlay(name[, enabled])`, `SetDefaultState(name, stateName)`, `GetDefaultState(name)`

Example:

```lua
local r, g, b = Engine.PointLightAttr.Color("Lamp")
Engine.PointLightAttr.Color("Lamp", 1.0, 0.8, 0.6)
Engine.RigidbodyAttr.Shape("Crate", "Box")
Engine.CameraAttr.Active("GameplayCamera", true)
Engine.Text2DAttr.Text("DialogueBox", "Hello there")
local message = Engine.Text2DAttr.Text("DialogueBox")
Engine.Text2DAttr.Priority("DialogueBox", 10)  -- Higher priority = rendered behind
Engine.Image2DAttr.Priority("UIBackground", 1)  -- Lower priority = rendered on top
```

**Priority Notes:**
- `Priority` is an integer that controls the stacking order of Text2D and Image2D overlays.
- Lower priority values render on top (drawn last); higher values render behind (drawn first).
- Default priority is `1` for all overlays.
- Example: Priority 1 will render on top of Priority 10. If priorities are equal, overlays are rendered in creation order.

### Animation Runtime Functions

Quick summary: The functions below control runtime animation state and parameters. They are not attribute accessors.

#### `Engine.Animator.GetState(name)`
Quick summary: Returns current runtime animator state name, or `nil` if unavailable.

```lua
local state = Engine.Animator.GetState("Fox")
```

#### `Engine.Animator.StateTime(name)`
Quick summary: Returns current runtime state local time in seconds, or `nil` if unavailable.

```lua
local t = Engine.Animator.StateTime("Fox")
```

#### `Engine.Animator.SetBool(name, parameterName, value)`
Quick summary: Sets a runtime bool parameter. Returns `true` on success.

```lua
local ok = Engine.Animator.SetBool("Fox", "isMoving", true)
```

#### `Engine.Animator.GetBool(name, parameterName)`
Quick summary: Gets a runtime bool parameter. Returns `false` if unavailable.

```lua
local isMoving = Engine.Animator.GetBool("Fox", "isMoving")
```

#### `Engine.Animator.SetTrigger(name, triggerName)`
Quick summary: Sets a runtime trigger parameter for transition evaluation. Returns `true` on success.

```lua
local ok = Engine.Animator.SetTrigger("Fox", "attack")
```

#### `Engine.Animator.SetState(name, stateName)`
Quick summary: Immediately requests a runtime state change. Returns `true` on success.

```lua
local ok = Engine.Animator.SetState("Fox", "Run")
```

Notes:
- Runtime controls: `SetBool`, `GetBool`, `SetTrigger`, `SetState`, `GetState`, `StateTime`.
- If an object has no Animator attribute, animator calls return `nil`, `false`, or no-op depending on function.

### `Time`

`Time` exposes fields, not functions.

#### `Time.DeltaTime`
Quick summary: Seconds elapsed since the previous frame.

```lua
local dt = Time.DeltaTime
```

#### `Time.TotalTime`
Quick summary: Seconds elapsed since Play started (or since a scene load).

```lua
local t = Time.TotalTime
```

### `Input`

#### `Input.IsKeyDown(keyName)`
Quick summary: Returns `true` while a key is held.

```lua
if Input.IsKeyDown("W") then
    Engine.Log("Moving forward")
end
```

#### `Input.WasKeyPressed(keyName)`
Quick summary: Returns `true` on the transition frame when a key is pressed.

```lua
if Input.WasKeyPressed("Space") then
    Engine.Log("Jump")
end
```

#### `Input.MousePosition()`
Quick summary: Returns current mouse position as `x, y`.

```lua
local mx, my = Input.MousePosition()
```

#### `Input.MouseDelta()`
Quick summary: Returns relative mouse movement this frame as `dx, dy`.

```lua
local dx, dy = Input.MouseDelta()
```

### `World`

#### `World.Subscribe(eventName, handler)`
Quick summary: Subscribes the current script instance to a named event.

```lua
World.Subscribe("Damage", function(self, sender, payload)
    Engine.Log("Damage from " .. tostring(sender))
end)
```

#### `World.Emit(eventName, payload)`
Quick summary: Emits an event to all subscribers.

```lua
World.Emit("Damage", "10")
```

#### `World.Spawn(name, modelPath, x, y, z, scriptPath)`
Quick summary: Spawns a runtime object and optionally attaches a script.

```lua
World.Spawn("Enemy_01", "Models/Enemy.glb", 4.0, 0.0, -2.0, "Scripts/Enemy.lua")
```

#### `World.SpawnFromObject(sourceName, newName, x, y, z, scriptOverride)`
Quick summary: Spawns from an existing object template.

```lua
World.SpawnFromObject("EnemyTemplate", "Enemy_02", 6.0, 0.0, -2.0)
```

#### `World.Destroy(name)`
Quick summary: Marks an object for runtime destruction.

```lua
World.Destroy("Enemy_01")
```

#### `World.DestroyByPrefix(prefix)`
Quick summary: Destroys all objects whose names start with the prefix. Returns count.

```lua
local removed = World.DestroyByPrefix("Bullet_")
```

#### `World.Exists(name)`
Quick summary: Returns `true` if an object currently exists.

```lua
if World.Exists("Boss") then
    Engine.Log("Boss is alive")
end
```

#### `World.GetAll()`
Quick summary: Returns an array of all current object names.

```lua
for i, name in ipairs(World.GetAll()) do
    Engine.Log(name)
end
```

#### `World.FindByPrefix(prefix)`
Quick summary: Returns object names that start with the prefix.

```lua
local enemies = World.FindByPrefix("Enemy_")
```

#### `World.GetCollisions([objectName], [phase])`
Quick summary: Returns collision events for the current frame, optionally filtered.

```lua
local hits = World.GetCollisions("Player", "Enter")
for i, hit in ipairs(hits) do
    Engine.Log("Hit " .. tostring(hit.other))
end
```

#### `World.GetCollisionsFor(objectName, [phase])`
Quick summary: Returns collision events for one object.

```lua
local hits = World.GetCollisionsFor("Player")
```

#### `World.GetCollisionsByPhase(phase)`
Quick summary: Returns collision events filtered by phase only.

```lua
local exits = World.GetCollisionsByPhase("Exit")
```

#### `World.SetTimeout(seconds, callback)`
Quick summary: Runs callback once after delay. Returns timer id.

```lua
local timerId = World.SetTimeout(1.5, function(self)
    Engine.Log("Timer fired")
end)
```

#### `World.SetInterval(seconds, callback)`
Quick summary: Runs callback repeatedly. Returns timer id.

```lua
local timerId = World.SetInterval(0.25, function(self)
    Engine.Log("Tick")
end)
```

#### `World.ClearTimer(timerId)`
Quick summary: Stops a timeout/interval by id. Returns `true` if found.

```lua
local ok = World.ClearTimer(timerId)
```

#### `World.LoadScene(sceneNameOrPath)`
Quick summary: Queues loading another scene on the next frame.

```lua
World.LoadScene("Level2.scene")
```

Notes:
- If no extension is provided, `.scene` is appended automatically.
- Relative paths are resolved from the project's `Scenes` folder.
- Scene load resets runtime script/physics state and uses the new scene's first active camera.

### `Physics`

#### `Physics.Raycast(ox, oy, oz, dx, dy, dz[, maxDistance])`
Quick summary: Casts a ray. Returns hit table or `nil`.

```lua
local hit = Physics.Raycast(0, 2, 0, 0, -1, 0, 100)
if hit then
    Engine.Log("Hit " .. hit.object)
end
```

#### `Physics.SetVelocity(name, x, y, z)`
Quick summary: Sets linear velocity for an object with a dynamic rigidbody.

```lua
Physics.SetVelocity("Player", 0.0, 5.0, 0.0)
```

#### `Physics.GetVelocity(name)`
Quick summary: Gets linear velocity. Returns `x, y, z` or `nil`.

```lua
local vx, vy, vz = Physics.GetVelocity("Player")
```

#### `Physics.AddImpulse(name, x, y, z)`
Quick summary: Applies an impulse to an object.

```lua
Physics.AddImpulse("Player", 0.0, 3.0, 0.0)
```

#### `Physics.AddForce(name, x, y, z)`
Quick summary: Applies force to an object for this step.

```lua
Physics.AddForce("Player", 10.0, 0.0, 0.0)
```

### `Audio`

#### `Audio.Play(name)`
Quick summary: Switches an object's `Audio` attribute to `On`, starting playback. Returns `true` on success.

```lua
Audio.Play("Speaker")
```

#### `Audio.Stop(name)`
Quick summary: Switches an object's `Audio` attribute to `Off`, stopping playback. Returns `true` on success.

```lua
Audio.Stop("Speaker")
```

#### `Audio.IsPlaying(name)`
Quick summary: Returns `true` while the object's `Audio` attribute is currently producing sound.

```lua
if Audio.IsPlaying("Speaker") then
    Engine.Log("Still playing")
end
```

#### `Audio.SetVolume(name, value)`
Quick summary: Sets runtime volume in the `0.0`–`1.0` range. Returns `true` on success.

```lua
Audio.SetVolume("Speaker", 0.5)
```

#### `Audio.SetPitch(name, value)`
Quick summary: Sets runtime pitch multiplier. Returns `true` on success.

```lua
Audio.SetPitch("Speaker", 1.25)
```

#### `Audio.SetLoop(name, enabled)`
Quick summary: Toggles looping at runtime. Returns `true` on success.

```lua
Audio.SetLoop("Speaker", true)
```

Notes:
- All `Audio.*` calls target the first `Audio` attribute on the named object and return `false` if the object has no `Audio` attribute.
- Audio is only active while a scene is playing.
- Clip format support: `.wav`, `.ogg`, `.mp3`.
- 3D spatialization (when enabled on the attribute) uses the active camera as the listener with inverse distance attenuation between `MinDistance` and `MaxDistance` plus doppler shift scaled by `DopplerFactor`.

## Callback Notes

Typical script callbacks are `OnStart(self)`, `OnUpdate(self, dt)`, and `OnDestroy(self)`.

### Trigger Callbacks

Objects with a `Trigger Volume` attribute can implement the following callbacks:

#### `OnTriggerEnter(self, objectName, otherName, phase)`
Quick summary: Called when another object first overlaps this trigger volume.

```lua
function Script:OnTriggerEnter(objectName, otherName, phase)
    if otherName == "Player" then
        Engine.Log(objectName .. " entered by " .. otherName .. " (" .. phase .. ")")
    end
end
```

#### `OnTriggerStay(self, objectName, otherName, phase)`
Quick summary: Called each frame while another object remains inside this trigger volume.

```lua
function Script:OnTriggerStay(objectName, otherName, phase)
    if otherName == "Player" then
        -- Keep this lightweight; it runs every frame during overlap.
    end
end
```

#### `OnTriggerExit(self, objectName, otherName, phase)`
Quick summary: Called when another object stops overlapping this trigger volume.

```lua
function Script:OnTriggerExit(objectName, otherName, phase)
    if otherName == "Player" then
        Engine.Log(objectName .. " exited by " .. otherName .. " (" .. phase .. ")")
    end
end
```

`otherName` is the other object in the overlap pair, and `phase` is one of `enter`, `stay`, `exit`.

`World.Subscribe`, `World.SetTimeout`, and `World.SetInterval` must be called from an active script callback.
