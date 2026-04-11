#pragma once

#include "assets/SceneMetadata.h"
#include "state/EngineState.h"

#include <functional>

using SceneObjectCameraPreviewCallback = std::function<void(
	EngineState& state,
	const SceneObjectMetadata& object,
	std::size_t attribute_index,
	const SceneObjectAttribute& attribute)>;

bool RenderSceneObjectAttributesEditor(
	EngineState& state,
	const SceneObjectMetadata& object,
	const SceneObjectCameraPreviewCallback& render_camera_preview = SceneObjectCameraPreviewCallback{});