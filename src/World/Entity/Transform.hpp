#pragma once

#include "Math/Types.hpp"
#include "World/Entity/Entity.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <optional>


namespace EESG {

class Transform {
    Vec3 position{};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};
    mutable bool matrixDirty = true;
    mutable std::optional<Mat4> matrix;
    std::optional<EntityID> parent;
public:
    Transform() = default;
    Transform(Vec3 p,Quat r = Quat(), Vec3 s = Vec3(1.0f, 1.0f, 1.0f))
        : position(p), rotation(r), scale(s) {}
        Transform(Vec3 p, Vec3 rEuler = Vec3(0.0f, 0.0f, 0.0f), Vec3 s = Vec3(1.0f, 1.0f, 1.0f))
        : position(p), rotation(Quat(rEuler)), scale(s) {}
    //explicit Transform(Vec3 position);

    void SetPos(Vec3 p)
    {
        position=p;
        matrixDirty=true;
    }
    constexpr Vec3 GetPos() const
    {
        return position;
    }
    void SetRot(Quat r)
    {
        rotation = r;
        matrixDirty=true;
    }
    constexpr Quat GetRot() const
    {
        return rotation;
    }
    void SetRotEuler(Vec3 eulerAngles)
    {
        rotation = Quat(eulerAngles);
        matrixDirty = true;
    }
    constexpr Vec3 GetRotEuler() const
    {
        return glm::eulerAngles(rotation);
    }

    void SetScale(Vec3 s)
    {
        scale = s;
        matrixDirty = true;
    }
    constexpr Vec3 GetScale() const
    {
        return scale;
    }
    Mat4 GetMatrix() const
    {
        if (matrixDirty || !matrix.has_value()) {
            matrix = ProduceMatrix(*this);
            matrixDirty = false;
        }
        return *matrix;
    }

    bool HasParent() const
    {
        return parent.has_value();
    }
    std::optional<EntityID> GetParent() const
    {
        return parent;
    }
    void SetParent(EntityID e)
    {
        parent = e;
    }
    void RemoveParent()
    {
        parent.reset();
    }
    private:
    static Mat4 ProduceMatrix(const Transform& t)
    {
        Mat4 translation = glm::translate(Mat4(1.0f), t.position);
        Mat4 rotation = glm::mat4(t.rotation);
        Mat4 scaling = glm::scale(Mat4(1.0f), t.scale);
        return translation * rotation * scaling;
    }
};

} // namespace EESG
