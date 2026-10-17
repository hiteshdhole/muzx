#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

#include <SFML/Audio.hpp>
#include <chrono>
#include <string>

class audio_player {
private:
  sf::Music song;
  bool hasPendingSeek = false;
  float pendingSeekTarget = 0.0F;
  std::chrono::steady_clock::time_point lastSeekInput{};

public:
  bool load(const std::string &filename);

  void play();
  void pause();
  void resume();
  void stop();

  // Commits debounced seek operations smoothly without stuttering SFML
  void update();

  bool isFinished() const;
  sf::Time getPlayingOffset() const;
  sf::Time getDuration() const;

  void increaseVolume();
  void decreaseVolume();

  void seekForward(float seconds = 2.5F);
  void seekBackward(float seconds = 2.5F);

  float getVolume() const;
};

#endif
