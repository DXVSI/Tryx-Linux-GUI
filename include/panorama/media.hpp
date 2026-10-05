#pragma once

#include <string>

namespace panorama {

enum class MediaType { Unknown, Video, Gif, Image };

class Media {
 public:
  static constexpr const char* TMP_DIR = "/tmp/tryx-panorama/";

  static MediaType detect_type(const std::string& path);
  static std::string get_extension(const std::string& path);
  static std::string get_basename(const std::string& path);
  static std::string get_filename(const std::string& path);
  static std::string get_converted_name(const std::string& original);
  static bool convert_gif_to_mp4(const std::string& input,
                                 const std::string& output);
  static bool convert_to_mp4(const std::string& input,
                             const std::string& output);
  static bool needs_conversion(const std::string& path);
  static bool is_ffmpeg_available();

 private:
  static std::string quote_for_shell(const std::string& arg);
  static bool run_ffmpeg(const std::string& args);
  static std::string normalize_ext(const std::string& filepath);
};

}  // namespace panorama
