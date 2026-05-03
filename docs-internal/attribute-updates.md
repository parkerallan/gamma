How to expose future attribute settings:

1. Add a new accessor id in the enum in src/render/RuntimeRenderer.h.
2. Add one binding row in the descriptor list in src/render/RuntimeRenderer.cpp:
Table name + method name + accessor id.
3. Add one switch case in the shared dispatcher in src/render/RuntimeScriptAPI.cpp, usually using existing helpers:
access_string, access_float, access_bool, access_vec2, access_vec3.
4. If the property has runtime side effects, add it in src/render/RuntimeRenderer.cpp:
Physics-related mutations rebuild physics, camera active mutation refreshes active camera.
5. Document the method in docs/developer-api.md.

this is now attribute-extensible without creating brand-new Lua C functions per property. You mostly add data rows plus one accessor case.
