#include "config.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

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
int main() {
  config c;

  std::string folder = c.get_music_folder();

  if (folder.empty()) {
    std::cout << "No music folder saved.\n";

    std::cout << "Enter music folder: ";

    std::getline(std::cin, folder);

    if (c.save_music_folder(folder)) {
      std::cout << "Music folder saved.\n";
    } else {
      std::cout << "Failed to save music folder.\n";
    }
  } else {
    std::cout << "Saved music folder: " << folder << '\n';
  }

  return 0;
}
