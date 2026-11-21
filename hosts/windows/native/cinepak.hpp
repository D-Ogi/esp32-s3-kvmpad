// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

// One full color V1 codebook, one independent strip. Each vector carries four quadrant
// luminances and shared chroma. Median-cut stores all block assignments in original raster order.
namespace cinepak {
constexpr unsigned width = 960, height = 540, blocks = width * height / 16;
constexpr unsigned size = 10 + 12 + 4 + 256 * 6 + 4 + blocks;
using Vector = std::array<int16_t, 6>;
struct Block { Vector color; unsigned position; };

// Computer UI graphics often fit the complete V1 codebook. Keep every such vector exact.
// Stop looking as soon as a photographic frame needs a 257th entry.
inline bool exact_palette(const std::vector<Block>& vectors, uint8_t* book, uint8_t* indices) {
    std::map<Vector, uint8_t> colors;
    for (const auto& block : vectors) {
        auto entry = colors.lower_bound(block.color);
        if (entry == colors.end() || entry->first != block.color) {
            if (colors.size() == 256) return false;
            auto index = static_cast<uint8_t>(colors.size());
            entry = colors.emplace_hint(entry, block.color, index);
            for (unsigned i = 0; i < 6; ++i)
                book[index * 6 + i] = static_cast<uint8_t>(block.color[i]);
        }
        indices[block.position] = entry->second;
    }
    return true;
}

inline void palette(Block* first, Block* last, unsigned index, unsigned depth,
                    uint8_t* book, uint8_t* indices) {
    if (!depth) {
        std::array<int, 6> sum{};
        for (auto block = first; block != last; ++block) {
            for (unsigned i = 0; i < 6; ++i) sum[i] += block->color[i];
            indices[block->position] = static_cast<uint8_t>(index);
        }
        for (unsigned i = 0; i < 6; ++i)
            book[index * 6 + i] = static_cast<uint8_t>(sum[i] / (last - first));
        return;
    }
    Vector low = first->color, high = first->color;
    for (auto block = first + 1; block != last; ++block)
        for (unsigned i = 0; i < 6; ++i) {
            low[i] = std::min(low[i], block->color[i]);
            high[i] = std::max(high[i], block->color[i]);
        }
    unsigned axis = 0;
    int widest = -1;
    for (unsigned i = 0; i < 6; ++i) {
        int width_on_axis = (high[i] - low[i]) * (i >= 4 ? 2 : 1);
        if (width_on_axis > widest) { widest = width_on_axis; axis = i; }
    }
    auto middle = first + (last - first) / 2;
    std::nth_element(first, middle, last, [axis](const Block& a, const Block& b) {
        return a.color[axis] < b.color[axis];
    });
    palette(first, middle, index * 2, depth - 1, book, indices);
    palette(middle, last, index * 2 + 1, depth - 1, book, indices);
}

inline std::vector<uint8_t> encode(const uint32_t* pixels) {
    std::vector<Block> vectors(blocks);
    for (unsigned by = 0; by < height; by += 4)
        for (unsigned bx = 0; bx < width; bx += 4) {
            unsigned position = by / 4 * (width / 4) + bx / 4;
            auto& block = vectors[position];
            block.position = position;
            int sum_u = 0, sum_v = 0;
            for (unsigned q = 0; q < 4; ++q) {
                int sum_y = 0;
                for (unsigned y = 0; y < 2; ++y)
                    for (unsigned x = 0; x < 2; ++x) {
                        uint32_t c = pixels[(by + (q / 2) * 2 + y) * width + bx + (q % 2) * 2 + x];
                        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                        int luma = (2*r + 4*g + b + 3) / 7;
                        sum_y += luma;
                        sum_u += b - luma;
                        sum_v += r - luma;
                    }
                block.color[q] = static_cast<int16_t>((sum_y + 2) / 4);
            }
            block.color[4] = static_cast<int16_t>(sum_u / 32);
            block.color[5] = static_cast<int16_t>(sum_v / 32);
        }
    std::vector<uint8_t> packet(size);
    auto length = [&](unsigned offset, unsigned value) {
        packet[offset] = static_cast<uint8_t>(value >> 16);
        packet[offset+1] = static_cast<uint8_t>(value >> 8);
        packet[offset+2] = static_cast<uint8_t>(value);
    };
    length(1, size); packet[4] = 3; packet[5] = 192; packet[6] = 2; packet[7] = 28;
    packet[9] = 1; packet[10] = 0x10; length(11, size - 10);
    packet[18] = 2; packet[19] = 28; packet[20] = 3; packet[21] = 192;
    packet[22] = 0x22; length(23, 4 + 256 * 6);
    constexpr unsigned image = 26 + 256 * 6;
    packet[image] = 0x32; length(image + 1, 4 + blocks);
    auto* book = packet.data() + 26;
    auto* indices = packet.data() + image + 4;
    if (!exact_palette(vectors, book, indices))
        palette(vectors.data(), vectors.data() + blocks, 0, 8, book, indices);
    return packet;
}
}
