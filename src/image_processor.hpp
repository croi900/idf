#ifndef IDF_IMAGE_PROCESSOR_HPP
#define IDF_IMAGE_PROCESSOR_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "src/third_party/stb_image.h"

#include "src/db_manager.hpp"
#include "src/dirtree.hpp"
#include "src/query_parser.h"

namespace idf::image {

inline std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::string classify_rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int ri = r;
    const int gi = g;
    const int bi = b;
    const int maxc = std::max({ri, gi, bi});
    const int minc = std::min({ri, gi, bi});
    const int spread = maxc - minc;

    if (maxc < 40) return "black";
    if (minc > 215 && spread < 30) return "white";
    if (spread < 35) return "gray";

    float hue = 0.f;
    if (spread > 0) {
        float rf = r / 255.f;
        float gf = g / 255.f;
        float bf = b / 255.f;
        float mx = std::max({rf, gf, bf});
        float mn = std::min({rf, gf, bf});
        float d = mx - mn;
        if (mx == rf) hue = (gf - bf) / d + (gf < bf ? 6.f : 0.f);
        else if (mx == gf) hue = (bf - rf) / d + 2.f;
        else hue = (rf - gf) / d + 4.f;
        hue *= 60.f;
    }

    if (hue < 15.f || hue >= 345.f) return "red";
    if (hue < 45.f) return "orange";
    if (hue < 70.f) return "yellow";
    if (hue < 160.f) return "green";
    if (hue < 200.f) return "cyan";
    if (hue < 260.f) return "blue";
    if (hue < 290.f) return "purple";
    if (hue < 330.f) return "pink";
    return "red";
}

inline std::string dominant_color_name(std::span<const std::uint8_t> file_bytes) {
    if (file_bytes.empty()) return "unknown";

    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char* pixels = stbi_load_from_memory(
        file_bytes.data(),
        static_cast<int>(file_bytes.size()),
        &w,
        &h,
        &comp,
        3);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return "unknown";
    }

    constexpr int kTarget = 32;
    std::unordered_map<std::string, int> counts;
    counts.reserve(32);

    for (int y = 0; y < kTarget; ++y) {
        int sy = y * h / kTarget;
        for (int x = 0; x < kTarget; ++x) {
            int sx = x * w / kTarget;
            const unsigned char* p = pixels + (sy * w + sx) * 3;
            std::string name = classify_rgb(p[0], p[1], p[2]);
            counts[name]++;
        }
    }

    stbi_image_free(pixels);

    std::string best = "unknown";
    int best_count = 0;
    for (const auto& [name, count] : counts) {
        if (count > best_count) {
            best_count = count;
            best = name;
        }
    }
    return best;
}

inline idf::parse_batch make_image_batch(
    const dirtree::file_entry& entry,
    uint64_t file_hash,
    const std::string& color)
{
    idf::parse_batch pb;
    pb.file = entry;
    pb.lang = "image";
    pb.score = idf::qp::score_path(entry, "image");
    pb.dominant_color = color;
    std::string token = "color:" + color;
    pb.text_tokens.emplace_back(
        std::move(token),
        idf::text_occurrence{file_hash, 0});
    return pb;
}

} // namespace idf::image

#endif
