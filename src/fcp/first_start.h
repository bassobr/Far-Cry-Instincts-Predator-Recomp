// Reading the player's disc image on the first start: default.xex for the game
// code build, the shader containers of shaders.fat/common.fat for the shader pack. The image is only read.
//
// Accepted: an .iso (XDVDFS: plain XISO, XGD2, XGD3, full Redump images; same partition offsets as tools/xiso.py)
// or a folder with the extracted disc (default.xex at its top).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fcp::first_start {

/** The disc image of this start: FCP_GAME_ISO (from the environment or [game] image of predator.toml). */
std::filesystem::path ImagePath();

/** Reads one file of the disc (path relative to its root, backslashes or slashes, any case). */
bool ReadDiscFile(const std::filesystem::path& image, const std::string& name, std::vector<uint8_t>& out,
                  std::string& why);

/** SHA-256 (hex) of default.xex in the image, remembered in <cache> per image path, size and time. Empty on error. */
std::string XexSha256(const std::filesystem::path& image, const std::filesystem::path& cache, std::string& why);

/** Shader pack target of this start: "spirv" (Vulkan) or "dxil" (D3D12), as src/fcp/render_bridge.cpp picks it. */
std::string PackTarget();

/** Where this start's shader pack goes: <user folder>\shaders\predator.<target>.fcpack. */
std::filesystem::path PackPath(const std::string& target);

/** True when the renderer finds a pack (next to the EXE, FCP_SHADER_PACK, or in the user folder). Sets
    FCP_SHADER_PACK to the user folder's pack when only that one exists. */
bool UseExistingPack(const std::string& target);

/** Builds the pack from the image: containers -> <work>\fxo, then fcshaders.exe pack. Progress in the window. */
bool BuildShaderPack(const std::filesystem::path& image, const std::filesystem::path& work, const std::string& target,
                     const std::filesystem::path& log, std::string& why);

}  // namespace fcp::first_start
