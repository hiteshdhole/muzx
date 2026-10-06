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

void audio_player::resume() { song.play(); }

void audio_player::stop() { song.stop(); }

bool audio_player::isFinished() const {
  return song.getStatus() == sf::Music::Status::Stopped;
}

sf::Time audio_player::getPlayingOffset() const {
  return song.getPlayingOffset();
}

sf::Time audio_player::getDuration() const { return song.getDuration(); }

void audio_player::increaseVolume() {
  float volume = song.getVolume();

  if (volume < 100.0f) {
    volume += 10.0f;

    if (volume > 100.0f)
      volume = 100.0f;

    song.setVolume(volume);
  }
}

void audio_player::decreaseVolume() {
  float volume = song.getVolume();

  if (volume > 0.0f) {
    volume -= 10.0f;

    if (volume < 0.0f)
      volume = 0.0f;

    song.setVolume(volume);
  }
}
float audio_player::getVolume() const { return song.getVolume(); }
