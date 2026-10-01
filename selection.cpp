#include "selection.hpp"

selection::selection() { selectedIndex = 0; }

void selection::moveUp(std::size_t itemCount) {
  if (itemCount == 0) {
    return;
  }

  if (selectedIndex > 0) {
    selectedIndex--;
  }
}

void selection::moveDown(std::size_t itemCount) {
  if (itemCount == 0) {
    return;
  }

  if (selectedIndex < itemCount - 1) {
    selectedIndex++;
  }
}

std::size_t selection::current() const { return selectedIndex; }
