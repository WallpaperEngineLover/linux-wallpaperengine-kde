#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace WallpaperEngine::Render {
/**
 * Script generated geometry (thisScene.createModelData, wallpaper64.exe 2.8.42 engine interface slots 10-12:
 * sub_14018C720, sub_1401D6400, sub_14018D8B0). Every shape becomes a mesh like the ones of a .mdl, model layers
 * created with the token draw them (sub_14021AD10)
 */
namespace ModelData {
    /** Vertex format bits a script can use (scenescript64 sub_18162FD40) */
    constexpr uint32_t FORMAT_POSITION = 0x1;
    constexpr uint32_t FORMAT_NORMAL = 0x2;
    constexpr uint32_t FORMAT_TANGENT_SIGNED = 0x4;
    constexpr uint32_t FORMAT_UV = 0x8;
    constexpr uint32_t FORMAT_COLOR = 0x8000;

    /** The error codes of the engine calls, scenescript64 turns them into these SyntaxError messages */
    enum class Error : int {
	None = 0,
	InvalidMaterial = 1,
	ShaderExpectingMoreVertexData = 2,
	InconsistentVertexBufferSize = 3,
	InvalidToken = 4,
	VertexBufferSizeCannotIncrease = 5,
	IndexBufferSizeCannotIncrease = 6,
	NotCreatedAsDynamic = 7,
	BufferLockFailed = 8,
	IncorrectIndexBufferType = 9,
	CannotAddShapes = 10,
	CannotDeleteShapes = 11,
	CannotChangeMaterial = 12,
	CannotChangeVertexFormat = 13,
    };

    [[nodiscard]] const char* errorMessage (Error error);

    /** One shape of a script configuration (scenescript64's 304 byte shape record, sub_181630720) */
    struct ShapeConfig {
	std::vector<float> vertices;
	bool hasVertices = false;
	std::vector<uint32_t> indices32;
	std::vector<uint16_t> indices16;
	bool hasIndices32 = false;
	bool hasIndices16 = false;
	/** indexBuffer: null */
	bool deleteIndices = false;
	uint32_t format = 0;
	std::string material;
	bool vertexDynamic = false;
	bool indexDynamic = false;
	/** the shape itself was null (replaceData deletes it) */
	bool deleteShape = false;

	[[nodiscard]] bool hasIndices () const { return this->hasIndices32 || this->hasIndices16; }
	[[nodiscard]] uint32_t indexCount () const {
	    return static_cast<uint32_t> (this->hasIndices32 ? this->indices32.size () : this->indices16.size ());
	}
    };

    struct Config {
	std::vector<ShapeConfig> shapes;
	std::optional<glm::vec3> boundsMin;
	std::optional<glm::vec3> boundsMax;
    };

    /** A mesh with its GPU buffer record: the 200 byte mesh (sub_1401C2F10) and the 32 byte buffer entry of WE's model
     */
    struct Mesh {
	std::string material;
	uint32_t format = 0;
	uint32_t stride = 0;
	/** buffer +18 */
	bool indexed = false;
	/** mesh flag 1, 32 bit indices */
	bool wideIndices = false;
	/** buffer +16 / +17 */
	bool vertexDynamic = false;
	bool indexDynamic = false;
	/** buffer +8 / +12: float and index counts the buffers were created with */
	uint32_t vertexFloats = 0;
	uint32_t indexCount = 0;
	std::vector<char> vertices;
	std::vector<char> indices;
	/** bumped whenever applyData writes into the buffers */
	uint64_t revision = 0;
    };

    struct Model {
	uint32_t token = 0;
	/** +560: the script handle and every layer using it hold one */
	int references = 1;
	std::vector<Mesh> meshes;
	glm::vec3 boundsMin = glm::vec3 (0.0f);
	glm::vec3 boundsMax = glm::vec3 (0.0f);
	/** bumped when replaceData changes meshes, the layers using it rebuild (the listeners sub_1401D6400 calls) */
	uint64_t structure = 0;
    };

    /** Checks a shape's material against its vertex format: InvalidMaterial / ShaderExpectingMoreVertexData or None */
    using MaterialCheck = std::function<Error (const std::string& material, uint32_t format)>;

    class Store {
    public:
	/** sub_14018C720, 0 and error set on failure */
	uint32_t create (const Config& config, const MaterialCheck& check, Error& error);
	/** sub_1401D6400: applyData (replace false) or replaceData */
	void apply (uint32_t token, const Config& config, bool replace, const MaterialCheck& check, Error& error);
	/** sub_14018D8B0: drops one reference */
	void release (uint32_t token);
	/** A layer takes a reference (sub_14021AD10), nullptr for an unknown token */
	std::shared_ptr<Model> acquire (int token);

    private:
	std::map<uint32_t, std::shared_ptr<Model>> m_models;
	/** scene +7224, zeroed with the scene, the first token is 1 */
	uint32_t m_counter = 0;
    };
} // namespace ModelData
} // namespace WallpaperEngine::Render
