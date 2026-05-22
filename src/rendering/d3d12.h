#pragma once

#include "openxr.h"
#include "texture.h"
#include "utils/render_utils.h"
#include "utils/debug_draw.h"

class RND_Renderer;

class RND_D3D12 {
    friend class RND_Renderer;

public:
    RND_D3D12();
    ~RND_D3D12();

    ID3D12Device* GetDevice() { return m_device.Get(); };
    ID3D12CommandQueue* GetCommandQueue() { return m_queue.Get(); };

    void StartFrame();
    void EndFrame();

    // todo: extract most to a base pipeline class if other pipelines are needed
    template <bool depth>
    class PresentPipeline {
        friend class Texture;

    public:
        explicit PresentPipeline(RND_Renderer* pRenderer);
        ~PresentPipeline() = default;

        void BindAttachment(uint32_t attachmentIdx, ID3D12Resource* srcTexture, DXGI_FORMAT overwriteFormat = DXGI_FORMAT_UNKNOWN);
        void BindTarget(uint32_t targetIdx, ID3D12Resource* dstTexture, DXGI_FORMAT overwriteFormat = DXGI_FORMAT_UNKNOWN);
        void BindDepthTarget(ID3D12Resource* dstTexture, DXGI_FORMAT overwriteFormat);
        void BindSettings(float screenWidth, float screenHeight, const RenderUtils::UvTransform& uvTransform = {});
        void SetUvTransform(const RenderUtils::UvTransform& uvTransform) { m_uvTransform = uvTransform; }
        void Render(ID3D12GraphicsCommandList* commandList, ID3D12Resource* swapchain);

    private:
        void UpdateSettingsBuffer(ID3D12Resource* swapchain);

        RND_Renderer* m_renderer = nullptr;
        float m_renderWidth = 0.0f;
        float m_renderHeight = 0.0f;
        RenderUtils::UvTransform m_uvTransform = {};

        void RecreatePipeline();

        ComPtr<ID3DBlob> m_vertexShader;
        ComPtr<ID3DBlob> m_pixelShader;

        ComPtr<ID3D12Resource> m_screenIndicesBuffer;
        D3D12_INDEX_BUFFER_VIEW m_screenIndicesView = {};

        ComPtr<ID3D12Resource> m_settingsBuffer;

        ComPtr<ID3D12RootSignature> m_signature;
        ComPtr<ID3D12PipelineState> m_pipelineState;

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, depth ? 3 : 1> m_attachmentHandles = {};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 1> m_targetHandles = {};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, depth ? 1 : 0> m_depthTargetHandles = {};
        ComPtr<ID3D12DescriptorHeap> m_attachmentHeap;
        ComPtr<ID3D12DescriptorHeap> m_targetHeap;
        ComPtr<ID3D12DescriptorHeap> m_depthHeap;
        std::array<DXGI_FORMAT, 2> m_targetFormats = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT };
    };

    template <bool blockTillExecuted>
    class CommandContext {
    public:
        template <typename F>
        CommandContext(ID3D12Device* d3d12Device, ID3D12CommandQueue* d3d12Queue, ID3D12CommandAllocator* d3d12Allocator, F&& recordCallback): m_device(d3d12Device), m_queue(d3d12Queue) {
            // Create commands to upload buffers
            checkHResult(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d12Allocator, nullptr, IID_PPV_ARGS(&this->m_cmdList)), "Failed to create D3D12_CommandContext's command list!");

            recordCallback(this);
        }

        ~CommandContext() {
            // Close command list and then execute command list in queue
            checkHResult(this->m_cmdList->Close(), "Failed to close D3D12_CommandContext's queue");
            ID3D12CommandList* collectedList[] = { this->m_cmdList.Get() };

            for (auto& [texture, value] : this->m_waitFor)
                texture->d3d12WaitForFence(value);
            m_queue->ExecuteCommandLists((UINT)std::size(collectedList), collectedList);
            for (auto& [texture, value] : this->m_signalTo)
                texture->d3d12SignalFence(value);

            // If enabled, wait until the command list and the fence signal has been executed
            if constexpr (blockTillExecuted) {
                m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&this->m_blockFence));
                m_queue->Signal(this->m_blockFence.Get(), 1);

                HANDLE waitEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
                checkAssert(waitEvent != NULL, "Failed to create upload event!");

                if (this->m_blockFence->GetCompletedValue() < 1) {
                    this->m_blockFence->SetEventOnCompletion(1, waitEvent);
                    WaitForSingleObject(waitEvent, INFINITE);
                }
                CloseHandle(waitEvent);
            }
        }

        ID3D12GraphicsCommandList* GetRecordList() { return this->m_cmdList.Get(); }
        void WaitFor(Texture* texture, uint64_t value) { this->m_waitFor.push_back({ texture, value }); }
        void Signal(Texture* texture, uint64_t value) { this->m_signalTo.push_back({ texture, value }); }

    private:
        ID3D12Device* m_device;
        ID3D12CommandQueue* m_queue;

        ComPtr<ID3D12GraphicsCommandList> m_cmdList;
        ComPtr<ID3D12Fence> m_blockFence;
        std::vector<std::pair<Texture*, uint64_t>> m_waitFor;
        std::vector<std::pair<Texture*, uint64_t>> m_signalTo;
    };

    class DebugDrawPipeline {
    public:
        DebugDrawPipeline();
        ~DebugDrawPipeline() = default;

        void Render(OpenXR::EyeSide side, ID3D12GraphicsCommandList* commandList, ID3D12Resource* sceneDepthTexture, ID3D12Resource* colorTarget, DXGI_FORMAT colorFormat, ID3D12Resource* depthTarget, DXGI_FORMAT depthFormat, const DebugDrawRenderData& renderData, const glm::mat4& viewProjection);

    private:
        void RecreatePipeline();
        void EnsureVertexBuffer(uint32_t requiredBytes);
        void UpdateSceneSettings(OpenXR::EyeSide side, uint32_t settingsIndex, ID3D12Resource* colorTarget, const glm::mat4& viewProjection, float xrayAlphaScale);
        void RenderVertices(ID3D12GraphicsCommandList* commandList, D3D12_PRIMITIVE_TOPOLOGY topology, ID3D12PipelineState* pipelineState, uint32_t vertexOffset, uint32_t vertexCount);

        ComPtr<ID3DBlob> m_vertexShader;
        ComPtr<ID3DBlob> m_pixelShader;
        ComPtr<ID3D12RootSignature> m_signature;
        ComPtr<ID3D12PipelineState> m_trianglePipelineState;
        ComPtr<ID3D12PipelineState> m_linePipelineState;
        ComPtr<ID3D12PipelineState> m_xrayTrianglePipelineState;
        ComPtr<ID3D12PipelineState> m_xrayLinePipelineState;

        std::array<std::array<ComPtr<ID3D12Resource>, 2>, 2> m_sceneSettingsBuffers;
        ComPtr<ID3D12Resource> m_vertexBuffer;
        D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
        uint32_t m_vertexBufferCapacity = 0;

        ComPtr<ID3D12DescriptorHeap> m_sceneDepthHeap;
        ComPtr<ID3D12DescriptorHeap> m_targetHeap;
        ComPtr<ID3D12DescriptorHeap> m_depthHeap;
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> m_sceneDepthHandles = {};
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> m_sceneDepthGpuHandles = {};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> m_targetHandles = {};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> m_depthHandles = {};
        std::array<DXGI_FORMAT, 2> m_targetFormats = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN };
    };

private:
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12Fence> m_fence;
};
