#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "Mod.hpp"

// A comfort mode that keeps UEVR's normal stereo scene and positional
// tracking, then masks the outside of a rectangle anchored in tracking space.
// The same physical rectangle is projected into both eyes each frame, so it
// stays in the room and can be viewed obliquely instead of following the HMD.
class WindowMode final : public Mod {
public:
    enum class Layout {
        LEFT_EYE,
        RIGHT_EYE,
        DOUBLE_WIDE,
    };

    static std::shared_ptr<WindowMode>& get();

    struct Status {
        bool enabled{};
        bool anchor_valid{};
        bool recenter_pending{};
        bool lock_aspect{};
        float width{};
        float height{};
        float anchor_distance{};
        float feather{};
        float corner_radius{};
        float curvature{};
        float opacity{};
        Vector3f surround_color{};
        Vector3f anchor_origin{};
        Vector3f anchor_center{};
        Vector3f anchor_right{1.0f, 0.0f, 0.0f};
        Vector3f anchor_up{0.0f, 1.0f, 0.0f};
        Vector3f anchor_back{0.0f, 0.0f, 1.0f};
    };

    WindowMode();
    ~WindowMode() override;

    std::string_view get_name() const override { return "WindowMode"; }
    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        return {{"6DOF Window", false}};
    }

    void on_draw_sidebar_entry(std::string_view entry) override;
    void on_device_reset() override;
    void request_recenter();
    void apply_cutscene_comfort_state(std::string_view payload);
    Status get_status() const;

    // These are called on UEVR's final color targets, after the stereo scene
    // has been copied but before the eye images are submitted. UI and depth
    // swapchains never pass through these methods.
    bool draw_d3d11(ID3D11DeviceContext* context, ID3D11Texture2D* target,
        ID3D11RenderTargetView* rtv, Layout layout);
    bool draw_d3d12(ID3D12GraphicsCommandList* command_list, ID3D12Resource* target,
        D3D12_CPU_DESCRIPTOR_HANDLE rtv, Layout layout,
        D3D12_RESOURCE_STATES target_state = D3D12_RESOURCE_STATE_RENDER_TARGET);

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Constants {
        float eye_origin[4];
        float ray_center[4];
        float ray_x[4];
        float ray_y[4];
        float anchor_origin[4];
        float anchor_right[4];
        float anchor_up[4];
        float anchor_back[4];
        float surround_color[4];
    };

    struct CachedD3D11RTV {
        ComPtr<ID3D11Texture2D> texture{};
        ComPtr<ID3D11RenderTargetView> rtv{};
    };

    struct RenderSettings {
        bool lock_aspect{};
        float width{2.4f};
        float height{1.35f};
        float distance{2.0f};
        float feather{0.10f};
        float corner_radius{};
        float curvature{};
        Vector3f surround_color{};
        float opacity{1.0f};
    };

    struct CutsceneComfortState {
        bool active{};
        bool lock_aspect{};
        float width{2.4f};
        float height{1.35f};
        float distance{2.0f};
        float feather{0.10f};
        float corner_radius{};
        float curvature{};
        Vector3f surround_color{};
        float opacity{};
    };

    bool ensure_d3d11_objects(ID3D11Device* device);
    ID3D11RenderTargetView* get_d3d11_rtv(ID3D11Device* device, ID3D11Texture2D* target);
    bool ensure_d3d12_objects(ID3D12Device* device, DXGI_FORMAT format);
    bool build_constants(bool right_eye, Constants& constants);
    bool update_enabled_state();
    bool cutscene_comfort_active() const;
    RenderSettings get_render_settings() const;
    void invalidate_anchor();

    void reset_d3d11();
    void reset_d3d12();

    const ModToggle::Ptr m_enabled{ModToggle::create(generate_name("Enabled"), false)};
    const ModToggle::Ptr m_lock_aspect{ModToggle::create(generate_name("LockAspect"), false)};
    const ModSlider::Ptr m_plane_width{ModSlider::create(generate_name("PlaneWidth"), 0.1f, 12.0f, 2.4f)};
    const ModSlider::Ptr m_plane_height{ModSlider::create(generate_name("PlaneHeight"), 0.1f, 8.0f, 1.35f)};
    const ModSlider::Ptr m_anchor_distance{ModSlider::create(generate_name("AnchorDistance"), 0.25f, 12.0f, 2.0f)};
    const ModSlider::Ptr m_feather{ModSlider::create(generate_name("Feather"), 0.0f, 0.5f, 0.10f)};
    const ModSlider::Ptr m_corner_radius{ModSlider::create(generate_name("CornerRadius"), 0.0f, 2.0f, 0.0f)};
    const ModSlider::Ptr m_curvature{ModSlider::create(generate_name("Curvature"), 0.0f, 1.0f, 0.0f)};
    const ModSlider::Ptr m_surround_red{ModSlider::create(generate_name("SurroundRed"), 0.0f, 1.0f, 0.0f)};
    const ModSlider::Ptr m_surround_green{ModSlider::create(generate_name("SurroundGreen"), 0.0f, 1.0f, 0.0f)};
    const ModSlider::Ptr m_surround_blue{ModSlider::create(generate_name("SurroundBlue"), 0.0f, 1.0f, 0.0f)};
    const ModSlider::Ptr m_opacity{ModSlider::create(generate_name("Opacity"), 0.0f, 1.0f, 1.0f)};
    // Capability marker read by the standalone CutsceneComfort plugin. The
    // actual override is transient and is never written into the user's
    // normal WindowMode configuration.
    const ModToggle::Ptr m_external_bridge_available{ModToggle::create(generate_name("ExternalBridgeAvailable"), true)};

    mutable std::mutex m_cutscene_comfort_mutex{};
    CutsceneComfortState m_cutscene_comfort{};

    mutable std::mutex m_anchor_mutex{};
    bool m_anchor_valid{false};
    Vector3f m_anchor_origin{};
    Vector3f m_anchor_right{1.0f, 0.0f, 0.0f};
    Vector3f m_anchor_up{0.0f, 1.0f, 0.0f};
    Vector3f m_anchor_back{0.0f, 0.0f, 1.0f};
    std::atomic_bool m_recenter_requested{true};
    std::atomic_bool m_was_enabled{false};

    ComPtr<ID3D11Device> m_d3d11_device{};
    ComPtr<ID3D11VertexShader> m_d3d11_vs{};
    ComPtr<ID3D11PixelShader> m_d3d11_ps{};
    ComPtr<ID3D11InputLayout> m_d3d11_layout{};
    ComPtr<ID3D11Buffer> m_d3d11_vertices{};
    ComPtr<ID3D11Buffer> m_d3d11_constants{};
    ComPtr<ID3D11BlendState> m_d3d11_blend{};
    ComPtr<ID3D11RasterizerState> m_d3d11_rasterizer{};
    ComPtr<ID3D11DepthStencilState> m_d3d11_depth{};
    std::unordered_map<ID3D11Texture2D*, CachedD3D11RTV> m_d3d11_rtvs{};

    ComPtr<ID3D12Device> m_d3d12_device{};
    ComPtr<ID3D12RootSignature> m_d3d12_root_signature{};
    ComPtr<ID3D12PipelineState> m_d3d12_pso{};
    ComPtr<ID3D12Resource> m_d3d12_vertices{};
    D3D12_VERTEX_BUFFER_VIEW m_d3d12_vbv{};
    DXGI_FORMAT m_d3d12_format{DXGI_FORMAT_UNKNOWN};
};
