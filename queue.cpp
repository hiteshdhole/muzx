#include "queue.hpp"

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
