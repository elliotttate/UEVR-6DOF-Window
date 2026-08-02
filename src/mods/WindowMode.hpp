#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "Mod.hpp"

// A comfort mode that keeps UEVR's normal stereo scene and positional
// tracking, then masks the outside of each submitted eye image. The visible
// aperture therefore behaves like a window onto the still-3D game rather
// than turning the game into a flat cinema texture.
class WindowMode final : public Mod {
public:
    enum class Layout {
        LEFT_EYE,
        RIGHT_EYE,
        DOUBLE_WIDE,
    };

    static std::shared_ptr<WindowMode>& get();

    WindowMode();
    ~WindowMode() override;

    std::string_view get_name() const override { return "WindowMode"; }
    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        return {{"6DOF Window", false}};
    }

    void on_draw_sidebar_entry(std::string_view entry) override;
    void on_device_reset() override;

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
        float center_x;
        float center_y;
        float half_x;
        float half_y;
        float feather;
        float alpha;
        float padding[2];
    };

    struct CachedD3D11RTV {
        ComPtr<ID3D11Texture2D> texture{};
        ComPtr<ID3D11RenderTargetView> rtv{};
    };

    bool ensure_d3d11_objects(ID3D11Device* device);
    ID3D11RenderTargetView* get_d3d11_rtv(ID3D11Device* device, ID3D11Texture2D* target);
    bool ensure_d3d12_objects(ID3D12Device* device, DXGI_FORMAT format);

    void reset_d3d11();
    void reset_d3d12();

    const ModToggle::Ptr m_enabled{ModToggle::create(generate_name("Enabled"), false)};
    const ModSlider::Ptr m_width{ModSlider::create(generate_name("Width"), 0.05f, 0.98f, 0.62f)};
    const ModSlider::Ptr m_height{ModSlider::create(generate_name("Height"), 0.05f, 0.98f, 0.56f)};
    const ModSlider::Ptr m_feather{ModSlider::create(generate_name("Feather"), 0.001f, 0.50f, 0.22f)};
    const ModSlider::Ptr m_depth{ModSlider::create(generate_name("Depth"), 0.0f, 0.08f, 0.012f)};
    const ModSlider::Ptr m_opacity{ModSlider::create(generate_name("Opacity"), 0.0f, 1.0f, 1.0f)};

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
