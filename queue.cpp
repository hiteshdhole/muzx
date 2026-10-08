#include "queue.hpp"
#include <fstream>

void queue::add(const std::string &song) { songs.push_back(song); }

bool queue::empty() const { return songs.empty(); }

std::size_t queue::size() const { return songs.size(); }

std::string queue::front() const { return songs.front(); }

std::string queue::song(std::size_t index) const { return songs[index]; }

void queue::remove(std::size_t index) {
  if (index < songs.size()) {
    songs.erase(songs.begin() + index);
  }
}

void queue::clear() { songs.clear(); }

void queue::save(const std::string &filename) const {
  std::ofstream file(filename);

  for (const std::string &songPath : songs) {
    file << songPath << '\n';
  }
}

void queue::load(const std::string &filename) {
  std::ifstream file(filename);

  if (!file) {
    return;
  }

  songs.clear();

  std::string songPath;

  while (std::getline(file, songPath)) {
    if (!songPath.empty()) {
      songs.push_back(songPath);
    }
  }
}
