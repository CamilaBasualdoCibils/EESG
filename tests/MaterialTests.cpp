#include "Materials/Materials.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>

using namespace EESG;
static void Check(bool condition)
{
  if (!condition) throw std::runtime_error("Material check failed");
}
template <class F> static void Rejects(F function)
{
  bool rejected = false;
  try { function(); }
  catch (const std::exception&) { rejected = true; }
  Check(rejected);
}

int main()
{
  Materials materials;
  const auto solid = materials.Load("materials/SolidColor.glsl");
  Check(solid->model == SurfaceModel::Unlit);
  Check(solid->source.find("struct UnlitSurface") != std::string::npos);
  Check(solid->source.find("evaluateMaterial") != std::string::npos);
  Check(materials.Load(solid->path) == solid);
  Check(materials.Load("materials/../materials/SolidColor.glsl") == solid);
  Rejects([&] { materials.Load("materials/DoesNotExist.glsl"); });

  const auto temporary = std::filesystem::temp_directory_path() /
      ("eesg-material-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + ".glsl");
  struct Cleanup
  {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
  } cleanup{temporary};
  // A wrong return type must fail, and failure must not poison the cache.
  { std::ofstream file(temporary); file << "float evaluateMaterial(SurfaceInput s, MaterialParameters p) { return 1.0; }"; }
  Rejects([&] { materials.Load(temporary); });
  { std::ofstream file(temporary); file << "UnlitSurface evaluateMaterial(SurfaceInput s, MaterialParameters p) { return UnlitSurface(p.baseColor.rgb, p.baseColor.a); }"; }
  Check(materials.Load(temporary)->model == SurfaceModel::Unlit);
}
