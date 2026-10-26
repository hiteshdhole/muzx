// Standalone LCD check:  g++ -std=c++17 lcdtest.cpp -o lcdtest && ./lcdtest
// [/dev/ttyUSB0]
#include "lcd_link.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>

int main(int argc, char **argv) {
  const char *candidates[] = {argc > 1 ? argv[1] : "", "/dev/ttyUSB0",
                              "/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyUSB1"};
  int fd = -1;
  const char *used = nullptr;
  for (const char *p : candidates) {
    if (!*p)
      continue;
    fd = ::open(p, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0) {
      used = p;
      std::printf("opened %s\n", p);
      break;
    }
    std::printf("cannot open %s: %s\n", p, std::strerror(errno));
  }
  if (fd < 0) {
    std::puts("\nNo port could be opened.");
    std::puts("- 'No such file': the Uno is not detected (cable? charge-only "
              "cable?).");
    std::puts("- 'Permission denied': run  sudo chmod 666 /dev/ttyUSB0  (or "
              "add yourself to group uucp and log in again).");
    return 1;
  }
  termios tio{};
  tcgetattr(fd, &tio);
  cfmakeraw(&tio);
  cfsetospeed(&tio, B115200);
  cfsetispeed(&tio, B115200);
  tio.c_cflag |= CLOCAL | CREAD;
  tcsetattr(fd, TCSANOW, &tio);
  std::puts("waiting 3 s for the Uno to restart...");
  std::this_thread::sleep_for(std::chrono::seconds(3));
  const char *tests[] = {"Hello LCD from MUZX", "Line two test", "konnichiha"};
  for (const char *t : tests) {
    std::string line = lcdtext::toLcd(t, false) + "\n";
    ssize_t n = ::write(fd, line.data(), line.size());
    std::printf("sent \"%s\" (%zd bytes)%s\n", t, n,
                n < 0 ? "  <-- write failed" : "");
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  std::puts("\nDid the LCD change text 3 times?");
  std::puts("- yes: MUZX side is the problem, tell me.");
  std::puts("- screen stays blank/backlight only: wrong I2C address (try 0x3F) "
            "or contrast knob.");
  std::puts("- no backlight: wiring / power.");
  std::puts("- shows 'waiting...' but never changes: sketch/baud/port "
            "mismatch, or the IDE serial monitor is open.");
  ::close(fd);
  return 0;
}
