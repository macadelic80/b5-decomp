#pragma once

// FLAG PC divergence: the UI path of the WebAssembly mods. A mod never touches the graphics
// device: it submits textured triangle lists (brn.ui.v1, the shape ImGui's ImDrawData and egui's
// ClippedPrimitive both produce) into its DrawList, and owns its textures as plain RGBA8 pixels
// in a TextureSet. A Backend draws both on the render thread. Only Backend knows the graphics
// API: ModUiD3D9.cpp is the one this build ships; a Vulkan or Metal port adds its own.

#include "pc/mods/api/brn_mod_api.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

struct IDirect3DDevice9;

namespace CgsPC { namespace Mods { namespace Ui
{
    struct Texture
    {
        uint64_t              muId;        // unique for the process, never reused (backend cache key)
        uint32_t              muWidth;
        uint32_t              muHeight;
        uint32_t              muRevision;  // bumped by every update: the backend re-uploads
        std::vector<uint8_t>  maRgba;      // muWidth * muHeight * 4, rows top to bottom
    };

    // One mod's textures, by the handle the mod sees. Touched only inside the mod's own calls
    // (the runtime holds its lock) and by the backend while it draws that mod's list.
    class TextureSet
    {
    public:
        TextureSet() = default;
        TextureSet(const TextureSet&) = delete;
        TextureSet& operator=(const TextureSet&) = delete;
        ~TextureSet();   // retires every texture: the backend frees the device copies

        static constexpr uint32_t KU_MAX_TEXTURES = 1024;

        uint32_t       Create(const uint8_t* lpRgba, uint32_t luWidth, uint32_t luHeight);   // 0 = refused
        bool           Update(uint32_t luHandle, uint32_t luX, uint32_t luY, uint32_t luWidth,
                              uint32_t luHeight, const uint8_t* lpRgba);
        void           Destroy(uint32_t luHandle);
        const Texture* Find(uint32_t luHandle) const;

    private:
        std::unordered_map<uint32_t, std::unique_ptr<Texture>> mTextures;
        uint32_t                                               muNextHandle = 1;
    };

    struct DrawCommand
    {
        uint32_t muTexture;      // TextureSet handle; 0 = untextured (white)
        brn_rect mClip;
        uint32_t muVertexOffset; // into DrawList::maVertices
        uint32_t muVertexCount;
        uint32_t muIndexOffset;  // into DrawList::maIndices; indices are relative to muVertexOffset
        uint32_t muIndexCount;
        uint32_t muFlags;        // BRN_UI_*
    };

    // What a mod submitted during one brn_mod_render.
    struct DrawList
    {
        static constexpr uint32_t KU_MAX_VERTICES = 1u << 21;
        static constexpr uint32_t KU_MAX_INDICES = 3u << 21;

        std::vector<brn_vertex>  maVertices;
        std::vector<uint32_t>    maIndices;
        std::vector<DrawCommand> maCommands;

        void Clear() { maVertices.clear(); maIndices.clear(); maCommands.clear(); }
    };

    class Backend
    {
    public:
        virtual ~Backend() = default;
        // Start of the mods' frame: free the device copies of retired textures.
        virtual void BeginFrame(uint32_t luWidth, uint32_t luHeight) = 0;
        // Draw lrList over the frame, in order, sampling lrTextures.
        virtual void Draw(const DrawList& lrList, const TextureSet& lrTextures) = 0;
    };

    // ModUiD3D9.cpp. The backend draws with lpDevice's current render target; the caller saves
    // and restores the device state around it.
    std::unique_ptr<Backend> CreateD3D9Backend(IDirect3DDevice9* lpDevice);

    // Texture ids whose TextureSet entry is gone; a backend frees its copies (render thread).
    std::vector<uint64_t> TakeRetiredTextures();
} } }
