#include "library.hpp"

#include <filesystem>

bool library::load(const std::string &folder) {
  items.clear();

  for (const auto &entry : std::filesystem::directory_iterator(folder)) {
    if (entry.is_directory()) {
      items.push_back(entry.path().string());
    }
  }

  return !items.empty();
}

bool library::empty() const { return items.empty(); }

std::size_t library::size() const { return items.size(); }

std::string library::item(std::size_t index) const { return items[index]; }

std::string library::itemName(std::size_t index) const {
  return std::filesystem::path(items[index]).filename().string();
}
