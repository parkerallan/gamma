# Developer API

This document will list every Lua function exposed by the runtime, this will be used for scripting a game rather than using node graph system. **All of these functions are subject to change**

## Packages Used

- `Lua` (`v5.4.7`): Scripting runtime used by all `Engine`, `Input`, `World`, `Time`, and `Physics` Lua calls.
- `SDL3` (`release-3.4.4`): Provides key/mouse input polling, frame timing (`Time.DeltaTime` / `Time.TotalTime`), and runtime log output.
- `Jolt Physics` (`v5.3.0`): Powers rigidbody simulation, raycasts, velocity/force APIs, and collision events exposed through `World` and `Physics`.
- `miniaudio` (`v0.11.21`): Backs the `Audio` runtime API and the `Audio` SceneObject attribute (3D spatialization, distance attenuation, doppler).
- `ffmpeg` (`latest`): Used in video system, audio used in videos still goes through audio engine using miniaudio.

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

#### `Engine.SetObjectEnabled(name, enabled)`
Quick summary: Enables or disables a scene object for the current Play session. Returns `true` on success.

```lua
Engine.SetObjectEnabled("EnemySpawner", false)
```

#### `Engine.GetObjectEnabled(name)`
Quick summary: Gets an object's direct enabled state. Returns `true`, `false`, or `nil` if not found.

```lua
local enabled = Engine.GetObjectEnabled("EnemySpawner")
```

#### `Engine.SetCameraActive(name, enabled)`
Quick summary: Sets the first Camera attribute on an object active or inactive for the current Play session. Returns `true` on success. Setting a camera active clears other active cameras.

```lua
Engine.SetCameraActive("GameplayCamera", true)
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
Methods: `ImagePath(name[, path])`, `Position(name[, x, y])`, `Size(name[, width, height])`, `LockAspectRatio(name[, enabled])`, `StretchToScreen(name[, enabled])`, `PlayMode(name[, value])`, `Tint(name[, r, g, b])`, `Alpha(name[, value])`, `Priority(name[, value])`

For `.gif` image paths, `PlayMode` uses the strings `"Loop"`, `"PlayOnce"`, and `"Off"`. GIFs use their embedded frame delays; `"Off"` shows the first frame without advancing.

#### `Engine.Color2DAttr`
Methods: `Position(name[, x, y])`, `Size(name[, width, height])`, `LockAspectRatio(name[, enabled])`, `StretchToScreen(name[, enabled])`, `Color(name[, r, g, b])`, `Alpha(name[, value])`, `Priority(name[, value])`

#### `Engine.Video2DAttr`
Methods: `VideoPath(name[, path])`, `Position(name[, x, y])`, `Size(name[, width, height])`, `LockAspectRatio(name[, enabled])`, `StretchToScreen(name[, enabled])`, `Tint(name[, r, g, b])`, `Alpha(name[, value])`, `Priority(name[, value])`, `PlayMode(name[, value])`, `Volume(name[, value])`, `Muted(name[, enabled])`

`PlayMode` uses the strings `"Loop"`, `"PlayOnce"`, and `"Off"`. Setting it to `"Off"` stops playback and releases the decoder; switching to `"Loop"` or `"PlayOnce"` (re)starts the stream. `StretchToScreen` overrides `Position`/`Size` and renders the 2D image, color, or video full-viewport. Audio plays only when `PlayMode` is `"Loop"` or `"PlayOnce"`.

#### `Engine.SkyboxAttr`
Methods: `ImagePath(name[, path])`, `Rotation(name[, value])`

#### `Engine.AudioAttr`
Methods: `ClipPath(name[, path])`, `PlayMode(name[, value])`, `Volume(name[, value])`, `Loop(name[, enabled])`, `Spatialize3D(name[, enabled])`, `Pitch(name[, value])`, `MinDistance(name[, value])`, `MaxDistance(name[, value])`, `DopplerFactor(name[, value])`

`PlayMode` uses the strings `"On"` and `"Off"`. Setting it to `"On"` starts playback (looping clips loop, one-shots play once); `"Off"` stops playback.

`Volume` is `0.0` (silent) to `1.0` (full) and is mapped through a perceptual cube-law curve so the slider/value feels uniform.

#### `Engine.EffectsAttr`
Methods: `EffectPath(name[, path])`, `PlayMode(name[, value])`

`PlayMode` uses the strings `"Loop"` and `"PlayOnce"` for trigger behavior. Runtime play state is controlled through `Effect.Play(name)` and `Effect.Stop(name)`.

#### `Engine.Animator`
Methods (attribute accessors): `ControllerPath(name[, path])`, `InitialState(name[, stateName])`, `PlaybackSpeed(name[, value])`, `AutoPlay(name[, enabled])`

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
Engine.Color2DAttr.Color("DamageFlash", 1.0, 0.0, 0.0)
```

**Priority Notes:**
- `Priority` is an integer that controls the stacking order of Text2D, Image2D, Color2D, and Video2D overlays.
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

#### `Engine.Animator.SetFacePose(name, poseName [, weight] [, speed])`
Quick summary: Blends the face toward an expression pose authored on the controller's Face tab. `weight` (0–1, default `1`) is the intensity, so you can hold a pose partially open. `speed` (default `0` = snap) eases the transition — higher is faster — so poses cross-fade instead of popping. Pass an empty `poseName` to blend back to the controller's default pose. Returns `true` on success.

```lua
Engine.Animator.SetFacePose("Npc", "Angry")          -- full strength, instant
Engine.Animator.SetFacePose("Npc", "Angry", 0.5, 6)  -- half strength, eased in
Engine.Animator.SetFacePose("Npc", "", 1.0, 6)       -- ease back to default
```

#### `Engine.Animator.PlayLipSync(name, clipName [, loop])`
Quick summary: Plays a baked lip-sync clip on the object. This **also plays the clip's source audio** (3D-positioned at the object) and drives the mouth from the audio's playback position, so speech and lips stay in sync. `clipName` matches a clip baked on the controller's Face tab, by file stem (e.g. `"hello"`) or its project-relative path. `loop` is optional (default `false`). Returns `true` if the clip was found and started.

```lua
Engine.Animator.PlayLipSync("Npc", "greeting")
```

#### `Engine.Animator.StopLipSync(name)`
Quick summary: Stops lip-sync playback and its audio on the object. Returns `true` on success.

```lua
Engine.Animator.StopLipSync("Npc")
```

#### `Engine.Animator.IsLipSyncPlaying(name)`
Quick summary: Returns `true` while a lip-sync clip is playing on the object.

```lua
if not Engine.Animator.IsLipSyncPlaying("Npc") then
    Engine.Animator.PlayLipSync("Npc", "next_line")
end
```

#### `Engine.Animator.SetEyeTarget(name, x, y, z)`
Quick summary: Makes the object's eyes track a world-space point (drives the ARKit `eyeLook*` shapes). The gaze is smoothed; it holds until changed or cleared. Returns `true` on success.

```lua
Engine.Animator.SetEyeTarget("Npc", 0.0, 1.6, 5.0)
```

#### `Engine.Animator.LookAt(name, targetName)`
Quick summary: Makes the object's eyes track another object's position, re-evaluated each frame as the target moves. Returns `true` on success.

```lua
Engine.Animator.LookAt("Npc", "Player")
```

#### `Engine.Animator.ClearEyeTarget(name)`
Quick summary: Stops gaze tracking; the eyes ease back to center. Returns `true` on success.

```lua
Engine.Animator.ClearEyeTarget("Npc")
```

Notes:
- Runtime controls: `SetBool`, `GetBool`, `SetTrigger`, `SetState`, `GetState`, `StateTime`.
- Facial controls: `SetFacePose`, `PlayLipSync`, `StopLipSync`, `IsLipSyncPlaying`, `SetEyeTarget`, `LookAt`, `ClearEyeTarget`. The expression pose, a playing lip-sync clip, and gaze layer together (lip-sync overrides the mouth/jaw shapes it animates; gaze overrides the `eyeLook*` shapes).
- Lip-sync clips are baked on the Animator panel's Face tab (drag a `.wav` onto the bake target); `PlayLipSync` owns audio playback, so do not start the same audio separately.
- If an object has no Animator attribute, animator calls return `nil`, `false`, or no-op depending on function.

### `Time`

`Time` exposes frame-timing fields and timer functions.

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

#### `Time.Delay(seconds, callback)`
Quick summary: Runs callback once after delay. Returns timer id. Alias of `World.SetTimeout`.

```lua
local timerId = Time.Delay(1.5, function(self)
    Engine.Log("Delay fired")
end)
```

#### `Time.Timer(seconds, callback)`
Quick summary: Runs callback repeatedly every `seconds`. Returns timer id. Alias of `World.SetInterval`.

```lua
local timerId = Time.Timer(0.25, function(self)
    Engine.Log("Tick")
end)
```

#### `Time.ClearTimer(timerId)`
Quick summary: Stops a `Time.Delay`/`Time.Timer` (or `World.SetTimeout`/`World.SetInterval`) by id. Returns `true` if found.

```lua
local ok = Time.ClearTimer(timerId)
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

#### `World.LoadScene(sceneName[, options])`
Quick summary: Switches the running game to another scene. The target scene's assets (models, audio, video) are parsed/read on a **background thread** so the main loop stays responsive — no "not responding" freeze, and the standalone game no longer exits while a large scene loads. `sceneName` may omit the `.scene` extension and is resolved relative to the project's `Scenes/` directory (absolute paths are also accepted).

`options` is an optional table:
- `loadingScene` — name of a lightweight scene to display **immediately** while the target streams in the background. Author it like any other scene (a `Color2D`/`Image2D` background, a `Text2D`, and a script that animates). It is swapped in instantly (its own assets should be small) and replaced by the target once streaming completes.

```lua
-- Stream a large scene while showing a loading screen.
World.LoadScene("Level2", { loadingScene = "Loading" })

-- Without a loading scene: the current scene keeps rendering until the
-- target is ready, then swaps in.
World.LoadScene("Level2")
```

Notes:
- If the target scene fails to parse or has no active camera, the call is logged and the **current scene keeps running** (it is not a fatal error).
- The request is honored on the next frame; calling it again while a load is in progress queues the latest request.

#### `World.IsSceneLoading()`
Quick summary: Returns `true` while a `World.LoadScene` target is still streaming in the background.

```lua
if World.IsSceneLoading() then ... end
```

#### `World.GetSceneLoadProgress()`
Quick summary: Returns the current load progress as a number in `0..1` (fraction of the target scene's models parsed so far), or `1` when nothing is loading. Use it from a loading scene's `OnUpdate` to drive a progress bar.

```lua
function OnUpdate(self, dt)
    local p = World.GetSceneLoadProgress()   -- 0..1
    Engine.SetObjectScale("ProgressBar", p, 1.0, 1.0)
end
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

#### `World.SpawnPrefab(prefabName[, x, y, z])`
Quick summary: Instantiates the root object of the named prefab (from `Assets/Prefabs/Prefabs.metadata`) into the current scene at runtime. Returns the actual spawned object name (auto-uniquified if a name collision occurs), or raises a Lua error on failure.

```lua
local name = World.SpawnPrefab("Cloud", 0.0, 5.0, 0.0)
Engine.Log("Spawned " .. name)
```

Notes:
- Position arguments are optional. When omitted, the prefab's stored root position is used.
- Spawns the prefab's **root object only** — child objects in the prefab's subtree are ignored at runtime. (Runtime spawned objects are flat; they don't carry a parent chain.)
- The root object's `Model`, `ModelVisualOffset`, `Rotation`, `Scale`, first `Script`, and `Tag` lines are applied. Attribute blocks (`Attributes:`) are not currently transferred to runtime spawned objects.
- Returned name is suitable to feed into `Engine.SetObjectPosition`, `Physics.SetVelocity`, `World.Destroy`, etc.

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

#### `World.FindByTag(tag)`
Quick summary: Returns the names of all enabled, non-destroyed objects that carry the given tag.

```lua
local enemies = World.FindByTag("Enemy")
for i, name in ipairs(enemies) do
    Engine.Log(name)
end
```

#### `World.GetObjectTags(objectName)`
Quick summary: Returns the array of tags currently assigned to the object (empty table if none / not found).

```lua
local tags = World.GetObjectTags("Player")
```

#### `World.ObjectHasTag(objectName, tag)`
Quick summary: Returns true if the object carries the given tag.

```lua
if World.ObjectHasTag("Player", "Invincible") then ... end
```

#### `World.AddObjectTag(objectName, tag)`
Quick summary: Adds a tag to the runtime view of the object. Returns true if the tag was added.

```lua
World.AddObjectTag("Enemy_01", "Stunned")
```

#### `World.RemoveObjectTag(objectName, tag)`
Quick summary: Removes a tag from the runtime view of the object. Returns true if a tag was removed.

```lua
World.RemoveObjectTag("Enemy_01", "Stunned")
```

##### Tag notes
- Tags are defined per-object in the scene file and shared in a project-wide registry stored in the `.engineproj` manifest (`projectTags`).
- Runtime tag changes via `AddObjectTag` / `RemoveObjectTag` are not persisted; they reset when Play stops or the scene reloads.
- Tag matching is case-sensitive and exact.

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

### `Video`

#### `Video.Play(name)`
Quick summary: Starts playback on an object's `Video2D` attribute. If the attribute is `Off`, it is switched to `Loop`. Returns `true` on success.

```lua
Video.Play("Cinematic")
```

#### `Video.Stop(name)`
Quick summary: Switches an object's `Video2D` attribute to `Off`, stopping playback and releasing the decoder. Returns `true` on success.

```lua
Video.Stop("Cinematic")
```

#### `Video.IsPlaying(name)`
Quick summary: Returns `true` while the object's `Video2D` attribute `PlayMode` is `"Loop"` or `"PlayOnce"`.

```lua
if Video.IsPlaying("Cinematic") then
    Engine.Log("Video active")
end
```

#### `Video.SetVolume(name, value)`
Quick summary: Sets runtime audio-track volume (`0.0`–`1.0`). Returns `true` on success.

```lua
Video.SetVolume("Cinematic", 0.5)
```

#### `Video.SetMuted(name, enabled)`
Quick summary: Mutes or unmutes the video's audio track at runtime. Returns `true` on success.

```lua
Video.SetMuted("Cinematic", true)
```

Notes:
- All `Video.*` calls target the first `Video2D` attribute on the named object and return `false` if the object has no `Video2D` attribute.
- Container/codec support: `.mp4`, `.mov`, `.mkv`, `.webm`, `.avi`, `.mpg`, `.mpeg`, `.m4v` via FFmpeg (with optional D3D11VA/DXVA2 hardware decode).
- Audio plays only while `PlayMode` is `"Loop"` or `"PlayOnce"`. `Muted` and `Volume` apply on top of `PlayMode`.

---

### `Effect`

Runtime control over an object's **Effects** attribute (Effekseer particle effects).

#### `Effect.Play(name)`

Starts playback on the named object's `Effects` attribute. If `PlayMode` is currently `"Stop"`, it is switched to `"Loop"`. Has no effect if the effect is already playing. Returns `true` on success, `false` if the object has no `Effects` attribute.

```lua
Effect.Play("Explosion")
```

#### `Effect.Stop(name)`

Stops playback on the named object's `Effects` attribute by setting `PlayMode` to `"Stop"`. Returns `true` on success.

```lua
Effect.Stop("Explosion")
```

#### `Effect.IsPlaying(name)`

Returns `true` while the object's `Effects` attribute `PlayMode` is `"Loop"` or `"PlayOnce"`.

```lua
if Effect.IsPlaying("Explosion") then
    -- still playing
end
```

Notes:
- All `Effect.*` calls target the first `Effects` attribute on the named object.
- `Effect.Play` switches from `"Stop"` → `"Loop"`. To start a one-shot play use `Engine.EffectsAttr.PlayMode(name, "PlayOnce")` instead.
- Use the `EffectsAttr` attribute accessor (`Engine.EffectsAttr.EffectPath`, `Engine.EffectsAttr.PlayMode`) for full read/write access to both fields.

## Callback Notes

A `Script:<Name>(self, ...)` method is invoked automatically when the runtime fires the matching event. All callbacks receive `self` as the script instance table; additional parameters vary per callback. Implementing any callback is optional — missing ones are silently skipped.

### Lifecycle Callbacks

#### `OnCreate(self)`
Quick summary: Called once when the script instance is first loaded, before `OnStart`. Use it to initialize fields, register event subscriptions, or create timers tied to the instance's lifetime.

```lua
function Script:OnCreate()
    self.hits = 0
    World.Subscribe("Damage", function(s, sender, payload)
        s.hits = s.hits + 1
    end)
end
```

#### `OnStart(self)`
Quick summary: Called once after `OnCreate` and after the scene is fully loaded. Use it for setup that depends on other objects already existing.

```lua
function Script:OnStart()
    Engine.Log("Hello from " .. tostring(self))
end
```

#### `OnUpdate(self, dt)`
Quick summary: Called every frame with the per-frame delta time in seconds. Keep work lightweight — this runs once per object per frame.

```lua
function Script:OnUpdate(dt)
    self.timer = (self.timer or 0) + dt
end
```

#### `OnDestroy(self)`
Quick summary: Called once when the object is destroyed (via `World.Destroy`, scene unload, or play-mode stop). Use it to release external resources or notify other systems.

```lua
function Script:OnDestroy()
    Engine.Log("Cleaning up")
end
```

### Trigger Callbacks

Objects with a `Trigger Volume` attribute — or an animator with a **Collision** bone modifier set to **Trigger** mode — can implement the following callbacks:

#### `OnTriggerEnter(self, objectName, otherName, phase, boneName)`
Quick summary: Called when another object first overlaps this trigger volume or bone trigger.

```lua
function Script:OnTriggerEnter(objectName, otherName, phase, boneName)
    if otherName == "Player" then
        Engine.Log(objectName .. " entered by " .. otherName .. " (" .. phase .. ")")
    end
end
```

#### `OnTriggerStay(self, objectName, otherName, phase, boneName)`
Quick summary: Called each frame while another object remains inside this trigger volume or bone trigger.

```lua
function Script:OnTriggerStay(objectName, otherName, phase, boneName)
    if otherName == "Player" then
        -- Keep this lightweight; it runs every frame during overlap.
    end
end
```

#### `OnTriggerExit(self, objectName, otherName, phase, boneName)`
Quick summary: Called when another object stops overlapping this trigger volume or bone trigger.

```lua
function Script:OnTriggerExit(objectName, otherName, phase, boneName)
    if otherName == "Player" then
        Engine.Log(objectName .. " exited by " .. otherName .. " (" .. phase .. ")")
    end
end
```

`otherName` is the other object in the overlap pair, and `phase` is one of `enter`, `stay`, `exit`.

`boneName` is set when the overlap was triggered by an animated **bone collider** in Trigger mode — it names the bone whose hitbox fired, so one object can host many distinct hitboxes (e.g. `"hand.R"`, `"foot.L"`). For ordinary `Trigger Volume` attributes it is an empty string. The callbacks are invoked on the script attached to the **object that owns the animator**.

Bone Collision modifiers in **Rigidbody** mode are purely physical: they are kinematic colliders that push dynamic rigidbodies the bone sweeps through, and they do **not** fire any script callbacks.

### Callback Restrictions

`World.Subscribe`, `World.SetTimeout`, `World.SetInterval`, `Time.Delay`, and `Time.Timer` must be called from inside an active script callback (`OnCreate`, `OnStart`, `OnUpdate`, a trigger callback, or another timer's callback). Calling them at file scope will error.
