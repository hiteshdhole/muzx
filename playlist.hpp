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

  bool load(const std::string &folder);

  bool empty() const;

  std::string current() const;

  bool next();
  bool previous();

  void shuffle();
  bool toggleShuffle();

  const std::vector<std::string> &getSongs() const;
};

#endif
