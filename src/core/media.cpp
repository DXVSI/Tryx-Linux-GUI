#include "panorama/media.hpp"

#include "panorama/process.hpp"

#include <array>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace panorama {

namespace {

const std::unordered_map<std::string, MediaType> kExtensionTable = {
    {".mp4", MediaType::Video},  {".webm", MediaType::Video},
    {".mkv", MediaType::Video},  {".avi", MediaType::Video},
    {".mov", MediaType::Video},  {".gif", MediaType::Gif},
    {".jpg", MediaType::Image},  {".jpeg", MediaType::Image},
    {".png", MediaType::Image},  {".bmp", MediaType::Image},
    {".webp", MediaType::Image},
};

const std::unordered_set<std::string> kConvertibleExtensions = {
    ".webm", ".mkv", ".avi", ".mov", ".gif",
};

constexpr const char* kScaleFilter =
    "scale=trunc(iw/2)*2:trunc(ih/2)*2";

}  // namespace

std::string Media::normalize_ext(const std::string& filepath) {
  std::string suffix = std::filesystem::path(filepath).extension().string();
  for (auto& ch : suffix) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return suffix;
}

std::string Media::get_extension(const std::string& path) {
  return normalize_ext(path);
}

std::string Media::get_basename(const std::string& path) {
  return std::filesystem::path(path).stem().string();
}

std::string Media::get_filename(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

MediaType Media::detect_type(const std::string& path) {
  auto suffix = normalize_ext(path);
  auto it = kExtensionTable.find(suffix);
  if (it != kExtensionTable.end()) {
    return it->second;
  }
  return MediaType::Unknown;
}

std::string Media::get_converted_name(const std::string& original) {
  return get_basename(original) + ".mp4";
}

bool Media::run_ffmpeg(const std::vector<std::string>& args) {
  std::vector<std::string> argv{"ffmpeg"};
  argv.insert(argv.end(), args.begin(), args.end());
  // ffmpeg runs without a shell, so file names reach it unchanged.
  auto result = run_process(argv, false);
  return result.has_value() && result->exit_code == 0;
}

bool Media::is_ffmpeg_available() {
  return run_ffmpeg({"-version"});
}

bool Media::convert_gif_to_mp4(const std::string& input,
                               const std::string& output) {
  std::filesystem::create_directories(TMP_DIR);

  if (!run_ffmpeg({"-y", "-i", input,
                   "-movflags", "faststart",
                   "-pix_fmt", "yuv420p",
                   "-vf", kScaleFilter,
                   output})) {
    return false;
  }
  return std::filesystem::exists(output);
}

bool Media::convert_to_mp4(const std::string& input,
                           const std::string& output) {
  std::filesystem::create_directories(TMP_DIR);

  if (!run_ffmpeg({"-y", "-i", input,
                   "-c:v", "libx264",
                   "-preset", "fast",
                   "-crf", "23",
                   "-movflags", "faststart",
                   "-pix_fmt", "yuv420p",
                   "-vf", kScaleFilter,
                   "-an",
                   output})) {
    return false;
  }
  return std::filesystem::exists(output);
}

bool Media::needs_conversion(const std::string& path) {
  auto suffix = normalize_ext(path);
  return kConvertibleExtensions.count(suffix) > 0;
}

}  // namespace panorama
