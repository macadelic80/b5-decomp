// FLAG PC divergence: the D3D9 backend of the mods' UI (see ModUi.h). Fixed-function, no depth,
// alpha blended, one scissor rect per draw; pre-transformed vertices in back buffer pixels.
// ModHost.cpp has already saved the whole device state and bound the back buffer, and puts both
// back afterwards, so nothing here restores anything.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModUi.h"

#include <Windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace CgsPC { namespace Mods { namespace Ui
{
    namespace
    {
        struct D3DVertex
        {
            float    mfX, mfY, mfZ, mfRhw;
            D3DCOLOR muColour;
            float    mfU, mfV;
        };
        const DWORD KU_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

        // brn_vertex / texture bytes are r, g, b, a; D3DCOLOR and A8R8G8B8 are b, g, r, a in memory.
        inline uint32_t SwapRedBlue(uint32_t luRgba)
        {
            return (luRgba & 0xFF00FF00u) | ((luRgba & 0x000000FFu) << 16) | ((luRgba & 0x00FF0000u) >> 16);
        }

        struct DeviceTexture
        {
            IDirect3DTexture9* mpTexture = nullptr;
            uint32_t           muWidth = 0, muHeight = 0;
            uint32_t           muRevision = 0;
        };

        class D3D9Backend final : public Backend
        {
        public:
            explicit D3D9Backend(IDirect3DDevice9* lpDevice) : mpDevice(lpDevice) {}

            ~D3D9Backend() override
            {
                for (auto& lrEntry : mTextures) lrEntry.second.mpTexture->Release();
                if (mpWhite) mpWhite->Release();
                if (mpVertices) mpVertices->Release();
                if (mpIndices) mpIndices->Release();
            }

            void BeginFrame(uint32_t luWidth, uint32_t luHeight) override
            {
                muWidth = luWidth;
                muHeight = luHeight;
                for (uint64_t luId : TakeRetiredTextures())
                {
                    const auto lIt = mTextures.find(luId);
                    if (lIt == mTextures.end()) continue;
                    lIt->second.mpTexture->Release();
                    mTextures.erase(lIt);
                }
            }

            void Draw(const DrawList& lrList, const TextureSet& lrTextures) override
            {
                if (lrList.maCommands.empty() || !Upload(lrList) || !SetupState()) return;
                for (const DrawCommand& lrCommand : lrList.maCommands)
                {
                    const float lfX0 = std::max(lrCommand.mClip.x0, 0.0f);
                    const float lfY0 = std::max(lrCommand.mClip.y0, 0.0f);
                    const float lfX1 = std::min(lrCommand.mClip.x1, static_cast<float>(muWidth));
                    const float lfY1 = std::min(lrCommand.mClip.y1, static_cast<float>(muHeight));
                    if (lfX1 - lfX0 < 1.0f || lfY1 - lfY0 < 1.0f || lrCommand.muIndexCount < 3) continue;
                    IDirect3DTexture9* lpTexture = Resolve(lrCommand.muTexture, lrTextures);
                    if (lpTexture == nullptr) continue;
                    const RECT lScissor = { static_cast<LONG>(std::lround(lfX0)), static_cast<LONG>(std::lround(lfY0)),
                                            static_cast<LONG>(std::lround(lfX1)), static_cast<LONG>(std::lround(lfY1)) };
                    mpDevice->SetScissorRect(&lScissor);
                    mpDevice->SetTexture(0, lpTexture);
                    // ImGui hands straight alpha, egui premultiplied: only the colour source factor differs.
                    mpDevice->SetRenderState(D3DRS_SRCBLEND, (lrCommand.muFlags & BRN_UI_PREMULTIPLIED) ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
                    mpDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, static_cast<INT>(lrCommand.muVertexOffset), 0,
                                                   lrCommand.muVertexCount, lrCommand.muIndexOffset, lrCommand.muIndexCount / 3);
                }
            }

        private:
            IDirect3DTexture9* CreateTexture(uint32_t luWidth, uint32_t luHeight)
            {
                IDirect3DTexture9* lpTexture = nullptr;
                // DEFAULT + DYNAMIC: valid on both the D3D9 and the D3D9Ex device this renderer creates.
                if (FAILED(mpDevice->CreateTexture(luWidth, luHeight, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                                   D3DPOOL_DEFAULT, &lpTexture, nullptr)))
                    return nullptr;
                return lpTexture;
            }

            static bool Fill(IDirect3DTexture9* lpTexture, const uint8_t* lpRgba, uint32_t luWidth, uint32_t luHeight)
            {
                D3DLOCKED_RECT lLocked = {};
                if (FAILED(lpTexture->LockRect(0, &lLocked, nullptr, D3DLOCK_DISCARD))) return false;
                for (uint32_t luRow = 0; luRow < luHeight; ++luRow)
                {
                    uint32_t* lpTarget = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lLocked.pBits) + luRow * lLocked.Pitch);
                    const uint8_t* lpSource = lpRgba + static_cast<size_t>(luRow) * luWidth * 4u;
                    for (uint32_t luColumn = 0; luColumn < luWidth; ++luColumn)
                    {
                        uint32_t luPixel;
                        std::memcpy(&luPixel, lpSource + luColumn * 4u, 4);
                        lpTarget[luColumn] = SwapRedBlue(luPixel);
                    }
                }
                lpTexture->UnlockRect(0);
                return true;
            }

            IDirect3DTexture9* Resolve(uint32_t luHandle, const TextureSet& lrTextures)
            {
                if (luHandle == 0)
                {
                    if (mpWhite == nullptr && (mpWhite = CreateTexture(1, 1)) != nullptr)
                    {
                        const uint8_t lacWhite[4] = { 255, 255, 255, 255 };
                        Fill(mpWhite, lacWhite, 1, 1);
                    }
                    return mpWhite;
                }
                const Texture* lpSource = lrTextures.Find(luHandle);
                if (lpSource == nullptr) return nullptr;
                DeviceTexture& lrCopy = mTextures[lpSource->muId];
                if (lrCopy.mpTexture != nullptr && lrCopy.muRevision == lpSource->muRevision) return lrCopy.mpTexture;
                if (lrCopy.mpTexture != nullptr && (lrCopy.muWidth != lpSource->muWidth || lrCopy.muHeight != lpSource->muHeight))
                {
                    lrCopy.mpTexture->Release();
                    lrCopy.mpTexture = nullptr;
                }
                if (lrCopy.mpTexture == nullptr)
                {
                    lrCopy.mpTexture = CreateTexture(lpSource->muWidth, lpSource->muHeight);
                    if (lrCopy.mpTexture == nullptr) { mTextures.erase(lpSource->muId); return nullptr; }
                    lrCopy.muWidth = lpSource->muWidth;
                    lrCopy.muHeight = lpSource->muHeight;
                }
                // The CPU copy is whole: a partial update re-sends the texture (font atlases change rarely).
                if (!Fill(lrCopy.mpTexture, lpSource->maRgba.data(), lpSource->muWidth, lpSource->muHeight)) return nullptr;
                lrCopy.muRevision = lpSource->muRevision;
                return lrCopy.mpTexture;
            }

            bool Upload(const DrawList& lrList)
            {
                const uint32_t luVertices = static_cast<uint32_t>(lrList.maVertices.size());
                const uint32_t luIndices = static_cast<uint32_t>(lrList.maIndices.size());
                if (luVertices == 0 || luIndices == 0) return false;
                if (mpVertices == nullptr || muVertexCapacity < luVertices)
                {
                    if (mpVertices) { mpVertices->Release(); mpVertices = nullptr; }
                    muVertexCapacity = std::max<uint32_t>(4096, 1u << static_cast<uint32_t>(std::ceil(std::log2(luVertices))));
                    if (FAILED(mpDevice->CreateVertexBuffer(muVertexCapacity * sizeof(D3DVertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                            KU_FVF, D3DPOOL_DEFAULT, &mpVertices, nullptr)))
                    {
                        mpVertices = nullptr;
                        return false;
                    }
                }
                if (mpIndices == nullptr || muIndexCapacity < luIndices)
                {
                    if (mpIndices) { mpIndices->Release(); mpIndices = nullptr; }
                    muIndexCapacity = std::max<uint32_t>(8192, 1u << static_cast<uint32_t>(std::ceil(std::log2(luIndices))));
                    if (FAILED(mpDevice->CreateIndexBuffer(muIndexCapacity * sizeof(uint32_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                                                           D3DFMT_INDEX32, D3DPOOL_DEFAULT, &mpIndices, nullptr)))
                    {
                        mpIndices = nullptr;
                        return false;
                    }
                }
                void* lpTarget = nullptr;
                if (FAILED(mpVertices->Lock(0, luVertices * sizeof(D3DVertex), &lpTarget, D3DLOCK_DISCARD))) return false;
                D3DVertex* lpVertex = static_cast<D3DVertex*>(lpTarget);
                for (const brn_vertex& lrSource : lrList.maVertices)
                {
                    // -0.5: D3D9 samples pixel centres at integer coordinates.
                    *lpVertex++ = { lrSource.x - 0.5f, lrSource.y - 0.5f, 0.0f, 1.0f, SwapRedBlue(lrSource.rgba),
                                    lrSource.u, lrSource.v };
                }
                mpVertices->Unlock();
                if (FAILED(mpIndices->Lock(0, luIndices * sizeof(uint32_t), &lpTarget, D3DLOCK_DISCARD))) return false;
                std::memcpy(lpTarget, lrList.maIndices.data(), luIndices * sizeof(uint32_t));
                mpIndices->Unlock();
                return true;
            }

            bool SetupState()
            {
                IDirect3DDevice9* lpDevice = mpDevice;
                if (FAILED(lpDevice->SetVertexShader(nullptr)) || FAILED(lpDevice->SetPixelShader(nullptr)) ||
                    FAILED(lpDevice->SetFVF(KU_FVF)))
                    return false;
                // The renderer instances: make sure stream 0 is plain per-vertex data.
                lpDevice->SetStreamSourceFreq(0, 1);
                lpDevice->SetStreamSourceFreq(1, 1);
                lpDevice->SetStreamSource(0, mpVertices, 0, sizeof(D3DVertex));
                lpDevice->SetIndices(mpIndices);
                const D3DVIEWPORT9 lViewport = { 0, 0, muWidth, muHeight, 0.0f, 1.0f };
                lpDevice->SetViewport(&lViewport);
                static const struct { D3DRENDERSTATETYPE meState; DWORD muValue; } KA_STATES[] = {
                    { D3DRS_FILLMODE, D3DFILL_SOLID },           { D3DRS_ZENABLE, FALSE },
                    { D3DRS_ZWRITEENABLE, FALSE },               { D3DRS_ALPHATESTENABLE, FALSE },
                    { D3DRS_CULLMODE, D3DCULL_NONE },            { D3DRS_ALPHABLENDENABLE, TRUE },
                    { D3DRS_BLENDOP, D3DBLENDOP_ADD },           { D3DRS_SRCBLEND, D3DBLEND_SRCALPHA },
                    { D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA },   { D3DRS_SEPARATEALPHABLENDENABLE, TRUE },
                    { D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD },      { D3DRS_SRCBLENDALPHA, D3DBLEND_ONE },
                    { D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA }, { D3DRS_SCISSORTESTENABLE, TRUE },
                    { D3DRS_STENCILENABLE, FALSE },              { D3DRS_FOGENABLE, FALSE },
                    { D3DRS_LIGHTING, FALSE },                   { D3DRS_COLORWRITEENABLE, 0xF },
                    { D3DRS_SRGBWRITEENABLE, FALSE },            { D3DRS_MULTISAMPLEANTIALIAS, FALSE },
                };
                for (const auto& lrState : KA_STATES) lpDevice->SetRenderState(lrState.meState, lrState.muValue);
                lpDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                lpDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                lpDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
                lpDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                lpDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                lpDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                lpDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
                lpDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
                lpDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                lpDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                lpDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                lpDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                lpDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
                lpDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
                lpDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
                return true;
            }

            IDirect3DDevice9*                           mpDevice;
            uint32_t                                    muWidth = 0, muHeight = 0;
            std::unordered_map<uint64_t, DeviceTexture> mTextures;
            IDirect3DTexture9*                          mpWhite = nullptr;
            IDirect3DVertexBuffer9*                     mpVertices = nullptr;
            IDirect3DIndexBuffer9*                      mpIndices = nullptr;
            uint32_t                                    muVertexCapacity = 0;
            uint32_t                                    muIndexCapacity = 0;
        };
    }

    std::unique_ptr<Backend> CreateD3D9Backend(IDirect3DDevice9* lpDevice)
    {
        return std::make_unique<D3D9Backend>(lpDevice);
    }
} } }

#endif // BRN_PC_MODS
