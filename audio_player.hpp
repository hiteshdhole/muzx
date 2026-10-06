#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

#include <SFML/Audio.hpp>
#include <string>

class audio_player {
private:
  sf::Music song;

public:
  bool load(const std::string &filename);

  void play();
  void pause();
  void resume();
  void stop();

  bool isFinished() const;
  sf::Time getPlayingOffset() const;
  sf::Time getDuration() const;

  void increaseVolume();
  void decreaseVolume();

  float getVolume() const;
};

#endif
