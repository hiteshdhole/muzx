#pragma once
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

namespace lcdtext {

inline std::size_t decode(const std::string &s, std::size_t i, unsigned &cp) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  std::size_t n = c < 0x80           ? 1
                  : (c >> 5) == 6    ? 2
                  : (c >> 4) == 14   ? 3
                  : (c >> 3) == 30   ? 4
                                     : 1;
  if (i + n > s.size())
    n = 1;
  if (n == 1) {
    cp = c < 0x80 ? c : 0xFFFD;
    return 1;
  }
  cp = c & (0xFF >> (n + 1));
  for (std::size_t k = 1; k < n; ++k)
    cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
  return n;
}

inline std::string kakasiRomaji(const std::string &in) {
  bool hasNonAscii = false;
  for (unsigned char c : in) {
    if (c >= 0x80) {
      hasNonAscii = true;
      break;
    }
  }
  if (!hasNonAscii)
    return in;

  std::string safe;
  safe.reserve(in.size());
  for (char c : in) {
    if (c == '\'')
      safe += "'\\''";
    else if (c != '\n' && c != '\r')
      safe += c;
  }

  std::string cmd = "printf '%s' '" + safe +
                    "' | kakasi -i utf8 -o utf8 -Ja -Ha -Ka -Ea -s 2>/dev/null";
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (!pipe)
    return in;

  char buf[512];
  std::string out;
  while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
    out += buf;
  }
  ::pclose(pipe);

  while (!out.empty() &&
         (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    out.pop_back();

  return out.empty() ? in : out;
}

inline const char *const kRomaji[86] = {
    "a",   "a",  "i",   "i",  "u",  "u",  "e",  "e",  "o",  "o",  "ka",
    "ga",  "ki", "gi",  "ku", "gu", "ke", "ge", "ko", "go", "sa", "za",
    "shi", "ji", "su",  "zu", "se", "ze", "so", "zo", "ta", "da", "chi",
    "ji",  "",   "tsu", "zu", "te", "de", "to", "do", "na", "ni", "nu",
    "ne",  "no", "ha",  "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu",
    "he",  "be", "pe",  "ho", "bo", "po", "ma", "mi", "mu", "me", "mo",
    "",    "ya", "",    "yu", "",   "yo", "ra", "ri", "ru", "re", "ro",
    "wa",  "wa", "i",   "e",  "o",  "n",  "vu", "ka", "ke"};

inline const unsigned char kKana[86][2] = {
    {0xA7, 0}, {0xB1, 0}, {0xA8, 0}, {0xB2, 0}, {0xA9, 0}, {0xB3, 0}, {0xAA, 0},
    {0xB4, 0}, {0xAB, 0}, {0xB5, 0}, {0xB6, 0}, {0xB6, 1}, {0xB7, 0}, {0xB7, 1},
    {0xB8, 0}, {0xB8, 1}, {0xB9, 0}, {0xB9, 1}, {0xBA, 0}, {0xBA, 1}, {0xBB, 0},
    {0xBB, 1}, {0xBC, 0}, {0xBC, 1}, {0xBD, 0}, {0xBD, 1}, {0xBE, 0}, {0xBE, 1},
    {0xBF, 0}, {0xBF, 1}, {0xC0, 0}, {0xC0, 1}, {0xC1, 0}, {0xC1, 1}, {0xAF, 0},
    {0xC2, 0}, {0xC2, 1}, {0xC3, 0}, {0xC3, 1}, {0xC4, 0}, {0xC4, 1}, {0xC5, 0},
    {0xC6, 0}, {0xC7, 0}, {0xC8, 0}, {0xC9, 0}, {0xCA, 0}, {0xCA, 1}, {0xCA, 2},
    {0xCB, 0}, {0xCB, 1}, {0xCB, 2}, {0xCC, 0}, {0xCC, 1}, {0xCC, 2}, {0xCD, 0},
    {0xCD, 1}, {0xCD, 2}, {0xCE, 0}, {0xCE, 1}, {0xCE, 2}, {0xCF, 0}, {0xD0, 0},
    {0xD1, 0}, {0xD2, 0}, {0xD3, 0}, {0xAC, 0}, {0xD4, 0}, {0xAD, 0}, {0xD5, 0},
    {0xAE, 0}, {0xD6, 0}, {0xD7, 0}, {0xD8, 0}, {0xD9, 0}, {0xDA, 0}, {0xDB, 0},
    {0xDC, 0}, {0xDC, 0}, {0xB2, 0}, {0xB4, 0}, {0xA6, 0}, {0xDD, 0}, {0xB3, 1},
    {0xB6, 0}, {0xB9, 0}};

inline bool isVowel(char c) {
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

inline std::string toLcd(const std::string &rawIn, bool kanaMode) {
  const std::string in = kanaMode ? rawIn : kakasiRomaji(rawIn);
  std::string out;
  bool doubleNext = false;
  for (std::size_t i = 0; i < in.size();) {
    unsigned cp = 0;
    i += decode(in, i, cp);
    if (cp >= 0xFF01 && cp <= 0xFF5E)
      cp -= 0xFEE0;
    if (cp >= 0x3041 && cp <= 0x3096)
      cp += 0x60;
    if (cp == 0x3000)
      cp = ' ';
    if (cp < 0x80) {
      if (cp >= 32 && cp < 127) {
        out += static_cast<char>(cp);
        doubleNext = false;
      }
      continue;
    }
    if (kanaMode) {
      if (cp >= 0xFF61 && cp <= 0xFF9F) {
        out += static_cast<char>(cp - 0xFF61 + 0xA1);
        continue;
      }
      if (cp >= 0x30A1 && cp <= 0x30F6) {
        const unsigned char *k = kKana[cp - 0x30A1];
        out += static_cast<char>(k[0]);
        if (k[1] == 1)
          out += static_cast<char>(0xDE);
        if (k[1] == 2)
          out += static_cast<char>(0xDF);
        continue;
      }
      switch (cp) {
      case 0x3001:
        out += static_cast<char>(0xA4);
        continue;
      case 0x3002:
        out += static_cast<char>(0xA1);
        continue;
      case 0x300C:
        out += static_cast<char>(0xA2);
        continue;
      case 0x300D:
        out += static_cast<char>(0xA3);
        continue;
      case 0x30FB:
        out += static_cast<char>(0xA5);
        continue;
      case 0x30FC:
        out += static_cast<char>(0xB0);
        continue;
      default:
        out += '?';
        continue;
      }
    }
    if (cp >= 0x30A1 && cp <= 0x30F6) {
      const unsigned idx = cp - 0x30A1;
      if (idx == 34) {
        doubleNext = true;
        continue;
      }
      if (idx == 66 || idx == 68 || idx == 70) {
        const char v = idx == 66 ? 'a' : idx == 68 ? 'u' : 'o';
        if (!out.empty() && out.back() == 'i') {
          out.pop_back();
          const bool soft =
              out.size() >= 2 && ((out.compare(out.size() - 2, 2, "sh") == 0) ||
                                  (out.compare(out.size() - 2, 2, "ch") == 0));
          if (!(soft || (!out.empty() && out.back() == 'j')))
            out += 'y';
        } else
          out += 'y';
        out += v;
        continue;
      }
      std::string r = kRomaji[idx];
      if (idx == 0 || idx == 2 || idx == 4 || idx == 6 || idx == 8) {
        if (!out.empty() && isVowel(out.back()))
          out.pop_back();
      }
      if (doubleNext && !r.empty() && !isVowel(r[0]))
        out += (r[0] == 'c' ? 't' : r[0]);
      doubleNext = false;
      out += r;
      continue;
    }
    switch (cp) {
    case 0x3001:
      out += ',';
      break;
    case 0x3002:
      out += '.';
      break;
    case 0x300C:
    case 0x300D:
      out += '"';
      break;
    case 0x30FB:
      out += ' ';
      break;
    case 0x30FC:
      break;
    default:
      out += '?';
      break;
    }
  }
  return out;
}

} // namespace lcdtext

class LcdLink {
public:
  ~LcdLink() { close_port(); }

  bool enabled() const { return enabled_; }
  bool connected() const { return fd_ >= 0; }
  const std::string &port() const { return port_; }

  void toggle() {
    enabled_ = !enabled_;
    if (!enabled_)
      close_port();
    last_.clear();
    cachedOut_.clear();
    nextTry_ = {};
  }

  void show(const std::string &text) {
    if (!enabled_)
      return;
    const auto now = std::chrono::steady_clock::now();

    if (fd_ >= 0 && now >= nextCheck_) {
      nextCheck_ = now + std::chrono::milliseconds(250);
      pollfd pfd{fd_, POLLOUT, 0};
      struct stat stPath{}, stFd{};
      int mstat = 0;
      const bool dead =
          (::poll(&pfd, 1, 0) < 0) ||
          ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) ||
          (::stat(port_.c_str(), &stPath) != 0) ||
          (::fstat(fd_, &stFd) != 0) ||
          (stPath.st_ino != stFd.st_ino) ||
          (::ioctl(fd_, TIOCMGET, &mstat) < 0);
      if (dead) {
        close_port();
        nextTry_ = now + std::chrono::milliseconds(500);
        return;
      }
    }

    if (fd_ < 0) {
      if (now < nextTry_)
        return;
      nextTry_ = now + std::chrono::milliseconds(1000);
      if (!open_port())
        return;
      openedAt_ = now;
      last_.clear();
    }

    if (now - openedAt_ < std::chrono::milliseconds(2400))
      return;

    if (text != last_) {
      cachedOut_ = lcdtext::toLcd(text, kanaMode_) + "\n";
      last_ = text;
    } else if (now - lastWrite_ < std::chrono::milliseconds(2500)) {
      return;
    }

    if (cachedOut_.empty())
      return;

    const ssize_t n = ::write(fd_, cachedOut_.data(), cachedOut_.size());
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR)
        return;
      close_port();
      nextTry_ = now + std::chrono::milliseconds(500);
      return;
    }
    lastWrite_ = now;
  }

private:
  bool open_port() {
    const char *env = std::getenv("MUZX_LCD_PORT");
    const std::string candidates[] = {
        env ? env : "",  "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2",
        "/dev/ttyACM0",  "/dev/ttyACM1", "/dev/ttyACM2"};
    for (const std::string &p : candidates) {
      if (p.empty())
        continue;
      const int fd = ::open(p.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
      if (fd < 0)
        continue;
      termios tio{};
      if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        continue;
      }
      cfmakeraw(&tio);
      cfsetospeed(&tio, B115200);
      cfsetispeed(&tio, B115200);
      tio.c_cflag |= CLOCAL | CREAD | HUPCL;
      tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS);
      if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        continue;
      }
      int status = TIOCM_DTR | TIOCM_RTS;
      ::ioctl(fd, TIOCMBIS, &status);
      tcflush(fd, TCIOFLUSH);
      fd_ = fd;
      port_ = p;
      return true;
    }
    return false;
  }

  void close_port() {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
    last_.clear();
  }

  bool kanaMode_ = std::getenv("MUZX_LCD_KANA") != nullptr;
  bool enabled_ = true;
  int fd_ = -1;
  std::string port_;
  std::string last_;
  std::string cachedOut_;
  std::chrono::steady_clock::time_point nextTry_{}, openedAt_{}, nextCheck_{},
      lastWrite_{};
};
