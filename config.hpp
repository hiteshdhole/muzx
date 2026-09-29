#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <string>

class config {
public:
  std::string get_music_folder();
  bool save_music_folder(const std::string &folder);

private:
  std::string get_config_file();
};

#endif
