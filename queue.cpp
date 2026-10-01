#include "queue.hpp"

void queue::add(const std::string &song) { songs.push_back(song); }

bool queue::empty() const { return songs.empty(); }

std::size_t queue::size() const { return songs.size(); }

std::string queue::front() const { return songs.front(); }

void queue::remove() {
  if (!songs.empty()) {
    songs.erase(songs.begin());
  }
}
