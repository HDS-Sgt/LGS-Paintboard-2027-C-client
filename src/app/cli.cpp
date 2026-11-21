#include "app/cli.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace paintboard {
namespace {

std::uint32_t hex_value(char c) {
  if (c >= '0' && c <= '9')
    return static_cast<std::uint32_t>(c - '0');
  if (c >= 'a' && c <= 'f')
    return static_cast<std::uint32_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F')
    return static_cast<std::uint32_t>(c - 'A' + 10);
  return 99;
}

bool parse_color(const char* text, std::uint32_t& out) {
  const std::size_t n = std::strlen(text);
  if (n != 6 && n != 8)
    return false;

  std::uint32_t value = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint32_t digit = hex_value(text[i]);
    if (digit > 15)
      return false;
    value = (value << 4) | digit;
  }

  std::uint32_t r, g, b, a = 255;
  if (n == 6) {
    r = (value >> 16) & 255;
    g = (value >> 8) & 255;
    b = value & 255;
  } else {
    r = (value >> 24) & 255;
    g = (value >> 16) & 255;
    b = (value >> 8) & 255;
    a = value & 255;
    if (a != 255)
      return false;
  }
  out = r | (g << 8) | (b << 16) | (a << 24);
  return true;
}

bool parse_token(const std::string& text, Token& out) {
  std::array<std::uint8_t, 32> digits{};
  std::size_t n = 0;
  for (char c : text) {
    if (c == '-')
      continue;
    const std::uint32_t digit = hex_value(c);
    if (digit > 15 || n >= 32)
      return false;
    digits[n++] = static_cast<std::uint8_t>(digit);
  }
  if (n != 32)
    return false;
  for (std::size_t i = 0; i < 16; ++i)
    out[i] = static_cast<std::uint8_t>((digits[2 * i] << 4) | digits[2 * i + 1]);
  return true;
}

bool add_token(Config& config, Group group, const char* text) {
  Token token{};
  if (!parse_token(text, token))
    return false;
  config.tokens.push_back(TokenEntry{group, token, false});
  return true;
}

std::size_t load_token_file(Config& config, const std::string& path) {
  std::ifstream in(path);
  if (!in)
    return 0;
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

  std::size_t added = 0;
  const std::pair<const char*, Group> groups[] = {
      {"paint", Group::Paint},
      {"repair", Group::Repair},
      {"sonar", Group::Sonar},
  };
  for (const auto& [key, group] : groups) {
    const std::string pattern = std::string("\"") + key + "\"";
    std::size_t pos = text.find(pattern);
    if (pos == std::string::npos)
      continue;
    const std::size_t open = text.find('[', pos);
    if (open == std::string::npos)
      continue;
    const std::size_t close = text.find(']', open);
    const std::size_t end = close == std::string::npos ? text.size() : close;
    std::size_t p = open;
    while (p < end) {
      const std::size_t q1 = text.find('"', p);
      if (q1 == std::string::npos || q1 >= end)
        break;
      const std::size_t q2 = text.find('"', q1 + 1);
      if (q2 == std::string::npos || q2 > end)
        break;
      const std::string value = text.substr(q1 + 1, q2 - q1 - 1);
      Token token{};
      if (parse_token(value, token)) {
        config.tokens.push_back(TokenEntry{group, token, false});
        ++added;
      }
      p = q2 + 1;
    }
  }
  return added;
}

bool file_readable(const std::string& path) {
  std::ifstream in(path);
  return static_cast<bool>(in);
}

} // namespace

void print_usage(const char* program) {
  std::printf("usage: %s [options] [paint-token]\n", program);
  std::puts("  --token / --token-paint <uuid>   绘制组 token（可重复；也可用 IKAPB_TOKEN）");
  std::puts("  --token-repair <uuid>            修复组 token（可重复）");
  std::puts("  --token-sonar <uuid>             声纳组 token（可重复）");
  std::puts("  --draw x,y,RRGGBB                目标图上一个像素（可重复）");
  std::puts("  --ppm <file.ppm> / --image <file> 目标图（PPM/PNG/JPEG）");
  std::puts("  --tokens <file.json>             从 JSON 读三组 token，默认自动读 ./tokens.json");
  std::puts("  --offset x,y                     目标图左上角偏移");
  std::puts("  --limit N                        所有组累计 OK 数达到 N 后退出");
  std::puts("  --dry-run                        只绑定并打印第一帧 TICK");
  std::puts("  --daemon                         常驻");
  std::puts("  --exec-ms N                      工作循环轮询周期");
  std::puts("  --paint-ms N / --repair-ms N / --sonar-ms N  各组补货周期");
  std::puts("  --paint-loop N                   整图完成后等 N 秒再重画（0=关闭）");
  std::puts("  --repair-thresh N                声纳与目标 RGB 曼哈顿差阈值");
  std::puts("  --repair-verify-ms N             声纳证据过期毫秒数（0=关闭）");
  std::puts("  --repair-follow N                常驻声纳网格点的 repair 槽位数");
  std::puts("  --sonar-stride N                 声纳采样步长（0=自动）");
  std::puts("  --round-cap N / --queue-cap N    生产/队列上限");
  std::puts("  --observe                        同时跑 public=1 观察连接");
  std::puts("  --sonar-out <file.ppm>           退出时导出声纳表");
  std::puts("  --board-out <file.ppm>           退出时导出公共画面");
}

CliResult parse_cli(int argc, char** argv) {
  CliResult result;
  Config& config = result.config;

  std::string tokens_path;
  bool tokens_path_set = false;

  auto need = [&](int& i, const char* option) -> const char* {
    if (i + 1 >= argc) {
      result.error = std::string(option) + " needs an argument";
      return nullptr;
    }
    return argv[++i];
  };

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (!std::strcmp(arg, "--help") || !std::strcmp(arg, "-h")) {
      result.help = true;
      result.ok = true;
      return result;
    }
    if (!std::strcmp(arg, "--token") || !std::strcmp(arg, "--token-paint")) {
      const char* value = need(i, arg);
      if (!value) return result;
      if (!add_token(config, Group::Paint, value)) {
        result.error = "bad token";
        return result;
      }
    } else if (!std::strcmp(arg, "--token-repair")) {
      const char* value = need(i, arg);
      if (!value) return result;
      if (!add_token(config, Group::Repair, value)) {
        result.error = "bad token";
        return result;
      }
    } else if (!std::strcmp(arg, "--token-sonar")) {
      const char* value = need(i, arg);
      if (!value) return result;
      if (!add_token(config, Group::Sonar, value)) {
        result.error = "bad token";
        return result;
      }
    } else if (!std::strcmp(arg, "--draw")) {
      const char* value = need(i, arg);
      if (!value) return result;
      unsigned x = 0, y = 0;
      char color[32] = {};
      if (std::sscanf(value, "%u,%u,%31s", &x, &y, color) != 3) {
        result.error = "bad --draw (want x,y,RRGGBB)";
        return result;
      }
      std::uint32_t parsed = 0;
      if (!parse_color(color, parsed)) {
        result.error = "bad --draw color";
        return result;
      }
      config.draws.push_back(Draw{x, y, parsed});
    } else if (!std::strcmp(arg, "--ppm") || !std::strcmp(arg, "--image")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.image_path = value;
    } else if (!std::strcmp(arg, "--tokens")) {
      const char* value = need(i, arg);
      if (!value) return result;
      tokens_path = value;
      tokens_path_set = true;
    } else if (!std::strcmp(arg, "--offset")) {
      const char* value = need(i, arg);
      if (!value) return result;
      int ox = 0, oy = 0;
      if (std::sscanf(value, "%d,%d", &ox, &oy) != 2) {
        result.error = "bad --offset (want x,y)";
        return result;
      }
      config.offset_x = ox;
      config.offset_y = oy;
    } else if (!std::strcmp(arg, "--limit")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.limit = std::strtoull(value, nullptr, 10);
    } else if (!std::strcmp(arg, "--dry-run")) {
      config.dry_run = true;
    } else if (!std::strcmp(arg, "--daemon")) {
      config.daemon = true;
    } else if (!std::strcmp(arg, "--observe")) {
      config.observe = true;
    } else if (!std::strcmp(arg, "--exec-ms")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.exec_ms = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--early-us")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.early_us = std::strtoull(value, nullptr, 10);
    } else if (!std::strcmp(arg, "--paint-ms")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.paint_ms = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--paint-loop")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.paint_loop_ms =
          static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10) * 1000u);
    } else if (!std::strcmp(arg, "--repair-ms")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.repair_ms = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--sonar-ms")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.sonar_ms = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--repair-thresh")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.repair_thresh = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--repair-verify-ms")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.repair_verify_ms = std::strtoull(value, nullptr, 10);
    } else if (!std::strcmp(arg, "--repair-follow")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.repair_follow = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--sonar-stride")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.sonar_stride = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--round-cap")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.round_cap = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--queue-cap")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.queue_cap = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
    } else if (!std::strcmp(arg, "--sonar-out")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.sonar_out = value;
    } else if (!std::strcmp(arg, "--board-out")) {
      const char* value = need(i, arg);
      if (!value) return result;
      config.board_out = value;
    } else if (arg[0] != '-') {
      if (!add_token(config, Group::Paint, arg)) {
        result.error = std::string("bad token: ") + arg;
        return result;
      }
    } else {
      result.error = std::string("unknown option ") + arg;
      return result;
    }
  }

  if (!tokens_path_set && !config.observe && file_readable("tokens.json"))
    tokens_path = "tokens.json";
  if (!tokens_path.empty()) {
    const std::size_t added = load_token_file(config, tokens_path);
    (void)added;
  }

  if (config.tokens.empty()) {
    if (const char* env = std::getenv("IKAPB_TOKEN"); env && *env) {
      if (!add_token(config, Group::Paint, env)) {
        result.error = "bad IKAPB_TOKEN";
        return result;
      }
    }
  }

  if (config.tokens.empty() && !config.observe)
    config.observe = true;

  // repair 组前 N 个槽位常驻声纳网格点。
  std::uint32_t left = config.repair_follow;
  for (TokenEntry& entry : config.tokens) {
    if (entry.group != Group::Repair)
      continue;
    if (left == 0)
      break;
    entry.follow = true;
    --left;
  }

  result.ok = true;
  return result;
}

} // namespace paintboard
