#include "library.hpp"
#include "selection.hpp"

#include <iostream>
#include <termios.h>
#include <unistd.h>

char getKey() {
  char key;
  read(STDIN_FILENO, &key, 1);
  return key;
}

void enableRawMode() {
  termios terminal;

  tcgetattr(STDIN_FILENO, &terminal);
  terminal.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
}

void disableRawMode() {
  termios terminal;

  tcgetattr(STDIN_FILENO, &terminal);
  terminal.c_lflag |= (ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
}

void showLibrary(const library &musicLibrary, const selection &cursor) {
  std::cout << "\033[2J\033[H";

  std::cout << "MUSIC LIBRARY\n\n";

  for (std::size_t i = 0; i < musicLibrary.size(); i++) {
    if (i == cursor.current()) {
      std::cout << "> ";
    } else {
      std::cout << "  ";
    }

    std::cout << musicLibrary.itemName(i) << '\n';
  }

  std::cout << "\n[j/k] Move   [q] Quit\n";
}

int main() {
  library musicLibrary;
  selection cursor;

  if (!musicLibrary.load("/home/hitesh/Hitesh/Music")) {
    std::cout << "No folders found\n";
    return 1;
  }

  enableRawMode();

  bool running = true;

  while (running) {
    showLibrary(musicLibrary, cursor);

    char key = getKey();

    if (key == 'j') {
      cursor.moveDown(musicLibrary.size());
    } else if (key == 'k') {
      cursor.moveUp(musicLibrary.size());
    } else if (key == 'q') {
      running = false;
    }
  }

  disableRawMode();

  return 0;
}
