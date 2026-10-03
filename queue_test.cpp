#include "queue.hpp"

#include <iostream>

int main() {
  queue musicQueue;

  musicQueue.add("song1.mp3");
  musicQueue.add("song2.mp3");
  musicQueue.add("song3.mp3");

  std::cout << "Queue size: " << musicQueue.size() << '\n';

  std::cout << "First song: " << musicQueue.front() << '\n';

  musicQueue.remove();

  std::cout << "After remove:\n";

  std::cout << "Queue size: " << musicQueue.size() << '\n';

  std::cout << "First song: " << musicQueue.front() << '\n';

  return 0;
}
