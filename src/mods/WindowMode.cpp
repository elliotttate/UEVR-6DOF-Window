#include "WindowMode.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include <d3dcompiler.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include "VR.hpp"

#pragma comment(lib, "d3dcompiler")

namespace {

constexpr char k_shader[] = R"(
cbuffer Constants : register(b0) {
    float4 eye_origin_and_full_mask;
    float4 ray_center_and_curvature;
    float4 ray_x_and_feather;
    float4 ray_y_and_corner_radius;
    float4 anchor_origin_and_distance;
    float4 anchor_right_and_half_width;
    float4 anchor_up_and_half_height;
    float4 anchor_back;
    float4 surround_color_and_opacity;
};

struct PSIn {
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

PSIn vs_main(float2 pos : POSITION) {
    PSIn output;
    output.pos = float4(pos, 0.0f, 1.0f);
    output.ndc = pos;
    return output;
}
float4 ps_main(PSIn input) : SV_TARGET {
    float mask = 1.0f;

    if (eye_origin_and_full_mask.w < 0.5f) {
        float3 eye_origin = eye_origin_and_full_mask.xyz;
        float3 ray_direction = normalize(
            ray_center_and_curvature.xyz +
            input.ndc.x * ray_x_and_feather.xyz +
            input.ndc.y * ray_y_and_corner_radius.xyz);
        float3 origin = anchor_origin_and_distance.xyz;
        float3 right = anchor_right_and_half_width.xyz;
        float3 up = anchor_up_and_half_height.xyz;
        float3 back = anchor_back.xyz;
        float distance = max(anchor_origin_and_distance.w, 0.001f);
        float curvature = saturate(ray_center_and_curvature.w);
        float hit_t = -1.0f;
        float local_x = 0.0f;
        float local_y = 0.0f;

        if (curvature <= 0.0001f) {
            float3 plane_center = origin - back * distance;
            float denominator = dot(ray_direction, back);
            if (abs(denominator) > 0.00001f) {
                hit_t = dot(plane_center - eye_origin, back) / denominator;
                if (hit_t > 0.0001f) {
                    float3 local = eye_origin + ray_direction * hit_t - plane_center;
                    local_x = dot(local, right);
                    local_y = dot(local, up);
                }
            }
        } else {
            // Curvature 1.0 places the cylinder axis at the recenter origin,
            // so the aperture wraps around the player. Smaller values move
            // the axis backward and approach the flat-plane solution.
            float radius = distance / curvature;
            float3 cylinder_center = origin + back * (radius - distance);
            float3 offset = eye_origin - cylinder_center;
            float3 horizontal_ray = ray_direction - up * dot(ray_direction, up);
            float3 horizontal_offset = offset - up * dot(offset, up);
            float a = dot(horizontal_ray, horizontal_ray);
            float b = 2.0f * dot(horizontal_offset, horizontal_ray);
            float c = dot(horizontal_offset, horizontal_offset) - radius * radius;
            float discriminant = b * b - 4.0f * a * c;
            if (a > 0.000001f && discriminant >= 0.0f) {
                float root = sqrt(discriminant);
                float near_t = (-b - root) / (2.0f * a);
                float far_t = (-b + root) / (2.0f * a);
                hit_t = near_t > 0.0001f ? near_t : (far_t > 0.0001f ? far_t : -1.0f);
                if (hit_t > 0.0f) {
                    float3 hit = eye_origin + ray_direction * hit_t;
                    float3 cylinder_local = hit - cylinder_center;
                    local_x = atan2(dot(cylinder_local, right), -dot(cylinder_local, back)) * radius;
                    local_y = dot(hit - origin, up);
                }
            }
        }

        if (hit_t > 0.0f) {
            float2 half_size = float2(
                anchor_right_and_half_width.w,
                anchor_up_and_half_height.w);
            float corner_radius = clamp(
                ray_y_and_corner_radius.w,
                0.0f,
                min(half_size.x, half_size.y));
            float2 rounded = abs(float2(local_x, local_y)) - (half_size - corner_radius);
            float signed_distance =
                length(max(rounded, 0.0f)) +
                min(max(rounded.x, rounded.y), 0.0f) -
                corner_radius;
            float feather = max(ray_x_and_feather.w, 0.0f);
            mask = feather > 0.00001f
                ? smoothstep(-feather, feather, signed_distance)
                : (signed_distance >= 0.0f ? 1.0f : 0.0f);
        }
    }

    return float4(
        surround_color_and_opacity.rgb,
        mask * saturate(surround_color_and_opacity.a));
}
)";

constexpr float k_vertices[]{
    -1.0f, -1.0f,
    -1.0f,  1.0f,
     1.0f,  1.0f,
    -1.0f, -1.0f,
     1.0f,  1.0f,
     1.0f, -1.0f,
};

DXGI_FORMAT resolve_typeless(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

class D3D11StateBackup {
public:
    explicit D3D11StateBackup(ID3D11DeviceContext* context) : m_context{context} {
        m_context->OMGetRenderTargets(1, m_rtv.GetAddressOf(), m_dsv.GetAddressOf());
        m_context->OMGetBlendState(m_blend.GetAddressOf(), m_blend_factor, &m_sample_mask);
        m_context->OMGetDepthStencilState(m_depth.GetAddressOf(), &m_stencil_ref);
        m_context->RSGetState(m_rasterizer.GetAddressOf());

        m_viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_scissor_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        m_context->RSGetViewports(&m_viewport_count, m_viewports);
        m_context->RSGetScissorRects(&m_scissor_count, m_scissors);

        m_context->VSGetShader(m_vs.GetAddressOf(), nullptr, nullptr);
        m_context->PSGetShader(m_ps.GetAddressOf(), nullptr, nullptr);
        m_context->GSGetShader(m_gs.GetAddressOf(), nullptr, nullptr);
        m_context->HSGetShader(m_hs.GetAddressOf(), nullptr, nullptr);
        m_context->DSGetShader(m_ds.GetAddressOf(), nullptr, nullptr);
        m_context->VSGetConstantBuffers(0, 1, m_vs_cb.GetAddressOf());
        m_context->PSGetConstantBuffers(0, 1, m_ps_cb.GetAddressOf());

        m_context->IAGetInputLayout(m_input_layout.GetAddressOf());
        m_context->IAGetPrimitiveTopology(&m_topology);
        m_context->IAGetVertexBuffers(0, 1, m_vertex_buffer.GetAddressOf(), &m_vertex_stride, &m_vertex_offset);
        m_context->IAGetIndexBuffer(m_index_buffer.GetAddressOf(), &m_index_format, &m_index_offset);
    }

    ~D3D11StateBackup() {
        ID3D11RenderTargetView* rtv = m_rtv.Get();
        m_context->OMSetRenderTargets(1, &rtv, m_dsv.Get());
        m_context->OMSetBlendState(m_blend.Get(), m_blend_factor, m_sample_mask);
        m_context->OMSetDepthStencilState(m_depth.Get(), m_stencil_ref);
        m_context->RSSetState(m_rasterizer.Get());
        m_context->RSSetViewports(m_viewport_count, m_viewports);
        m_context->RSSetScissorRects(m_scissor_count, m_scissors);

        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
        m_context->PSSetShader(m_ps.Get(), nullptr, 0);
        m_context->GSSetShader(m_gs.Get(), nullptr, 0);
        m_context->HSSetShader(m_hs.Get(), nullptr, 0);
        m_context->DSSetShader(m_ds.Get(), nullptr, 0);
        ID3D11Buffer* vs_cb = m_vs_cb.Get();
        ID3D11Buffer* ps_cb = m_ps_cb.Get();
        m_context->VSSetConstantBuffers(0, 1, &vs_cb);
        m_context->PSSetConstantBuffers(0, 1, &ps_cb);

        m_context->IASetInputLayout(m_input_layout.Get());
        m_context->IASetPrimitiveTopology(m_topology);
        ID3D11Buffer* vertex_buffer = m_vertex_buffer.Get();
        m_context->IASetVertexBuffers(0, 1, &vertex_buffer, &m_vertex_stride, &m_vertex_offset);
        m_context->IASetIndexBuffer(m_index_buffer.Get(), m_index_format, m_index_offset);
    }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ID3D11DeviceContext> m_context{};
    ComPtr<ID3D11RenderTargetView> m_rtv{};
    ComPtr<ID3D11DepthStencilView> m_dsv{};
    ComPtr<ID3D11BlendState> m_blend{};
    ComPtr<ID3D11DepthStencilState> m_depth{};
    ComPtr<ID3D11RasterizerState> m_rasterizer{};
    ComPtr<ID3D11VertexShader> m_vs{};
    ComPtr<ID3D11PixelShader> m_ps{};
    ComPtr<ID3D11GeometryShader> m_gs{};
    ComPtr<ID3D11HullShader> m_hs{};
    ComPtr<ID3D11DomainShader> m_ds{};
    ComPtr<ID3D11Buffer> m_vs_cb{};
    ComPtr<ID3D11Buffer> m_ps_cb{};
    ComPtr<ID3D11InputLayout> m_input_layout{};
    ComPtr<ID3D11Buffer> m_vertex_buffer{};
    ComPtr<ID3D11Buffer> m_index_buffer{};
    float m_blend_factor[4]{};
    UINT m_sample_mask{};
    UINT m_stencil_ref{};
    UINT m_viewport_count{};
    UINT m_scissor_count{};
    D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT m_scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_PRIMITIVE_TOPOLOGY m_topology{};
    UINT m_vertex_stride{};
    UINT m_vertex_offset{};
    DXGI_FORMAT m_index_format{DXGI_FORMAT_UNKNOWN};
    UINT m_index_offset{};
};

} // namespace

std::shared_ptr<WindowMode>& WindowMode::get() {
    static auto instance = std::make_shared<WindowMode>();
    return instance;
}

WindowMode::WindowMode() {
    static_assert(sizeof(Constants) % 16 == 0,
        "D3D11 constant buffers must be 16-byte aligned");
    static_assert(sizeof(Constants) / sizeof(float) <= 64,
        "D3D12 root constants must fit the 64-DWORD root signature limit");
    m_options = {*m_enabled, *m_lock_aspect, *m_plane_width, *m_plane_height,
        *m_anchor_distance, *m_feather, *m_corner_radius, *m_curvature,
        *m_surround_red, *m_surround_green, *m_surround_blue, *m_opacity,
        *m_external_bridge_available};
}

WindowMode::~WindowMode() {
    reset_d3d11();
    reset_d3d12();
}

void WindowMode::on_draw_sidebar_entry(std::string_view entry) {
    if (entry != "6DOF Window") {
        return;
    }

    ImGui::TextWrapped("Optional comfort view: places a physical flat or curved aperture in tracking space. The aperture stays where it was anchored while you move or turn your head.");
    ImGui::Spacing();
    if (m_enabled->draw("Enable Room-Anchored 6DOF Window") && m_enabled->value()) {
        request_recenter();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Ctrl+click a slider to type an exact value.");
    m_plane_width->draw("Window X Width (meters)");
    m_lock_aspect->draw("Lock to 16:9 Aspect");
    if (m_lock_aspect->value()) {
        ImGui::TextDisabled("Window Y Height: %.3f m (derived)",
            std::clamp(m_plane_width->value(), 0.1f, 12.0f) * 9.0f / 16.0f);
    } else {
        m_plane_height->draw("Window Y Height (meters)");
    }
    m_anchor_distance->draw("Distance From Recenter Origin (meters)");
    m_feather->draw("Feather Width (meters)");
    m_corner_radius->draw("Corner Radius (meters; capped at half the shorter side)");
    m_curvature->draw("Horizontal Curvature (0 = flat, 1 = around player)");
    float surround_color[]{
        std::clamp(m_surround_red->value(), 0.0f, 1.0f),
        std::clamp(m_surround_green->value(), 0.0f, 1.0f),
        std::clamp(m_surround_blue->value(), 0.0f, 1.0f),
    };
    if (ImGui::ColorEdit3("Surround Color", surround_color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB)) {
        m_surround_red->value() = surround_color[0];
        m_surround_green->value() = surround_color[1];
        m_surround_blue->value() = surround_color[2];
    }
    m_opacity->draw("Surround Opacity");
    ImGui::Spacing();
    if (ImGui::Button("Recenter Window In Front Of Me")) {
        request_recenter();
    }
    {
        std::scoped_lock lock{m_anchor_mutex};
        ImGui::SameLine();
        ImGui::TextDisabled(m_anchor_valid ? "anchored" : "waiting for tracking");
    }
    ImGui::Spacing();
    ImGui::TextWrapped("The game remains native stereo and 6DOF. The aperture is fixed at recenter time. Curvature bends that fixed surface toward a cylinder around the recenter origin; it never follows later head movement.");
}

void WindowMode::on_device_reset() {
    invalidate_anchor();
    reset_d3d11();
    reset_d3d12();
}

void WindowMode::request_recenter() {
    m_recenter_requested = true;
}

void WindowMode::apply_cutscene_comfort_state(std::string_view payload) {
    std::istringstream stream{std::string{payload}};
    int version{};
    int active{};
    int recenter{};
    int lock_aspect{};
    CutsceneComfortState next{};
    if (!(stream >> version >> active >> recenter >> lock_aspect >>
            next.width >> next.height >> next.distance >> next.feather >>
            next.corner_radius >> next.curvature >>
            next.surround_color.x >> next.surround_color.y >> next.surround_color.z >>
            next.opacity) || version != 1) {
        spdlog::warn("[6DOF Window] Ignoring malformed CutsceneComfort bridge state");
        return;
    }

    next.active = active != 0;
    next.lock_aspect = lock_aspect != 0;

    bool entering{};
    {
        std::scoped_lock lock{m_cutscene_comfort_mutex};
        entering = next.active && !m_cutscene_comfort.active;
        m_cutscene_comfort = next;
    }

    if (next.active && (entering || recenter != 0)) {
        request_recenter();
    }
}

bool WindowMode::cutscene_comfort_active() const {
    std::scoped_lock lock{m_cutscene_comfort_mutex};
    return m_cutscene_comfort.active;
}

WindowMode::RenderSettings WindowMode::get_render_settings() const {
    RenderSettings settings{};
    {
        std::scoped_lock lock{m_cutscene_comfort_mutex};
        if (m_cutscene_comfort.active) {
            settings.lock_aspect = m_cutscene_comfort.lock_aspect;
            settings.width = m_cutscene_comfort.width;
            settings.height = m_cutscene_comfort.height;
            settings.distance = m_cutscene_comfort.distance;
            settings.feather = m_cutscene_comfort.feather;
            settings.corner_radius = m_cutscene_comfort.corner_radius;
            settings.curvature = m_cutscene_comfort.curvature;
            settings.surround_color = m_cutscene_comfort.surround_color;
            settings.opacity = m_cutscene_comfort.opacity;
        } else {
            settings.lock_aspect = m_lock_aspect->value();
            settings.width = m_plane_width->value();
            settings.height = m_plane_height->value();
            settings.distance = m_anchor_distance->value();
            settings.feather = m_feather->value();
            settings.corner_radius = m_corner_radius->value();
            settings.curvature = m_curvature->value();
            settings.surround_color = Vector3f{
                m_surround_red->value(), m_surround_green->value(), m_surround_blue->value()};
            settings.opacity = m_opacity->value();
        }
    }

    settings.width = std::clamp(settings.width, 0.1f, 12.0f);
    settings.height = settings.lock_aspect
        ? settings.width * 9.0f / 16.0f
        : std::clamp(settings.height, 0.1f, 8.0f);
    settings.distance = std::clamp(settings.distance, 0.25f, 12.0f);
    settings.feather = std::clamp(settings.feather, 0.0f, 0.5f);
    settings.corner_radius = std::min(
        std::clamp(settings.corner_radius, 0.0f, 2.0f),
        std::min(settings.width, settings.height) * 0.5f);
    settings.curvature = std::clamp(settings.curvature, 0.0f, 1.0f);
    settings.surround_color = glm::clamp(settings.surround_color, Vector3f{0.0f}, Vector3f{1.0f});
    settings.opacity = std::clamp(settings.opacity, 0.0f, 1.0f);
    return settings;
}

WindowMode::Status WindowMode::get_status() const {
    const auto settings = get_render_settings();
    Status status{};
    status.enabled = m_enabled->value() || cutscene_comfort_active();
    status.recenter_pending = m_recenter_requested.load();
    status.lock_aspect = settings.lock_aspect;
    status.width = settings.width;
    status.height = settings.height;
    status.anchor_distance = settings.distance;
    status.feather = settings.feather;
    status.corner_radius = settings.corner_radius;
    status.curvature = settings.curvature;
    status.opacity = settings.opacity;
    status.surround_color = settings.surround_color;

    std::scoped_lock lock{m_anchor_mutex};
    status.anchor_valid = m_anchor_valid;
    status.anchor_origin = m_anchor_origin;
    status.anchor_center = m_anchor_origin - m_anchor_back * status.anchor_distance;
    status.anchor_right = m_anchor_right;
    status.anchor_up = m_anchor_up;
    status.anchor_back = m_anchor_back;
    return status;
}

void WindowMode::invalidate_anchor() {
    std::scoped_lock lock{m_anchor_mutex};
    m_anchor_valid = false;
    m_recenter_requested = true;
}

bool WindowMode::update_enabled_state() {
    const bool enabled = m_enabled->value() || cutscene_comfort_active();
    if (!enabled) {
        if (m_was_enabled.exchange(false)) {
            invalidate_anchor();
        }
        return false;
    }

    if (!m_was_enabled.exchange(true)) {
        request_recenter();
    }
    return true;
}

bool WindowMode::build_constants(bool right_eye, Constants& constants) {
    const auto settings = get_render_settings();
    constants = {};
    constants.eye_origin[3] = 1.0f;
    constants.surround_color[0] = settings.surround_color.x;
    constants.surround_color[1] = settings.surround_color.y;
    constants.surround_color[2] = settings.surround_color.z;
    constants.surround_color[3] = settings.opacity;

    const auto& vr = VR::get();
    if (vr == nullptr || !vr->is_hmd_active()) {
        return false;
    }

    const auto frame_count = static_cast<uint32_t>(std::max(vr->get_frame_count(), 0));
    const auto hmd_transform = vr->get_hmd_transform(frame_count);
    const auto hmd_position = Vector3f{hmd_transform[3]};
    const auto hmd_right = Vector3f{hmd_transform[0]};
    const auto hmd_up = Vector3f{hmd_transform[1]};
    const auto hmd_back = Vector3f{hmd_transform[2]};

    const auto finite_vector = [](const Vector3f& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    };
    if (!finite_vector(hmd_position) || !finite_vector(hmd_right) ||
        !finite_vector(hmd_up) || !finite_vector(hmd_back) ||
        glm::length(hmd_right) < 0.5f || glm::length(hmd_up) < 0.5f ||
        glm::length(hmd_back) < 0.5f) {
        return false;
    }

    Vector3f anchor_origin{};
    Vector3f anchor_right{};
    Vector3f anchor_up{};
    Vector3f anchor_back{};
    {
        std::scoped_lock lock{m_anchor_mutex};
        // Consume the request even on the first valid frame. Leaving it set
        // when m_anchor_valid is false makes the second eye recenter and log
        // the same plane again because of boolean short-circuiting.
        const bool recenter_requested = m_recenter_requested.exchange(false);
        if (!m_anchor_valid || recenter_requested) {
            m_anchor_right = glm::normalize(hmd_right);
            m_anchor_up = glm::normalize(hmd_up);
            m_anchor_back = glm::normalize(hmd_back);
            m_anchor_origin = hmd_position;
            m_anchor_valid = true;
            const auto center = m_anchor_origin - m_anchor_back *
                settings.distance;
            spdlog::info(
                "[6DOF Window] Anchored room-space aperture origin ({:.3f}, {:.3f}, {:.3f}), center ({:.3f}, {:.3f}, {:.3f})",
                m_anchor_origin.x, m_anchor_origin.y, m_anchor_origin.z,
                center.x, center.y, center.z);
        }

        anchor_origin = m_anchor_origin;
        anchor_right = m_anchor_right;
        anchor_up = m_anchor_up;
        anchor_back = m_anchor_back;
    }

    const float width = settings.width;
    const float height = settings.height;

    const auto eye = right_eye ? VRRuntime::Eye::RIGHT : VRRuntime::Eye::LEFT;
    const auto eye_world = hmd_transform * vr->get_eye_transform(static_cast<uint32_t>(eye));
    const auto eye_origin = Vector3f{eye_world[3]};
    const Matrix4x4f tracking_to_projection{
        1.0f, 0.0f,  0.0f, 0.0f,
        0.0f, 1.0f,  0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        0.0f, 0.0f,  0.0f, 1.0f,
    };
    const auto view = tracking_to_projection * glm::inverse(eye_world);
    const auto projection = vr->get_projection_matrix(eye);
    const auto clip_to_tracking = glm::inverse(projection * view);

    const auto make_ray = [&](float x, float y, Vector3f& ray) {
        const auto tracking = clip_to_tracking * glm::vec4{x, y, 1.0f, 1.0f};
        if (!std::isfinite(tracking.x) || !std::isfinite(tracking.y) ||
            !std::isfinite(tracking.z) || !std::isfinite(tracking.w) ||
            std::abs(tracking.w) <= 0.000001f) {
            return false;
        }
        ray = Vector3f{tracking} / tracking.w - eye_origin;
        return finite_vector(ray) && glm::length(ray) > 0.000001f;
    };

    Vector3f ray_center{};
    Vector3f ray_at_x{};
    Vector3f ray_at_y{};
    if (!make_ray(0.0f, 0.0f, ray_center) ||
        !make_ray(1.0f, 0.0f, ray_at_x) ||
        !make_ray(0.0f, 1.0f, ray_at_y)) {
        return true;
    }

    const auto set_vector = [](float (&destination)[4], const Vector3f& value, float scalar) {
        destination[0] = value.x;
        destination[1] = value.y;
        destination[2] = value.z;
        destination[3] = scalar;
    };
    set_vector(constants.eye_origin, eye_origin, 0.0f);
    set_vector(constants.ray_center, ray_center,
        settings.curvature);
    set_vector(constants.ray_x, ray_at_x - ray_center,
        settings.feather);
    set_vector(constants.ray_y, ray_at_y - ray_center,
        settings.corner_radius);
    set_vector(constants.anchor_origin, anchor_origin,
        settings.distance);
    set_vector(constants.anchor_right, anchor_right, width * 0.5f);
    set_vector(constants.anchor_up, anchor_up, height * 0.5f);
    set_vector(constants.anchor_back, anchor_back, 0.0f);
    return true;
}

bool WindowMode::ensure_d3d11_objects(ID3D11Device* device) {
    if (device == nullptr) {
        return false;
    }

    if (m_d3d11_device.Get() == device && m_d3d11_vs != nullptr) {
        return true;
    }

    reset_d3d11();
    m_d3d11_device = device;

    ComPtr<ID3DBlob> vs_blob{};
    ComPtr<ID3DBlob> ps_blob{};
    ComPtr<ID3DBlob> errors{};
    if (FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, &errors))) {
        spdlog::error("[6DOF Window] Failed to compile D3D11 vertex shader: {}",
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return false;
    }
    errors.Reset();
    if (FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps_blob, &errors))) {
        spdlog::error("[6DOF Window] Failed to compile D3D11 pixel shader: {}",
            errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
        return false;
    }

    if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &m_d3d11_vs)) ||
        FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &m_d3d11_ps))) {
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC input_desc[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(input_desc, static_cast<UINT>(std::size(input_desc)),
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &m_d3d11_layout))) {
        return false;
    }

    D3D11_BUFFER_DESC vertex_desc{};
    vertex_desc.ByteWidth = sizeof(k_vertices);
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertex_data{};
    vertex_data.pSysMem = k_vertices;
    if (FAILED(device->CreateBuffer(&vertex_desc, &vertex_data, &m_d3d11_vertices))) {
        return false;
    }

    D3D11_BUFFER_DESC constants_desc{};
    constants_desc.ByteWidth = sizeof(Constants);
    constants_desc.Usage = D3D11_USAGE_DYNAMIC;
    constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constants_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&constants_desc, nullptr, &m_d3d11_constants))) {
        return false;
    }

    D3D11_BLEND_DESC blend_desc{};
    auto& blend = blend_desc.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D11_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend_desc, &m_d3d11_blend))) {
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D11_FILL_SOLID;
    rasterizer_desc.CullMode = D3D11_CULL_NONE;
    rasterizer_desc.DepthClipEnable = FALSE;
    rasterizer_desc.ScissorEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rasterizer_desc, &m_d3d11_rasterizer))) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = FALSE;
    depth_desc.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &m_d3d11_depth))) {
        return false;
    }

    return true;
}

ID3D11RenderTargetView* WindowMode::get_d3d11_rtv(ID3D11Device* device, ID3D11Texture2D* target) {
    if (auto it = m_d3d11_rtvs.find(target); it != m_d3d11_rtvs.end()) {
        return it->second.rtv.Get();
    }

    // OpenXR normally rotates through three images. This cap also prevents a
    // runtime that recreates swapchains repeatedly from retaining old images
    // for the rest of the process lifetime.
    if (m_d3d11_rtvs.size() >= 16) {
        m_d3d11_rtvs.clear();
    }

    D3D11_TEXTURE2D_DESC target_desc{};
    target->GetDesc(&target_desc);
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = resolve_typeless(target_desc.Format);
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    CachedD3D11RTV cached{};
    cached.texture = target;
    if (FAILED(device->CreateRenderTargetView(target, &rtv_desc, &cached.rtv))) {
        return nullptr;
    }

    auto [it, inserted] = m_d3d11_rtvs.emplace(target, std::move(cached));
    return inserted ? it->second.rtv.Get() : nullptr;
}

bool WindowMode::draw_d3d11(ID3D11DeviceContext* context, ID3D11Texture2D* target,
    ID3D11RenderTargetView* rtv, Layout layout) {
    const auto settings = get_render_settings();
    if (!update_enabled_state() || context == nullptr || target == nullptr ||
        settings.opacity <= 0.001f) {
        return false;
    }

    ComPtr<ID3D11Device> device{};
    context->GetDevice(&device);
    if (!ensure_d3d11_objects(device.Get())) {
        return false;
    }

    if (rtv == nullptr) {
        rtv = get_d3d11_rtv(device.Get(), target);
    }
    if (rtv == nullptr) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) {
        return false;
    }

    D3D11StateBackup backup{context};

    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->OMSetBlendState(m_d3d11_blend.Get(), nullptr, UINT_MAX);
    context->OMSetDepthStencilState(m_d3d11_depth.Get(), 0);
    context->RSSetState(m_d3d11_rasterizer.Get());
    context->VSSetShader(m_d3d11_vs.Get(), nullptr, 0);
    context->PSSetShader(m_d3d11_ps.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    context->IASetInputLayout(m_d3d11_layout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(float) * 2;
    UINT offset = 0;
    ID3D11Buffer* vertices = m_d3d11_vertices.Get();
    context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);

    const bool double_wide = layout == Layout::DOUBLE_WIDE;
    const UINT eye_count = double_wide ? 2u : 1u;
    const float eye_width = double_wide ? static_cast<float>(desc.Width) * 0.5f : static_cast<float>(desc.Width);

    for (UINT eye = 0; eye < eye_count; ++eye) {
        const bool right_eye = double_wide ? eye == 1 : layout == Layout::RIGHT_EYE;
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = double_wide && right_eye ? eye_width : 0.0f;
        viewport.Width = eye_width;
        viewport.Height = static_cast<float>(desc.Height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        D3D11_RECT scissor{};
        scissor.left = static_cast<LONG>(viewport.TopLeftX);
        scissor.top = 0;
        scissor.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
        scissor.bottom = static_cast<LONG>(desc.Height);
        context->RSSetScissorRects(1, &scissor);

        Constants constants{};
        if (!build_constants(right_eye, constants)) {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(m_d3d11_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(m_d3d11_constants.Get(), 0);

        ID3D11Buffer* constant_buffer = m_d3d11_constants.Get();
        context->PSSetConstantBuffers(0, 1, &constant_buffer);
        context->Draw(6, 0);
    }

    return true;
}

bool WindowMode::ensure_d3d12_objects(ID3D12Device* device, DXGI_FORMAT format) {
    if (device == nullptr) {
        return false;
    }

    if (m_d3d12_device.Get() == device && m_d3d12_pso != nullptr && m_d3d12_format == format) {
        return true;
    }

    if (m_d3d12_device.Get() != device) {
        reset_d3d12();
        m_d3d12_device = device;
    }

    if (m_d3d12_root_signature == nullptr) {
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.ShaderRegister = 0;
        parameter.Constants.Num32BitValues = sizeof(Constants) / sizeof(float);
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC root_desc{};
        root_desc.NumParameters = 1;
        root_desc.pParameters = &parameter;
        root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized{};
        ComPtr<ID3DBlob> errors{};
        if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&m_d3d12_root_signature)))) {
            spdlog::error("[6DOF Window] Failed to create D3D12 root signature");
            return false;
        }
    }

    ComPtr<ID3DBlob> vs_blob{};
    ComPtr<ID3DBlob> ps_blob{};
    if (FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vs_blob, nullptr)) ||
        FAILED(D3DCompile(k_shader, sizeof(k_shader) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &ps_blob, nullptr))) {
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC input_desc[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = m_d3d12_root_signature.Get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
    pso_desc.InputLayout = {input_desc, static_cast<UINT>(std::size(input_desc))};
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = format;
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = FALSE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    auto& blend = pso_desc.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    ComPtr<ID3D12PipelineState> pso{};
    if (FAILED(device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso)))) {
        return false;
    }
    m_d3d12_pso = pso;
    m_d3d12_format = format;

    if (m_d3d12_vertices == nullptr) {
        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = sizeof(k_vertices);
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_d3d12_vertices)))) {
            return false;
        }

        void* mapped = nullptr;
        D3D12_RANGE no_read{0, 0};
        if (FAILED(m_d3d12_vertices->Map(0, &no_read, &mapped))) {
            return false;
        }
        std::memcpy(mapped, k_vertices, sizeof(k_vertices));
        m_d3d12_vertices->Unmap(0, nullptr);

        m_d3d12_vbv.BufferLocation = m_d3d12_vertices->GetGPUVirtualAddress();
        m_d3d12_vbv.SizeInBytes = sizeof(k_vertices);
        m_d3d12_vbv.StrideInBytes = sizeof(float) * 2;
    }

    return true;
}

bool WindowMode::draw_d3d12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* target,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, Layout layout, D3D12_RESOURCE_STATES target_state) {
    const auto settings = get_render_settings();
    if (!update_enabled_state() || command_list == nullptr || target == nullptr ||
        rtv.ptr == 0 || settings.opacity <= 0.001f) {
        return false;
    }

    ComPtr<ID3D12Device> device{};
    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device)))) {
        return false;
    }

    const auto desc = target->GetDesc();
    if (desc.Width == 0 || desc.Height == 0 ||
        !ensure_d3d12_objects(device.Get(), resolve_typeless(desc.Format))) {
        return false;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    const bool needs_transition = target_state != D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (needs_transition) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = target_state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command_list->ResourceBarrier(1, &barrier);
    }

    command_list->SetPipelineState(m_d3d12_pso.Get());
    command_list->SetGraphicsRootSignature(m_d3d12_root_signature.Get());
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->IASetVertexBuffers(0, 1, &m_d3d12_vbv);
    command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const bool double_wide = layout == Layout::DOUBLE_WIDE;
    const UINT eye_count = double_wide ? 2u : 1u;
    const float eye_width = double_wide ? static_cast<float>(desc.Width) * 0.5f : static_cast<float>(desc.Width);

    for (UINT eye = 0; eye < eye_count; ++eye) {
        const bool right_eye = double_wide ? eye == 1 : layout == Layout::RIGHT_EYE;
        D3D12_VIEWPORT viewport{};
        viewport.TopLeftX = double_wide && right_eye ? eye_width : 0.0f;
        viewport.Width = eye_width;
        viewport.Height = static_cast<float>(desc.Height);
        viewport.MaxDepth = 1.0f;
        command_list->RSSetViewports(1, &viewport);

        D3D12_RECT scissor{};
        scissor.left = static_cast<LONG>(viewport.TopLeftX);
        scissor.top = 0;
        scissor.right = static_cast<LONG>(viewport.TopLeftX + viewport.Width);
        scissor.bottom = static_cast<LONG>(desc.Height);
        command_list->RSSetScissorRects(1, &scissor);

        Constants constants{};
        if (!build_constants(right_eye, constants)) {
            if (needs_transition) {
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = target_state;
                command_list->ResourceBarrier(1, &barrier);
            }
            return false;
        }
        command_list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / sizeof(float), &constants, 0);
        command_list->DrawInstanced(6, 1, 0, 0);
    }

    if (needs_transition) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = target_state;
        command_list->ResourceBarrier(1, &barrier);
    }

    return true;
}

void WindowMode::reset_d3d11() {
    m_d3d11_rtvs.clear();
    m_d3d11_depth.Reset();
    m_d3d11_rasterizer.Reset();
    m_d3d11_blend.Reset();
    m_d3d11_constants.Reset();
    m_d3d11_vertices.Reset();
    m_d3d11_layout.Reset();
    m_d3d11_ps.Reset();
    m_d3d11_vs.Reset();
    m_d3d11_device.Reset();
}

void WindowMode::reset_d3d12() {
    m_d3d12_vertices.Reset();
    m_d3d12_pso.Reset();
    m_d3d12_root_signature.Reset();
    m_d3d12_device.Reset();
    m_d3d12_vbv = {};
    m_d3d12_format = DXGI_FORMAT_UNKNOWN;
}
