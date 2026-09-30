#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

std::string config::get_config_file() {
  const char *home = std::getenv("HOME");

  if (home == nullptr) {
    return "";
  }

  std::filesystem::path config_directory =
      std::filesystem::path(home) / ".config" / "muzx";

  std::filesystem::create_directories(config_directory);

  return (config_directory / "config").string();
}

std::string config::get_music_folder() {
  std::string config_file = get_config_file();

  if (config_file.empty()) {
    return "";
  }

  std::ifstream file(config_file);

  if (!file) {
    return "";
  }

  std::string folder;
  std::getline(file, folder);

  return folder;
}

bool config::save_music_folder(const std::string &folder) {
  std::string config_file = get_config_file();

  if (config_file.empty()) {
    return false;
  }

  std::ofstream file(config_file);

  if (!file) {
    return false;
  }

  file << folder << '\n';

  return true;
}
