#ifndef SELECTION_HPP
#define SELECTION_HPP

#include <cstddef>

class selection {
private:
  std::size_t selectedIndex;

public:
  selection();

  void moveUp(std::size_t itemCount);
  void moveDown(std::size_t itemCount);

  std::size_t current() const;
};

#endif
