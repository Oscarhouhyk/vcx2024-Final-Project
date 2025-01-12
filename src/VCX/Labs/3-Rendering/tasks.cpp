#include "Labs/3-Rendering/tasks.h"

namespace VCX::Labs::Rendering {

    glm::vec4 GetTexture(Engine::Texture2D<Engine::Formats::RGBA8> const & texture, glm::vec2 const & uvCoord) {
        if (texture.GetSizeX() == 1 || texture.GetSizeY() == 1) return texture.At(0, 0);
        glm::vec2 uv      = glm::fract(uvCoord);
        uv.x              = uv.x * texture.GetSizeX() - .5f;
        uv.y              = uv.y * texture.GetSizeY() - .5f;
        std::size_t xmin  = std::size_t(glm::floor(uv.x) + texture.GetSizeX()) % texture.GetSizeX();
        std::size_t ymin  = std::size_t(glm::floor(uv.y) + texture.GetSizeY()) % texture.GetSizeY();
        std::size_t xmax  = (xmin + 1) % texture.GetSizeX();
        std::size_t ymax  = (ymin + 1) % texture.GetSizeY();
        float       xfrac = glm::fract(uv.x), yfrac = glm::fract(uv.y);
        return glm::mix(glm::mix(texture.At(xmin, ymin), texture.At(xmin, ymax), yfrac), glm::mix(texture.At(xmax, ymin), texture.At(xmax, ymax), yfrac), xfrac);
    }

    glm::vec4 GetAlbedo(Engine::Material const & material, glm::vec2 const & uvCoord) {
        glm::vec4 albedo       = GetTexture(material.Albedo, uvCoord);
        glm::vec3 diffuseColor = albedo;
        return glm::vec4(glm::pow(diffuseColor, glm::vec3(2.2)), albedo.w);
    }

    /******************* 1. Ray-triangle intersection *****************/
    bool IntersectTriangle(Intersection & output, Ray const & ray, glm::vec3 const & p1, glm::vec3 const & p2, glm::vec3 const & p3) {
        // your code here
        glm::vec3 edge1 = p2 - p1;
        glm::vec3 edge2 = p3 - p1;
        glm::vec3 T = ray.Origin - p1;
        glm::vec3 P = glm::cross(ray.Direction, edge2);
        glm::vec3 Q = glm::cross(T, edge1);

        float a = glm::dot(edge1, P);

        float f = 1.0f / a;
       
        float u = f * glm::dot(T, P);
        float v = f * glm::dot(ray.Direction, Q);
        float t = f * glm::dot(edge2, Q);

        if (u < 0.0f || v < 0.0f || u + v > 1.0f) {
            return false; 
        }
        
        if (t >= 0.0f && t < INFINITY) {
            output.t = t;
            output.u = u;
            output.v = v;
            return true;
        }

        return false;
    }

    glm::vec3 RayTrace(const RayIntersector & intersector, Ray ray, int maxDepth, bool enableShadow) {
 glm::vec3 color(0.0f);
    glm::vec3 throughput(1.0f); // 光线的累积贡献

    for (int depth = 0; depth < maxDepth; depth++) {
        auto rayHit = intersector.IntersectRay(ray);
        if (!rayHit.IntersectState) break; // 如果没有交点，返回背景色

        // 获取交点的材质属性
        const glm::vec3 pos   = rayHit.IntersectPosition;
        const glm::vec3 n     = rayHit.IntersectNormal;
        const glm::vec3 kd    = rayHit.IntersectAlbedo;       // 漫反射
        const glm::vec3 ks    = rayHit.IntersectMetaSpec;     // 镜面反射
        const float     alpha = rayHit.IntersectAlbedo.w;     // 透明度（折射相关）

        // 直接光照：计算光源的贡献
        glm::vec3 directLight(0.0f);
        for (const Engine::Light & light : intersector.InternalScene->Lights) {
            glm::vec3 l;
            float attenuation;

            if (light.Type == Engine::LightType::Point) {
                l           = light.Position - pos;
                attenuation = 1.0f / glm::dot(l, l);
            } else if (light.Type == Engine::LightType::Directional) {
                l           = light.Direction;
                attenuation = 1.0f;
            }

            if (enableShadow) {
                Ray shadowRay(pos + 1e-4f * n, glm::normalize(l));
                auto shadowHit = intersector.IntersectRay(shadowRay);
                if (shadowHit.IntersectState) continue; // 阴影阻挡，跳过当前光源
            }

            glm::vec3 lightDir = glm::normalize(l);
            float NdotL = glm::max(glm::dot(n, lightDir), 0.0f);

            // 漫反射 + 镜面反射
            glm::vec3 diffuse  = kd * NdotL * light.Intensity * attenuation;
            glm::vec3 specular = ks * glm::pow(glm::max(0.0f, glm::dot(glm::reflect(-lightDir, n), -ray.Direction)), 16.0f) * light.Intensity * attenuation;

            directLight += diffuse + specular;
        }

        // 累加直接光照
        color += throughput * directLight;

        // 间接光照：基于材质类型决定反射或折射方向
        float p = glm::max(kd.r, glm::max(kd.g, kd.b)); // 俄罗斯轮盘概率
        if (depth > 3 && glm::linearRand(0.0f, 1.0f) >= p) break; // 终止递归
        throughput /= p; // 补偿被终止路径的贡献

        if (alpha < 0.9f) {
            // 折射逻辑
            glm::vec3 refractedDir = glm::refract(ray.Direction, n, 1.0f / 1.5f); // 假设折射率为 1.5
            ray = Ray(pos - 1e-4f * n, refractedDir);
        } else {
            // 反射逻辑
            glm::vec3 reflectedDir = glm::reflect(ray.Direction, n);
            ray = Ray(pos + 1e-4f * n, reflectedDir);
        }

        // 更新光线的贡献
        throughput *= kd; // 假设材质的反射率与漫反射颜色一致
    }

    return color;
    }
} // namespace VCX::Labs::Rendering