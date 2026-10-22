#include "WallpaperEngine/Render/Objects/PuppetClipping.h"
#include "WallpaperEngine/Render/Objects/PuppetMeshFile.h"
#include "WallpaperEngine/Render/Objects/PuppetRig.h"

#include <tuple>
#include <vector>

using namespace WallpaperEngine::Render::Objects;

extern "C" int LLVMFuzzerTestOneInput (const uint8_t* bytes, size_t size) {
    const std::vector<char> data (reinterpret_cast<const char*> (bytes), reinterpret_cast<const char*> (bytes) + size);
    const auto mesh = readPuppetMeshFile (data);

    try {
	std::ignore = readMdlvMeshes (data);
    } catch (const std::exception&) { }

    if (!mesh.has_value ()) {
	return 0;
    }

    try {
	auto clipping = PuppetClipping::read (data, mesh->indexEnd, mesh->version, mesh->indices.size ());

	if (clipping.has_value ()) {
	    clipping->build (mesh->indices);
	}
    } catch (const std::exception&) { }

    if (mesh->skeleton < data.size () && !mesh->blendIndices.empty ()) {
	try {
	    PuppetRig rig;
	    rig.load (data, mesh->sections, mesh->meshCount, "fuzz");
	    rig.addSceneLayers ({});

	    for (int i = 0; i < 4; i++) {
		rig.updatePose (glm::mat4 (1.0f));
	    }
	} catch (const std::exception&) { }
    }

    return 0;
}
