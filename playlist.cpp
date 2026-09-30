#include "playlist.hpp"
#include <algorithm>
#include <filesystem>
#include <random>

playlist::playlist() {
  currentSong = 0;
  shuffleMode = false;
}

bool playlist::load(const std::string &folder) {
  songs.clear();
  currentSong = 0;

  for (const auto &file :
       std::filesystem::recursive_directory_iterator(folder)) {
    if (file.path().extension() == ".mp3") {
      songs.push_back(file.path().string());
    }
  }

  return !songs.empty();
}

bool playlist::empty() const { return songs.empty(); }

std::string playlist::current() const { return songs[currentSong]; }

bool playlist::next() {
  if (songs.empty()) {
    return false;
  }

  if (shuffleMode) {
    if (songs.size() == 1) {
      return true;
    }

    std::random_device rd;
    std::mt19937 generator(rd());

    std::uniform_int_distribution<std::size_t> distribution(0,
                                                            songs.size() - 2);

    std::size_t nextSong = distribution(generator);

    if (nextSong >= currentSong) {
      nextSong++;
    }

    currentSong = nextSong;

    return true;
  }

  if (currentSong < songs.size() - 1) {
    currentSong++;
    return true;
  }

  return false;
}

bool playlist::previous() {
  if (currentSong > 0) {
    currentSong--;
    return true;
  }

  return false;
}
void playlist::shuffle() {
  std::random_device rd;
  std::mt19937 generator(rd());

  std::shuffle(songs.begin(), songs.end(), generator);

  currentSong = 0;
}
bool playlist::toggleShuffle() {
  shuffleMode = !shuffleMode;

  return shuffleMode;
}

const std::vector<std::string> &playlist::getSongs() const { return songs; }
