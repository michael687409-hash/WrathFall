#pragma once

#include <windows.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace asura::upscaler_events {

inline std::filesystem::path File(const std::filesystem::path& logs, DWORD pid, HANDLE process) {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (logs.empty() || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return {};
  const uint64_t start = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
  return logs / ("upscaler-" + std::to_string(pid) + "-" + std::to_string(start) + ".tsv");
}

inline int64_t NowMilliseconds() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

struct ShaderPair {
  uint64_t vertex = 0;
  uint64_t pixel = 0;
  bool operator==(const ShaderPair&) const = default;
};

struct Cause {
  std::string reason;
  ShaderPair draw, motion;
  std::string detail;
  bool operator==(const Cause&) const = default;
};

struct Record {
  bool end = false;
  uint64_t frame = 0;
  int64_t time = 0;
  std::string provider;
  Cause cause;
};

struct SceneEvidence {
  bool menu = false;
  bool recent_gameplay = false;
  bool motion_program = false;
  bool opaque_view = false;
  bool compositor = false;
  bool depth = false;
  bool camera = false;
  bool color = false;
  bool final_resolve = false;
  bool compatible_frontbuffer = false;
  std::string_view hud_failure;
};

inline std::string_view FallbackReason(const SceneEvidence& scene) {
  if (scene.menu || (!scene.recent_gameplay && !scene.motion_program) ||
      !(scene.depth || scene.camera || scene.opaque_view || scene.motion_program)) return {};
  if (!scene.hud_failure.empty()) return scene.hud_failure;
  if (!scene.compatible_frontbuffer) return "unsupported-frontbuffer";
  if (!scene.compositor) return "unrecognized-scene-compositor";
  if (!scene.depth) return "missing-scene-depth";
  if (!scene.camera) return "missing-camera-matrix";
  if (!scene.color) return "missing-scene-color";
  if (!scene.final_resolve) return "missing-final-color-resolve";
  return "upscaled-output-unavailable";
}

inline constexpr auto kColumns =
    "event\tframe\tunix_ms\tprovider\treason\tvertex_shader\tpixel_shader"
    "\tmotion_vertex_shader\tmotion_pixel_shader\tdetail";

inline void Write(std::ostream& output, bool end, uint64_t frame, std::string_view provider,
                  std::string_view reason = {}, ShaderPair draw = {}, ShaderPair motion = {},
                  std::string_view detail = {}) {
  output << (end ? "end" : "fallback") << '\t' << frame << '\t' << NowMilliseconds()
         << '\t' << provider << '\t' << reason << '\t' << std::hex << draw.vertex << '\t'
         << draw.pixel << '\t' << motion.vertex << '\t' << motion.pixel << std::dec << '\t';
  for (const char ch : detail.substr(0, 512)) {
    output << ((ch == '\t' || ch == '\r' || ch == '\n') ? ' ' : ch);
  }
  output << std::endl;
}

inline std::optional<Record> Parse(std::string_view line) {
  std::array<std::string_view, 10> fields{};
  for (size_t i = 0; i + 1 < fields.size(); ++i) {
    const auto tab = line.find('\t');
    if (tab == std::string_view::npos) return std::nullopt;
    fields[i] = line.substr(0, tab);
    line.remove_prefix(tab + 1);
  }
  fields.back() = line;
  if ((fields[0] != "fallback" && fields[0] != "end") ||
      (fields[3] != "dlss" && fields[3] != "fsr3") ||
      (fields[0] == "fallback" && fields[4].empty())) return std::nullopt;
  const auto number = [](std::string_view value, auto& target, int base) {
    const auto end = value.data() + value.size();
    const auto result = std::from_chars(value.data(), end, target, base);
    return result.ec == std::errc{} && result.ptr == end;
  };
  Record record;
  record.end = fields[0] == "end";
  record.provider = fields[3];
  record.cause.reason = fields[4];
  record.cause.detail = fields[9];
  if (!number(fields[1], record.frame, 10) || !number(fields[2], record.time, 10) ||
      !number(fields[5], record.cause.draw.vertex, 16) || !number(fields[6], record.cause.draw.pixel, 16) ||
      !number(fields[7], record.cause.motion.vertex, 16) ||
      !number(fields[8], record.cause.motion.pixel, 16) || record.time <= 0) return std::nullopt;
  return record;
}

}  // namespace asura::upscaler_events
