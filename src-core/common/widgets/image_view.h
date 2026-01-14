#pragma once

#include "imgui/imgui.h"
#include "common/image/image.h"
#include <mutex>
#include <vector>
#include <string>
#include <functional>

class ImageViewWidget
{
private:
    struct CachedChunk
    {
        unsigned int texture_id = 0;
        int img_width = 0;
        int img_height = 0;
        int offset_x = 0;
        int offset_y = 0;
    };

    struct ImageContainer
    {
        unsigned int texture_id = 0;
        std::vector<uint32_t> texture_buffer;

        int img_width = 0;
        int img_height = 0;

        int offset_x = 0;
        int offset_y = 0;
    };

private:
    std::vector<ImageContainer> img_chunks;

    int fimg_width = 0;
    int fimg_height = 0;

    bool has_to_update = false;
    std::mutex image_mtx;

    std::string id_str;

    bool autoFitNextFrame = false;
    const image::Image *cached_image_ptr = nullptr;
    uint64_t cached_image_revision = 0;
    std::vector<CachedChunk> cached_chunks;
    bool has_cached_image = false;
    ImVec4 image_tint = ImVec4(1, 1, 1, 1);

public:
    ImageViewWidget();
    ~ImageViewWidget();

    std::function<void(int x, int y)> mouseCallback = [](int, int) {}; // Function that can be used to handle mouse events
    std::function<void()> plotOverlay = []() {};

    void update(image::Image &image);
    void updateCached(const image::Image *image, uint64_t revision, float alpha = 1.0f);
    void syncTextures();
    void plotChunks();
    void draw(ImVec2 win_size);

    unsigned int getTextID() { return img_chunks.size() > 0 ? img_chunks[0].texture_id : 0; }
};
