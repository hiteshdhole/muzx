#ifndef QUEUE_HPP
#define QUEUE_HPP

#include <string>
#include <vector>

class queue {
private:
  std::vector<std::string> songs;

public:
  void add(const std::string &song);

  bool empty() const;
  std::size_t size() const;

  std::string front() const;
  std::string song(std::size_t index) const;
  void remove(std::size_t index);
};

#endif
