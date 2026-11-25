// conker_leia_sr.dll: the Leia SR (Simulated Reality) weaver binding for the Leia SR stereo
// mode. Built with MSVC against Leia's SR SDK through libs/SR-lib (bo3b's wrapper), which
// probes the SR runtime before touching it and turns the SDK's exceptions into HRESULTs. The
// SR DLLs are delay-loaded from this module only, so the game executable has no SR imports
// and a machine without the SR Platform just gets a failed create(): RT64 then presents the
// plain side-by-side pair.
//
// RT64 loads this file at run time and talks to it through the C ABI in
// tools/rt64/src/render/rt64_leiasr_abi.h (version 1: keep the struct identical). Per weave
// RT64 has already composed the pair (both eyes at panel resolution, side by side) and
// submitted that work on its present queue; this module records the weave on its own command
// lists (a small ring, fenced) and submits them on the same queue, so they run after the
// compose and before RT64's next submission, which copies the woven image to the swap chain.
// The SDK never records on RT64's own command lists, so the renderer's cached state is never
// changed behind its back.
//
// The call pattern is the one verified on SR panels in the other ports (vf2_leia_sr.dll,
// 2026-10-05): the weaver created before the context is initialized (SR-lib does both), the
// input's size and format read from its resource description every frame, the output format
// set only when it changes, the viewport and scissor set on the command list itself (the
// weaver's own viewport does not drive the rasterizer), sRGB conversion off (both sides
// UNORM, RT64's compose already encoded the game's gamma), one frame of latency and late
// latching (SR-lib's defaults).
//
// Diagnostics: CONKER_LEIA_SIMULATE=1 makes create() succeed without the SR runtime and the
// weave a plain copy (the left half of the left eye beside the right half of the right eye),
// to check RT64's side of the path (the pair's composition, the submission order, the copy
// to the swap chain) on a machine without an SR display.

#include "render/rt64_leiasr_abi.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "SR.hpp"

namespace {
    using Microsoft::WRL::ComPtr;

    // An allocator may only be reset once its list has run: a ring of lists, each slot's last
    // submission fenced.
    constexpr int Ring = 3;

    struct LeiaImpl {
        SimulatedReality::SRInterfaceDX12 *sr = nullptr;
        ID3D12Device *device = nullptr;
        ID3D12CommandQueue *queue = nullptr;
        ComPtr<ID3D12CommandAllocator> allocators[Ring];
        ComPtr<ID3D12GraphicsCommandList> lists[Ring];
        ComPtr<ID3D12Fence> fence;
        HANDLE fenceEvent = nullptr;
        UINT64 slotFenceValue[Ring] = {};
        UINT64 fenceValue = 0;
        unsigned ringIndex = 0;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        DXGI_FORMAT lastOutputFormat = DXGI_FORMAT_UNKNOWN;
        bool loggedFirstWeave = false;
        bool simulate = false;

        bool waitForValue(UINT64 value) {
            if ((value == 0) || (fence->GetCompletedValue() >= value)) {
                return true;
            }

            ResetEvent(fenceEvent);
            if (FAILED(fence->SetEventOnCompletion(value, fenceEvent))) {
                return false;
            }

            return WaitForSingleObject(fenceEvent, 2000) == WAIT_OBJECT_0;
        }

        ~LeiaImpl() {
            // The device and the queue are RT64's: not released here. This module's lists may
            // still be in flight, so they are waited for before going away. The SDK can throw
            // on a device lost under it; nothing escapes the C ABI.
            if (fence && fenceEvent) {
                waitForValue(fenceValue);
            }

            if (fenceEvent != nullptr) {
                CloseHandle(fenceEvent);
                fenceEvent = nullptr;
            }

            for (int i = 0; i < Ring; i++) {
                lists[i].Reset();
                allocators[i].Reset();
            }

            fence.Reset();
            rtvHeap.Reset();

            // Delete() destroys the weaver and then the context, in that order.
            try {
                if (sr != nullptr) {
                    sr->Delete();
                    sr = nullptr;
                }
            }
            catch (...) {
            }
        }
    };

    void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

    // CONKER_LEIA_SIMULATE's stand-in for the weave: the output's left half from the left eye's
    // left half, its right half from the right eye's right half. Both resources are put back in
    // the states they came in (the pair pixel-shader readable, the output a render target).
    void recordSimulatedWeave(ID3D12GraphicsCommandList *list, ID3D12Resource *input, ID3D12Resource *output, uint32_t width, uint32_t height) {
        transition(list, input, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(list, output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = input;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = output;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const UINT half = width / 2;
        const D3D12_BOX leftBox = { 0, 0, 0, half, height, 1 };
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, &leftBox);
        const D3D12_BOX rightBox = { width + half, 0, 0, width * 2, height, 1 };
        list->CopyTextureRegion(&destination, half, 0, 0, &source, &rightBox);
        transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        transition(list, input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    bool createD3D12Objects(LeiaImpl &m) {
        for (int i = 0; i < Ring; i++) {
            if (FAILED(m.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m.allocators[i].GetAddressOf())))) {
                return false;
            }

            if (FAILED(m.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m.allocators[i].Get(), nullptr, IID_PPV_ARGS(m.lists[i].GetAddressOf())))) {
                return false;
            }

            // CreateCommandList opens the list; it is reset on its slot's first turn.
            m.lists[i]->Close();
        }

        if (FAILED(m.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m.fence.GetAddressOf())))) {
            return false;
        }

        const D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
        if (FAILED(m.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(m.rtvHeap.GetAddressOf())))) {
            return false;
        }

        m.fenceEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return m.fenceEvent != nullptr;
    }
}

extern "C" {
    static bool __cdecl leiaCreate(void *device, void *queue, void *window, void **outHandle) {
        if (outHandle == nullptr) {
            return false;
        }

        *outHandle = nullptr;
        if ((device == nullptr) || (queue == nullptr) || (window == nullptr)) {
            return false;
        }

        try {
            auto m = std::make_unique<LeiaImpl>();
            m->device = static_cast<ID3D12Device *>(device);
            m->queue = static_cast<ID3D12CommandQueue *>(queue);

            m->simulate = (std::getenv("CONKER_LEIA_SIMULATE") != nullptr);
            if (m->simulate) {
                std::fprintf(stderr, "[LeiaSR] CONKER_LEIA_SIMULATE: no SR runtime used, the weave is a plain copy.\n");
            }
            else {
                // E_NOINTERFACE (or another failure) when the SR Platform, its service or an
                // SR display isn't here: the usual case on a machine without the hardware.
                const HRESULT hr = SimulatedReality::CreateSRInterfaceDX12(m->device, static_cast<HWND>(window), &m->sr);
                if (FAILED(hr) || (m->sr == nullptr)) {
                    std::fprintf(stderr, "[LeiaSR] no SR runtime, service or display here (CreateSRInterfaceDX12 hr 0x%08lX).\n", static_cast<unsigned long>(hr));
                    m->sr = nullptr;
                    return false;
                }

                m->sr->SetShaderSRGBConversion(false, false);
            }

            if (!createD3D12Objects(*m)) {
                std::fprintf(stderr, "[LeiaSR] couldn't create the weave's command lists.\n");
                return false;
            }

            std::fprintf(stderr, "[LeiaSR] weaver initialized.\n");
            *outHandle = m.release();
            return true;
        }
        catch (...) {
            std::fprintf(stderr, "[LeiaSR] the SR runtime threw while initializing.\n");
            return false;
        }
    }

    static bool __cdecl leiaWeave(void *handle, void *input, void *output, uint32_t width, uint32_t height, uint32_t outputFormat) {
        LeiaImpl *m = static_cast<LeiaImpl *>(handle);
        if ((m == nullptr) || ((m->sr == nullptr) && !m->simulate) || (input == nullptr) || (output == nullptr) || (width == 0) || (height == 0)) {
            return false;
        }

        try {
            ID3D12Resource *inputResource = static_cast<ID3D12Resource *>(input);
            ID3D12Resource *outputResource = static_cast<ID3D12Resource *>(output);
            const D3D12_RESOURCE_DESC inputDesc = inputResource->GetDesc();
            const D3D12_RESOURCE_DESC outputDesc = outputResource->GetDesc();
            if ((inputDesc.Width < UINT64(width) * 2) || (inputDesc.Height < height) || (outputDesc.Width < width) || (outputDesc.Height < height)) {
                std::fprintf(stderr, "[LeiaSR] weave declined: input %llux%u or output %llux%u is smaller than %ux%u.\n",
                    static_cast<unsigned long long>(inputDesc.Width), inputDesc.Height, static_cast<unsigned long long>(outputDesc.Width), outputDesc.Height, width, height);
                return false;
            }

            if (outputDesc.Format != DXGI_FORMAT(outputFormat)) {
                std::fprintf(stderr, "[LeiaSR] weave declined: the output's format %u isn't the one given (%u).\n", unsigned(outputDesc.Format), outputFormat);
                return false;
            }

            const unsigned slot = m->ringIndex++ % Ring;
            if (!m->waitForValue(m->slotFenceValue[slot])) {
                std::fprintf(stderr, "[LeiaSR] a previous weave never finished.\n");
                return false;
            }

            ID3D12CommandAllocator *allocator = m->allocators[slot].Get();
            ID3D12GraphicsCommandList *list = m->lists[slot].Get();
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator, nullptr))) {
                return false;
            }

            if (m->simulate) {
                recordSimulatedWeave(list, inputResource, outputResource, width, height);
            }
            else {
                // The output's render-target view; written at record time, so one slot serves.
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m->rtvHeap->GetCPUDescriptorHandleForHeapStart();
                m->device->CreateRenderTargetView(outputResource, nullptr, rtv);

                const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
                const D3D12_RECT scissor = { 0, 0, LONG(width), LONG(height) };
                list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                list->RSSetViewports(1, &viewport);
                list->RSSetScissorRects(1, &scissor);

                // Re-set every frame: SR-lib reads the size and format off the resource, and a
                // view cached across a resize goes stale.
                m->sr->SetInputTexture(inputResource);
                if (DXGI_FORMAT(outputFormat) != m->lastOutputFormat) {
                    m->sr->SetOutputFormat(DXGI_FORMAT(outputFormat));
                    m->lastOutputFormat = DXGI_FORMAT(outputFormat);
                }

                m->sr->Weave(list, viewport, scissor);
            }

            if (FAILED(list->Close())) {
                return false;
            }

            ID3D12CommandList *lists[] = { list };
            m->queue->ExecuteCommandLists(1, lists);
            const UINT64 value = ++m->fenceValue;
            if (FAILED(m->queue->Signal(m->fence.Get(), value))) {
                return false;
            }

            m->slotFenceValue[slot] = value;
            if (!m->loggedFirstWeave) {
                std::fprintf(stderr, "[LeiaSR] weaving %ux%u (pair %llux%u).\n", width, height, static_cast<unsigned long long>(inputDesc.Width), inputDesc.Height);
                m->loggedFirstWeave = true;
            }

            return true;
        }
        catch (...) {
            std::fprintf(stderr, "[LeiaSR] the SR runtime threw while weaving.\n");
            return false;
        }
    }

    static void __cdecl leiaDestroy(void *handle) {
        try {
            delete static_cast<LeiaImpl *>(handle);
        }
        catch (...) {
        }
    }

    // The one name RT64 asks for. The struct is static: the functions live in this module,
    // which RT64 keeps loaded for as long as it uses them.
    __declspec(dllexport) const void *__cdecl conker_leia_sr_get_api(uint32_t version) {
        static const conker_leia_sr_api_v1 api = {
            CONKER_LEIA_SR_ABI_VERSION, 0,
            &leiaCreate,
            &leiaWeave,
            &leiaDestroy,
        };

        return (version == CONKER_LEIA_SR_ABI_VERSION) ? static_cast<const void *>(&api) : nullptr;
    }
}
