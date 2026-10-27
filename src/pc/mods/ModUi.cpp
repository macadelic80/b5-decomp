// FLAG PC divergence: the graphics-API-free half of the mods' UI path (see ModUi.h).

#include "pc/mods/ModHost.h"

#if BRN_PC_MODS

#include "pc/mods/ModUi.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace CgsPC { namespace Mods { namespace Ui
{
    namespace
    {
        std::atomic<uint64_t> suNextTextureId{1};
        std::mutex            sRetiredMutex;
        std::vector<uint64_t> saRetired;

        void Retire(uint64_t luId)
        {
            std::lock_guard<std::mutex> lLock(sRetiredMutex);
            saRetired.push_back(luId);
        }
    }

    std::vector<uint64_t> TakeRetiredTextures()
    {
        std::vector<uint64_t> laRetired;
        std::lock_guard<std::mutex> lLock(sRetiredMutex);
        laRetired.swap(saRetired);
        return laRetired;
    }

    TextureSet::~TextureSet()
    {
        for (const auto& lrEntry : mTextures) Retire(lrEntry.second->muId);
    }

    uint32_t TextureSet::Create(const uint8_t* lpRgba, uint32_t luWidth, uint32_t luHeight)
    {
        if (luWidth == 0 || luHeight == 0 || luWidth > BRN_UI_TEXTURE_LIMIT || luHeight > BRN_UI_TEXTURE_LIMIT ||
            mTextures.size() >= KU_MAX_TEXTURES)
            return 0;
        auto lpTexture = std::make_unique<Texture>();
        lpTexture->muId = suNextTextureId.fetch_add(1);
        lpTexture->muWidth = luWidth;
        lpTexture->muHeight = luHeight;
        lpTexture->muRevision = 1;
        lpTexture->maRgba.assign(lpRgba, lpRgba + static_cast<size_t>(luWidth) * luHeight * 4u);
        const uint32_t luHandle = muNextHandle++;
        mTextures.emplace(luHandle, std::move(lpTexture));
        return luHandle;
    }

    bool TextureSet::Update(uint32_t luHandle, uint32_t luX, uint32_t luY, uint32_t luWidth,
                            uint32_t luHeight, const uint8_t* lpRgba)
    {
        const auto lIt = mTextures.find(luHandle);
        if (lIt == mTextures.end()) return false;
        Texture& lrTexture = *lIt->second;
        if (luX > lrTexture.muWidth || luWidth > lrTexture.muWidth - luX ||
            luY > lrTexture.muHeight || luHeight > lrTexture.muHeight - luY)
            return false;
        for (uint32_t luRow = 0; luRow < luHeight; ++luRow)
            std::memcpy(&lrTexture.maRgba[(static_cast<size_t>(luY + luRow) * lrTexture.muWidth + luX) * 4u],
                        lpRgba + static_cast<size_t>(luRow) * luWidth * 4u, static_cast<size_t>(luWidth) * 4u);
        ++lrTexture.muRevision;
        return true;
    }

    void TextureSet::Destroy(uint32_t luHandle)
    {
        const auto lIt = mTextures.find(luHandle);
        if (lIt == mTextures.end()) return;
        Retire(lIt->second->muId);
        mTextures.erase(lIt);
    }

    const Texture* TextureSet::Find(uint32_t luHandle) const
    {
        const auto lIt = mTextures.find(luHandle);
        return lIt == mTextures.end() ? nullptr : lIt->second.get();
    }
} } }

#endif // BRN_PC_MODS
