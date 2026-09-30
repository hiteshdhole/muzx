#ifndef PLAYLIST_HPP
#define PLAYLIST_HPP

#include <string>
#include <vector>

class playlist {
private:
  std::vector<std::string> songs;
  std::size_t currentSong;
  bool shuffleMode;

public:
  playlist();
  std::string songName(std::size_t index) const;
  bool load(const std::string &folder);
  std::size_t currentIndex() const;
  bool empty() const;

  std::string current() const;
  std::string currentName() const;
  bool next();
  bool previous();

  void shuffle();
  bool toggleShuffle();

  const std::vector<std::string> &getSongs() const;
};

#endif
