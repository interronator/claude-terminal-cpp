#pragma once
#include <GLFW/glfw3.h>

#include <ctime>
#include <list>
#include <string>
#include <unordered_map>

#include "imgui.h"

#include "stb_image.h"

// Loads image files into OpenGL textures on demand (one new load per frame) and evicts the least recently used.
class ImageCache
{
public:
    struct Texture
    {
        ImTextureID Id = 0;
        int Width = 0;
        int Height = 0;
        bool Failed = false;
    };

    ~ImageCache()
    {
        for (const std::pair<const std::string, Slot>& Item : Slots_)
        {
            Release(Item.second);
        }
    }

    void BeginFrame() { LoadedThisFrame_ = false; }

    // Returns nullptr while the image is still waiting for its turn to load.
    const Texture* Get(const std::string& Path, const std::time_t ModifiedAt)
    {
        const std::string Key = Path + "|" + std::to_string(ModifiedAt);
        std::unordered_map<std::string, Slot>::iterator Found = Slots_.find(Key);
        if (Found != Slots_.end())
        {
            Recent_.splice(Recent_.begin(), Recent_, Found->second.RecentPosition);
            return &Found->second.Image;
        }
        if (LoadedThisFrame_ == true)
        {
            return nullptr;
        }
        LoadedThisFrame_ = true;

        Slot NewSlot;
        NewSlot.Image = Load(Path);
        Recent_.push_front(Key);
        NewSlot.RecentPosition = Recent_.begin();
        Slot& Stored = Slots_.emplace(Key, NewSlot).first->second;
        Evict();
        return &Stored.Image;
    }

private:
    struct Slot
    {
        Texture Image;
        std::list<std::string>::iterator RecentPosition;
    };

    static constexpr size_t MAX_TEXTURES = 40;
    static constexpr unsigned int CLAMP_TO_EDGE = 0x812F;

    static Texture Load(const std::string& Path)
    {
        Texture Result;
        int Channels = 0;
        unsigned char* const Pixels = stbi_load(Path.c_str(), &Result.Width, &Result.Height, &Channels, 4);
        if (Pixels == nullptr)
        {
            Result.Failed = true;
            return Result;
        }
        GLuint Handle = 0;
        glGenTextures(1, &Handle);
        glBindTexture(GL_TEXTURE_2D, Handle);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Result.Width, Result.Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, Pixels);
        stbi_image_free(Pixels);
        Result.Id = static_cast<ImTextureID>(Handle);
        return Result;
    }

    static void Release(const Slot& Item)
    {
        if (Item.Image.Id == 0)
        {
            return;
        }
        const GLuint Handle = static_cast<GLuint>(Item.Image.Id);
        glDeleteTextures(1, &Handle);
    }

    void Evict()
    {
        while (Slots_.size() > MAX_TEXTURES)
        {
            const std::string Oldest = Recent_.back();
            Recent_.pop_back();
            Release(Slots_.at(Oldest));
            Slots_.erase(Oldest);
        }
    }

    std::unordered_map<std::string, Slot> Slots_;
    std::list<std::string> Recent_;
    bool LoadedThisFrame_ = false;
};
