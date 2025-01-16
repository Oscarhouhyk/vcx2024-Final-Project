#include <spdlog/spdlog.h>

#include "Labs/3-Rendering/CaseShadow.h"
#include "Labs/Common/ImGuiHelper.h"

namespace VCX::Labs::Rendering {
    static constexpr auto c_ShadowFrameSize = std::pair<std::uint32_t, std::uint32_t> { 4096U, 4096U };
    static constexpr auto c_ShadowCubeFrameSize = std::pair<std::uint32_t, std::uint32_t> { 2048U, 2048U };

    CaseShadow::CaseShadow(std::initializer_list<Assets::ExampleScene> && scenes):
        _scenes(scenes),
        _shadowMapProgram(
            Engine::GL::UniqueProgram({ Engine::GL::SharedShader("assets/shaders/shadowmap.vert"),
                                        Engine::GL::SharedShader("assets/shaders/shadowmap.frag") })),
        _shadowCubeMapProgram(
            Engine::GL::UniqueProgram({ Engine::GL::SharedShader("assets/shaders/shadowcubemap.vert"),
                                        Engine::GL::SharedShader("assets/shaders/shadowcubemap.geom"),
                                        Engine::GL::SharedShader("assets/shaders/shadowcubemap.frag") })),
        _shadingProgram(
            Engine::GL::UniqueProgram({ Engine::GL::SharedShader("assets/shaders/phong-shadow.vert"),
                                        Engine::GL::SharedShader("assets/shaders/phong-shadow.frag") })),
        _shadingCubeProgram(
            Engine::GL::UniqueProgram({ Engine::GL::SharedShader("assets/shaders/phong-shadowcubemap.vert"),
                                        Engine::GL::SharedShader("assets/shaders/phong-shadowcubemap.frag") })),
        _sceneObject(3) {
        _cameraManager.AutoRotate = false;
        _shadingProgram.BindUniformBlock("PassConstants", 3);
        _shadingProgram.GetUniforms().SetByName("u_DiffuseMap", 0);
        _shadingProgram.GetUniforms().SetByName("u_SpecularMap", 1);
        _shadingProgram.GetUniforms().SetByName("u_ShadowMap", 2);
        _shadingCubeProgram.BindUniformBlock("PassConstants", 3);
        _shadingCubeProgram.GetUniforms().SetByName("u_DiffuseMap", 0);
        _shadingCubeProgram.GetUniforms().SetByName("u_SpecularMap", 1);
        _shadingCubeProgram.GetUniforms().SetByName("u_ShadowCubeMap", 2);
        _shadowMapProgram.GetUniforms().SetByName("u_DiffuseMap", 0);
        _shadowCubeMapProgram.GetUniforms().SetByName("u_DiffuseMap", 0);
        _shadowFrame.GetDepthStencilAttachment().SetUnit(2);
        _shadowFrame.Resize(c_ShadowFrameSize);
        _shadowCubeFrame.GetDepthStencilAttachment().SetUnit(2);
        _shadowCubeFrame.Resize(c_ShadowCubeFrameSize);
    }

    void CaseShadow::OnSetupPropsUI() {
        if (ImGui::BeginCombo("Scene", GetSceneName(_sceneIdx))) {
            for (std::size_t i = 0; i < _scenes.size(); ++i) {
                bool selected = i == _sceneIdx;
                if (ImGui::Selectable(GetSceneName(i), selected)) {
                    if (! selected) {
                        _sceneIdx           = i;
                        _recompute          = true;
                        _uniformDirty = true;
                    }
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Combo("Anti-Aliasing", &_msaa, "None\0002x MSAA\0004x MSAA\0008x MSAA\0");
        Common::ImGuiHelper::SaveImage(_frame.GetColorAttachment(), _frame.GetSize(), true);
        ImGui::Spacing();
        _uniformDirty |= ImGui::RadioButton("Hard Shadow", &_useSoftShadow, 0);
        ImGui::SameLine();
        _uniformDirty |= ImGui::RadioButton("Soft Shadow", &_useSoftShadow, 1);
        ImGui::Spacing();

        _uniformDirty |= ImGui::RadioButton("PCF", &_usePCF, 1);
        ImGui::SameLine();
        _uniformDirty |= ImGui::RadioButton("PCSS", &_usePCF, 0);
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen)) {
            _uniformDirty |= ImGui::SliderFloat("Ambient", &_ambientScale, 0.f, 2.f, "%.2fx");
            ImGui::Spacing();

            _uniformDirty |= ImGui::RadioButton("Uniform Sample", &_useUniform, 1);
            ImGui::SameLine();
            _uniformDirty |= ImGui::RadioButton("Poisson Sample", &_useUniform, 0);
            ImGui::Spacing();


            _uniformDirty |= ImGui::SliderFloat("PCF Bias", &_pcfBiasC, 0.00f, 0.10f, "%.2f");
            ImGui::Spacing();

            _uniformDirty |= ImGui::SliderFloat("Filter Radius", &_filterRadius, 5.0f, 50.0f, "%.0f");
           
           //_uniformDirty |= ImGui::SliderFloat("Frustrum Size", &_frustrumSize, 0.00f, 1000.0f, "%.0f");

           //_uniformDirty |= ImGui::SliderFloat("Light World Size", &_lightworldSize, 0.00f, 100.0f, "%.0f");

           // _uniformDirty |= ImGui::SliderInt("Poisson Ring", &_poissonRingSize, 0, 100);
    
        }
        ImGui::Spacing();

        if (ImGui::CollapsingHeader("Control")) {
            ImGui::Checkbox("Zoom Tooltip", &_enableZoom);
            ImGui::Checkbox("Ease Touch", &_cameraManager.EnableDamping);
        }
        ImGui::Spacing();
    }

    Common::CaseRenderResult CaseShadow::OnRender(std::pair<std::uint32_t, std::uint32_t> const desiredSize) {
        if (_recompute) {
            _recompute = false;
            _sceneObject.ReplaceScene(GetScene(_sceneIdx));
            _cameraManager.Save(_sceneObject.Camera);
            if (_sceneObject.CntPointLights > 0) { // light 0 is point light
                float                    aspec    = float(c_ShadowCubeFrameSize.first) / c_ShadowCubeFrameSize.second;
                float                    zfar     = _sceneObject.Camera.ZFar;
                glm::mat4                proj     = glm::perspective(glm::radians(90.f), aspec, _sceneObject.Camera.ZNear / 10, zfar);
                const glm::vec3 &        lightPos = _sceneObject.Lights[0].Position;
                std::array<glm::mat4, 6> lightCubeTransforms;
                lightCubeTransforms[0] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(1., 0., 0.), glm::vec3(0., -1., 0.));
                lightCubeTransforms[1] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(-1., 0., 0.), glm::vec3(0., -1., 0.));
                lightCubeTransforms[2] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(0., 1., 0.), glm::vec3(0., 0., 1.));
                lightCubeTransforms[3] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(0., -1., 0.), glm::vec3(0., 0., -1.));
                lightCubeTransforms[4] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(0., 0., 1.), glm::vec3(0., -1., 0.));
                lightCubeTransforms[5] = proj * glm::lookAt(lightPos, lightPos + glm::vec3(0., 0., -1.), glm::vec3(0., -1., 0.));
                _shadowCubeMapProgram.GetUniforms().SetByName("u_LightMatrices", lightCubeTransforms);
                _shadowCubeMapProgram.GetUniforms().SetByName("u_LightPosition", lightPos);
                _shadowCubeMapProgram.GetUniforms().SetByName("u_FarPlane", zfar);
                _shadingCubeProgram.GetUniforms().SetByName("u_FarPlane", zfar);
            } else { // light 0 is directional light
                const auto [minAABB, maxAABB] = GetScene(_sceneIdx).GetAxisAlignedBoundingBox();
                glm::vec3 lightLookAt         = (minAABB + maxAABB) * glm::vec3(0.5);
                float     offset              = 0;
                glm::vec3 lightDir            = _sceneObject.Lights[0].Direction;
                for (int i = 0; i < 3; ++i)
                    offset += std::max(lightDir[i] * minAABB[i], lightDir[i] * maxAABB[i]);
                offset                    = offset - glm::dot(lightLookAt, lightDir) + _sceneObject.Camera.ZNear;
                glm::vec3 lightPosition   = lightLookAt + offset * lightDir;
                float     shadowMapSize   = 0.15 * _sceneObject.Camera.ZFar;
                glm::mat4 lightProjection = glm::ortho(-shadowMapSize / 2, shadowMapSize / 2, -shadowMapSize / 2, shadowMapSize / 2, 0.f, _sceneObject.Camera.ZFar);
                glm::mat4 lightView       = glm::lookAt(lightPosition, lightLookAt, glm::vec3(0, 1, 0));
                _shadowMapProgram.GetUniforms().SetByName("u_lightSpaceMatrix", lightProjection * lightView);
                _shadingProgram.GetUniforms().SetByName("u_lightSpaceMatrix", lightProjection * lightView);
            }
        }
        _frame.Resize(desiredSize, 1 << _msaa);

        _cameraManager.Update(_sceneObject.Camera);
        _sceneObject.PassConstantsBlock.Update(&SceneObject::PassConstants::Projection, _sceneObject.Camera.GetProjectionMatrix((float(desiredSize.first) / desiredSize.second)));
        _sceneObject.PassConstantsBlock.Update(&SceneObject::PassConstants::View, _sceneObject.Camera.GetViewMatrix());
        _sceneObject.PassConstantsBlock.Update(&SceneObject::PassConstants::ViewPosition, _sceneObject.Camera.Eye);

        if (_uniformDirty) {
            _uniformDirty = false;

            _shadingProgram.GetUniforms().SetByName("u_AmbientScale", _ambientScale);
            _shadingCubeProgram.GetUniforms().SetByName("u_AmbientScale", _ambientScale);
            _shadingProgram.GetUniforms().SetByName("u_useUniform", _useUniform);
            _shadingCubeProgram.GetUniforms().SetByName("u_useUniform", _useUniform);
            _shadingProgram.GetUniforms().SetByName("u_useSoftShadow", _useSoftShadow);
            _shadingCubeProgram.GetUniforms().SetByName("u_useSoftShadow", _useSoftShadow);

            _shadingProgram.GetUniforms().SetByName("pcfBiasC", _pcfBiasC);
            _shadingCubeProgram.GetUniforms().SetByName("pcfBiasC", _pcfBiasC);
            _shadingProgram.GetUniforms().SetByName("FILTER_RADIUS", _filterRadius);
            _shadingCubeProgram.GetUniforms().SetByName("FILTER_RADIUS", _filterRadius);
            _shadingProgram.GetUniforms().SetByName("u_usePCF", _usePCF);
            _shadingCubeProgram.GetUniforms().SetByName("u_usePCF", _usePCF);
            //_shadingProgram.GetUniforms().SetByName("FRUSTRUM_SIZE", _frustrumSize);
           // _shadingCubeProgram.GetUniforms().SetByName("FRUSTRUM_SIZE", _frustrumSize);
            //_shadingProgram.GetUniforms().SetByName("LIGHT_WORLD_SIZE", _lightworldSize);
            //_shadingCubeProgram.GetUniforms().SetByName("LIGHT_WORLD_SIZE", _lightworldSize);
            //_shadingProgram.GetUniforms().SetByName("poisson_ring_size", _poissonRingSize);
        }

        if (_sceneObject.CntPointLights > 0) { // light 0 is point light
            {
                gl_using(_shadowCubeFrame);
                glEnable(GL_DEPTH_TEST);
                for (auto const & model : _sceneObject.OpaqueModels) {
                    auto const & material = _sceneObject.Materials[model.MaterialIndex];
                    model.Mesh.Draw({ material.Albedo.Use(), _shadowCubeMapProgram.Use() });
                }
                glDisable(GL_DEPTH_TEST);
            }
            {
                gl_using(_frame);
                glEnable(GL_DEPTH_TEST);
                for (auto const & model : _sceneObject.OpaqueModels) {
                    auto const & material = _sceneObject.Materials[model.MaterialIndex];
                    model.Mesh.Draw({ material.Albedo.Use(), material.MetaSpec.Use(), _shadowCubeFrame.GetDepthStencilAttachment().Use(), _shadingCubeProgram.Use() });
                }

                glDisable(GL_DEPTH_TEST);
            }
        } else { // light 0 is directional light
            {
                gl_using(_shadowFrame);
                glEnable(GL_DEPTH_TEST);
                glClear(GL_DEPTH_BUFFER_BIT);
                for (auto const & model : _sceneObject.OpaqueModels) {
                    auto const & material = _sceneObject.Materials[model.MaterialIndex];
                    model.Mesh.Draw({ material.Albedo.Use(), _shadowMapProgram.Use() });
                }
                glDisable(GL_DEPTH_TEST);
            }
            {
                gl_using(_frame);

                glEnable(GL_DEPTH_TEST);
                for (auto const & model : _sceneObject.OpaqueModels) {
                    auto const & material = _sceneObject.Materials[model.MaterialIndex];
                    model.Mesh.Draw({ material.Albedo.Use(), material.MetaSpec.Use(), _shadowFrame.GetDepthStencilAttachment().Use(), _shadingProgram.Use() });
                }

                glDisable(GL_DEPTH_TEST);
            }
        }

        return Common::CaseRenderResult {
            .Fixed     = false,
            .Flipped   = true,
            .Image     = _frame.GetColorAttachment(),
            .ImageSize = desiredSize,
        };
    }

    void CaseShadow::OnProcessInput(ImVec2 const & pos) {
        _cameraManager.ProcessInput(_sceneObject.Camera, pos);
        if(_enableZoom)
            Common::ImGuiHelper::ZoomTooltip(_frame.GetColorAttachment(), _frame.GetSize(), pos, true);
    }
} // namespace VCX::Labs::Rendering
