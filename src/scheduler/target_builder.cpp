#define STB_IMAGE_IMPLEMENTATION
#include ".deps/stb/stb_image.h"

#include "scheduler/target_builder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace paintboard {
namespace {

bool load_ppm(const std::string& path, std::uint32_t& width,
              std::uint32_t& height, std::vector<std::uint32_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
  if (data.size() < 2 || data[0] != 'P' || data[1] != '6')
    return false;

  std::size_t pos = 2;
  auto skip = [&]() {
    for (;;) {
      while (pos < data.size() &&
             std::isspace(static_cast<unsigned char>(data[pos])))
        ++pos;
      if (pos < data.size() && data[pos] == '#') {
        while (pos < data.size() && data[pos] != '\n')
          ++pos;
        continue;
      }
      break;
    }
  };
  auto number = [&](std::uint32_t& value) {
    skip();
    if (pos >= data.size() ||
        !std::isdigit(static_cast<unsigned char>(data[pos])))
      return false;
    value = 0;
    while (pos < data.size() &&
           std::isdigit(static_cast<unsigned char>(data[pos]))) {
      value = value * 10 + static_cast<std::uint32_t>(data[pos] - '0');
      ++pos;
    }
    return true;
  };

  std::uint32_t max_value = 0;
  if (!number(width) || !number(height) || !number(max_value) ||
      max_value != 255 || width == 0 || height == 0)
    return false;
  if (pos < data.size() && std::isspace(static_cast<unsigned char>(data[pos]))) {
    if (data[pos] == '\r' && pos + 1 < data.size() && data[pos + 1] == '\n')
      pos += 2;
    else
      ++pos;
  }

  const std::uint64_t need = static_cast<std::uint64_t>(width) * height * 3;
  if (need > data.size() - pos)
    return false;

  out.resize(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < out.size(); ++i) {
    const std::uint32_t r = data[pos], g = data[pos + 1], b = data[pos + 2];
    pos += 3;
    out[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
  }
  return true;
}

bool load_image(const std::string& path, std::uint32_t& width,
                std::uint32_t& height, std::vector<std::uint32_t>& out) {
  if (path.empty())
    return false;

  int w = 0, h = 0, channels = 0;
  unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (pixels) {
    if (w <= 0 || h <= 0) {
      stbi_image_free(pixels);
      return false;
    }
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    out.resize(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < out.size(); ++i) {
      std::uint32_t r = pixels[4 * i], g = pixels[4 * i + 1],
                   b = pixels[4 * i + 2], a = pixels[4 * i + 3];
      if (a != 255) {
        r = r * a / 255 + (255 - a);
        g = g * a / 255 + (255 - a);
        b = b * a / 255 + (255 - a);
      }
      out[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
    }
    stbi_image_free(pixels);
    return true;
  }
  return load_ppm(path, width, height, out);
}

} // namespace

Target TargetBuilder::build(const Config& config, const Rules& rules) const {
  Target target;
  target.built = true;
  target.W = rules.width;
  target.H = rules.height;
  if (rules.width == 0 || rules.height == 0)
    return target;

  const std::size_t n = static_cast<std::size_t>(rules.width) * rules.height;
  target.pix.assign(n, 0);
  target.known.assign(n, 0);

  std::uint32_t sw = 0, sh = 0;
  std::vector<std::uint32_t> src;
  const bool has_image = load_image(config.image_path, sw, sh, src);
  if (has_image) {
    for (std::uint32_t y = 0; y < sh; ++y) {
      for (std::uint32_t x = 0; x < sw; ++x) {
        const std::int64_t gx = static_cast<std::int64_t>(x) + config.offset_x;
        const std::int64_t gy = static_cast<std::int64_t>(y) + config.offset_y;
        if (gx < 0 || gy < 0 || gx >= rules.width || gy >= rules.height)
          continue;
        const std::size_t p = static_cast<std::size_t>(gy) * rules.width + gx;
        target.pix[p] = src[static_cast<std::size_t>(y) * sw + x];
        target.known[p] = 1;
      }
    }
  }

  for (const Draw& draw : config.draws) {
    if (draw.x >= rules.width || draw.y >= rules.height)
      continue;
    const std::size_t p = static_cast<std::size_t>(draw.y) * rules.width + draw.x;
    target.pix[p] = draw.color;
    target.known[p] = 1;
  }

  target.bb_x0 = rules.width;
  target.bb_y0 = rules.height;
  target.bb_x1 = 0;
  target.bb_y1 = 0;
  for (std::uint32_t y = 0; y < rules.height; ++y) {
    for (std::uint32_t x = 0; x < rules.width; ++x) {
      const std::size_t p = static_cast<std::size_t>(y) * rules.width + x;
      if (!target.known[p])
        continue;
      target.known_idx.push_back(static_cast<std::uint32_t>(p));
      target.bb_x0 = std::min(target.bb_x0, x);
      target.bb_y0 = std::min(target.bb_y0, y);
      target.bb_x1 = std::max(target.bb_x1, x);
      target.bb_y1 = std::max(target.bb_y1, y);
    }
  }
  if (target.known_idx.empty()) {
    target.bb_x0 = 0;
    target.bb_y0 = 0;
    target.bb_x1 = rules.width - 1;
    target.bb_y1 = rules.height - 1;
  }

  std::uint32_t stride = config.sonar_stride;
  const std::uint32_t need = 2 * static_cast<std::uint32_t>(rules.radius) + 1;
  if (stride < need)
    stride = need;
  if (stride < 1)
    stride = 1;
  const std::uint32_t bw =
      target.bb_x1 >= target.bb_x0 ? (target.bb_x1 - target.bb_x0) / stride + 1 : 0;
  const std::uint32_t bh =
      target.bb_y1 >= target.bb_y0 ? (target.bb_y1 - target.bb_y0) / stride + 1 : 0;
  for (std::uint32_t k = 0; k < bw * bh; ++k) {
    const std::uint32_t gx = target.bb_x0 + (k % bw) * stride;
    const std::uint32_t gy = target.bb_y0 + (k / bw) * stride;
    if (gx >= rules.width || gy >= rules.height)
      continue;
    target.sp_pts.push_back(gy * rules.width + gx);
  }

  return target;
}

Target build_target(const Config& config, const Rules& rules, const Board&) {
  return TargetBuilder{}.build(config, rules);
}

} // namespace paintboard
