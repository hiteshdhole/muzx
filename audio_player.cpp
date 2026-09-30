#include "audio_player.hpp"
#include <iostream>

bool audio_player::load(const std::string &filename) {
  if (!song.openFromFile(filename)) {
    std::cerr << "Error: Could not load music: " << filename << std::endl;

    return false;
  }

  return true;
}

void audio_player::play() { song.play(); }

void audio_player::pause() { song.pause(); }

void audio_player::stop() { song.stop(); }

bool audio_player::isFinished() const {
  return song.getStatus() == sf::Music::Status::Stopped;
}
