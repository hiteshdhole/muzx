#ifndef LIBRARY_HPP
#define LIBRARY_HPP

#include <string>
#include <vector>

class library {
private:
  std::vector<std::string> items;

public:
  bool load(const std::string &folder);

  bool empty() const;
  std::size_t size() const;

  std::string item(std::size_t index) const;
  std::string itemName(std::size_t index) const;
};

#endif
