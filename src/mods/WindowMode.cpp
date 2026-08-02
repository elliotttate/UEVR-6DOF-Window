#include "WindowMode.hpp"

#include <algorithm>
#include <cstring>

#include <d3dcompiler.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#pragma comment(lib, "d3dcompiler")

namespace {

constexpr char k_shader[] = R"(
cbuffer Constants : register(b0) {
    float2 center;
    float2 half_extents;
    float feather;
    float alpha;
    float2 padding;
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
    float2 d = abs(input.ndc - center) - half_extents;
    float dist = length(max(d, 0.0f)) + min(max(d.x, d.y), 0.0f);
    float mask = smoothstep(0.0f, max(feather, 0.0001f), dist);
    return float4(0.0f, 0.0f, 0.0f, mask * alpha);
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
    m_options = {*m_enabled, *m_width, *m_height, *m_feather, *m_depth, *m_opacity};
}

WindowMode::~WindowMode() {
    reset_d3d11();
    reset_d3d12();
}

void WindowMode::on_draw_sidebar_entry(std::string_view entry) {
    if (entry != "6DOF Window") {
        return;
    }

    ImGui::TextWrapped("Optional comfort view: masks the outer part of each submitted eye image while leaving UEVR's stereo rendering and 6DOF head tracking untouched inside the window.");
    ImGui::Spacing();
    m_enabled->draw("Enable 6DOF Window Mode");
    ImGui::Separator();
    m_width->draw("Window Width");
    m_height->draw("Window Height");
    m_feather->draw("Soft Edge");
    m_depth->draw("Window Stereo Depth");
    m_opacity->draw("Outside Darkness");
    ImGui::Spacing();
    ImGui::TextWrapped("The world is not flattened or reprojected. You can still lean and see normal stereo parallax; this mode only darkens pixels outside the aperture.");
}

void WindowMode::on_device_reset() {
    reset_d3d11();
    reset_d3d12();
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
    if (!m_enabled->value() || context == nullptr || target == nullptr || m_opacity->value() <= 0.001f) {
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
        constants.center_x = right_eye ? -m_depth->value() : m_depth->value();
        constants.half_x = std::clamp(m_width->value(), 0.05f, 0.98f);
        constants.half_y = std::clamp(m_height->value(), 0.05f, 0.98f);
        constants.feather = std::max(m_feather->value(), 0.001f);
        constants.alpha = std::clamp(m_opacity->value(), 0.0f, 1.0f);

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
    if (!m_enabled->value() || command_list == nullptr || target == nullptr || rtv.ptr == 0 || m_opacity->value() <= 0.001f) {
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
        constants.center_x = right_eye ? -m_depth->value() : m_depth->value();
        constants.half_x = std::clamp(m_width->value(), 0.05f, 0.98f);
        constants.half_y = std::clamp(m_height->value(), 0.05f, 0.98f);
        constants.feather = std::max(m_feather->value(), 0.001f);
        constants.alpha = std::clamp(m_opacity->value(), 0.0f, 1.0f);
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
