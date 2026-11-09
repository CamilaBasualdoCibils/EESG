#include <Lighthouse/Engine/Assets/Assets.hpp>
#include <stdexcept>
int main() {
 const auto asset = Lighthouse::Assets::LoadGLTF(std::filesystem::path(EESG_ASSET_ROOT) / "DamagedHelmet.gltf");
 if (asset.meshes.empty() || asset.images.size() != 5 || asset.textures.size() != 5)
   throw std::runtime_error("Unexpected game glTF contents");
}
