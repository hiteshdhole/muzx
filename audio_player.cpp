#include "audio_player.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string shellEscape(const std::string &input) {
  std::string out = "'";
  for (char c : input) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

bool convertToCleanWav(const std::string &inputPath,
                       const std::string &outputWav) {
  std::error_code ec;
  std::filesystem::remove(outputWav, ec);

  std::string cmd = "ffmpeg -y -loglevel quiet -i " + shellEscape(inputPath) +
                    " -vn -acodec pcm_s16le -ar 44100 -ac 2 " +
                    shellEscape(outputWav) + " >/dev/null 2>&1";
  int ret = std::system(cmd.c_str());
  return (ret == 0 && std::filesystem::exists(outputWav, ec) &&
          std::filesystem::file_size(outputWav, ec) > 4096);
}

// Opens `path` with SFML's decoder inside a short-lived CHILD process and
// reports whether that was safe.
//
// Why: for some mp3 files SFML's mp3 reader corrupts the heap while opening
// ("double free or corruption" -> abort). That kills the whole process and
// cannot be caught with try/catch. In a child process the same crash only
// kills the child, so the player can see it coming and use the ffmpeg
// fallback for that file instead.
//   true  = the child opened the file (or cleanly reported "cannot open")
//   false = the child crashed or hung, so this file is skipped
bool sfmlCanOpenSafely(const std::string &path) {
  std::cout.flush();
  const pid_t pid = fork();
  if (pid < 0)
    return true; // cannot probe: behave like before

  if (pid == 0) {
    // ---- child ----
    rlimit noCore{0, 0};
    setrlimit(RLIMIT_CORE, &noCore); // a crash here must not dump core
    const int devNull = open("/dev/null", O_WRONLY);
    if (devNull >= 0) {
      dup2(devNull, STDOUT_FILENO);
      dup2(devNull, STDERR_FILENO);
    }
    signal(SIGALRM, SIG_DFL);
    alarm(5); // a hang is treated like a crash
    sf::InputSoundFile file;
    const bool ok = file.openFromFile(path);
    _exit(ok ? 0 : 3);
  }

  // ---- parent ----
  int status = 0;
  pid_t r;
  do {
    r = waitpid(pid, &status, 0);
  } while (r < 0 && errno == EINTR);
  if (r < 0)
    return true;
  return WIFEXITED(status) &&
         (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 3);
}

} // namespace

void audio_player::VisualMusic::onSeek(sf::Time timeOffset) {
  sf::Music::onSeek(timeOffset);
  const unsigned int rate = std::max(1U, getSampleRate());
  std::lock_guard<std::mutex> lock(ringMutex);
  ringEndFrame = std::max<std::int64_t>(
      0, static_cast<std::int64_t>(timeOffset.asSeconds() *
                                   static_cast<float>(rate)));
  ++seekEpoch;
}

bool audio_player::VisualMusic::onGetData(Chunk &data) {
  std::uint64_t epochBefore = 0;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    epochBefore = seekEpoch;
  }

  if (!sf::Music::onGetData(data)) {
    return false;
  }

  if (data.samples == nullptr || data.sampleCount == 0) {
    return true;
  }

  const unsigned int channels = std::max(1U, getChannelCount());
  const std::size_t frames = data.sampleCount / channels;

  std::lock_guard<std::mutex> lock(ringMutex);
  if (seekEpoch != epochBefore) {
    // A seek happened while decoding; resync to current playing offset
    ringEndFrame = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(
               getPlayingOffset().asSeconds() *
               static_cast<float>(std::max(1U, getSampleRate()))));
  }

  for (std::size_t f = 0; f < frames; ++f) {
    float mono = 0.0F;
    for (unsigned int c = 0; c < channels; ++c) {
      mono += static_cast<float>(data.samples[f * channels + c]) / 32768.0F;
    }
    mono /= static_cast<float>(channels);

    const std::size_t idx = static_cast<std::size_t>(
        ringEndFrame % static_cast<std::int64_t>(kRingFrames));
    monoRing[idx] = mono;
    ++ringEndFrame;
  }

  return true;
}

std::array<float, audio_player::kBandCount>
audio_player::VisualMusic::sampleCurrentWindow() const {
  std::array<float, kBandCount> out{};
  const unsigned int rate = std::max(1U, getSampleRate());
  const float currentSec = getPlayingOffset().asSeconds();
  const std::int64_t playFrame =
      static_cast<std::int64_t>(currentSec * static_cast<float>(rate));

  constexpr std::size_t kWindow = 1024;
  std::array<float, kWindow> window{};

  {
    std::lock_guard<std::mutex> lock(ringMutex);
    if (ringEndFrame <= 0)
      return out;

    std::int64_t startFrame =
        playFrame - static_cast<std::int64_t>(kWindow / 2);
    if (startFrame + static_cast<std::int64_t>(kWindow) > ringEndFrame) {
      startFrame = ringEndFrame - static_cast<std::int64_t>(kWindow);
    }
    if (startFrame < 0)
      startFrame = 0;

    for (std::size_t i = 0; i < kWindow; ++i) {
      const std::size_t ringIdx =
          static_cast<std::size_t>((startFrame + static_cast<std::int64_t>(i)) %
                                   static_cast<std::int64_t>(kRingFrames));
      window[i] = monoRing[ringIdx];
    }
  }

  // 4-stage filter bank across the 1024-sample real-time window
  // Separates Sub-Bass, Bass, Low-Mids, High-Mids, and Treble at 60 FPS
  std::array<float, kBandCount> energy{};
  float lp1 = 0.0F;
  float lp2 = 0.0F;
  float lp3 = 0.0F;
  float lp4 = 0.0F;

  for (std::size_t i = 0; i < kWindow; ++i) {
    const float x = window[i];
    lp1 += 0.04F * (x - lp1); // Sub-bass (< 250 Hz)
    lp2 += 0.12F * (x - lp2); // Bass (250 - 750 Hz)
    lp3 += 0.28F * (x - lp3); // Mids (750 - 2200 Hz)
    lp4 += 0.55F * (x - lp4); // Upper-Mids (2200 - 5000 Hz)

    const float band0 = lp1;
    const float band1 = lp2 - lp1;
    const float band2 = lp3 - lp2;
    const float band3 = lp4 - lp3;
    const float band4 = x - lp4; // Treble / Hi-hats (> 5000 Hz)

    // Sub-divide into 16 visually distinct bars using time-slice & harmonic mix
    const std::size_t slice = (i * 4) / kWindow; // 0..3
    energy[0 + slice] += (band0 * 2.4F) * (band0 * 2.4F);
    energy[4 + slice] += (band1 * 2.8F) * (band1 * 2.8F);
    energy[8 + slice] += (band2 * 3.2F) * (band2 * 3.2F);
    energy[12 + slice] +=
        ((band3 * 2.6F + band4 * 3.4F)) * ((band3 * 2.6F + band4 * 3.4F));
  }

  constexpr float kSliceSamples = static_cast<float>(kWindow) / 4.0F;
  for (std::size_t b = 0; b < kBandCount; ++b) {
    const float rms = std::sqrt(energy[b] / kSliceSamples);
    // Smooth logarithmic curve so quiet and loud beats both look dynamic
    out[b] = std::clamp(std::pow(rms * 2.6F, 0.65F), 0.0F, 1.0F);
  }

  return out;
}

bool audio_player::load(const std::string &filename) {
  hasPendingSeek = false;
  pendingSeekTarget = 0.0F;
  song.stop();
  song.resetRing();

  static std::ostringstream nullStream;
  sf::err().rdbuf(nullStream.rdbuf());

  // Only let SFML's own decoder touch the file if a child process survived
  // opening it. A file that crashes (or hangs) the decoder is skipped: load()
  // returns false and the caller moves on to the next song.
  if (!sfmlCanOpenSafely(filename))
    return false;

  bool opened = false;
  try {
    opened = song.openFromFile(filename);
  } catch (...) {
    opened = false;
  }

  if (opened && song.getDuration().asSeconds() > 0.1F) {
    return true;
  }

  const std::string fallbackWav = "/tmp/muzx_fallback.wav";
  if (convertToCleanWav(filename, fallbackWav)) {
    try {
      if (song.openFromFile(fallbackWav) &&
          song.getDuration().asSeconds() > 0.1F) {
        return true;
      }
    } catch (...) {
    }
  }

  return false;
}

void audio_player::play() {
  hasPendingSeek = false;
  song.play();
}

void audio_player::pause() {
  if (hasPendingSeek) {
    song.setPlayingOffset(sf::seconds(pendingSeekTarget));
    hasPendingSeek = false;
  }
  song.pause();
}

void audio_player::resume() { song.play(); }

void audio_player::stop() {
  hasPendingSeek = false;
  song.stop();
  song.resetRing();
}

void audio_player::update() {
  if (!hasPendingSeek)
    return;

  auto now = std::chrono::steady_clock::now();
  auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeekInput)
          .count();

  if (elapsedMs >= 90) {
    song.setPlayingOffset(sf::seconds(pendingSeekTarget));
    hasPendingSeek = false;
  }
}

bool audio_player::isFinished() const {
  if (hasPendingSeek)
    return false;
  return song.getStatus() == sf::Music::Status::Stopped;
}

sf::Time audio_player::getPlayingOffset() const {
  if (hasPendingSeek)
    return sf::seconds(pendingSeekTarget);
  return song.getPlayingOffset();
}

sf::Time audio_player::getDuration() const { return song.getDuration(); }

void audio_player::increaseVolume() {
  float volume = song.getVolume();
  if (volume < 100.0F) {
    volume = std::min(100.0F, volume + 5.0F);
    song.setVolume(volume);
  }
}

void audio_player::decreaseVolume() {
  float volume = song.getVolume();
  if (volume > 0.0F) {
    volume = std::max(0.0F, volume - 5.0F);
    song.setVolume(volume);
  }
}

float audio_player::getVolume() const { return song.getVolume(); }

void audio_player::seekForward(float seconds) {
  const float duration = song.getDuration().asSeconds();
  if (duration <= 0.0F || std::isnan(duration) || std::isinf(duration))
    return;

  const float current =
      hasPendingSeek ? pendingSeekTarget : song.getPlayingOffset().asSeconds();
  const float maxTarget = std::max(0.0F, duration - 0.3F);
  pendingSeekTarget =
      std::clamp(current + std::max(0.0F, seconds), 0.0F, maxTarget);
  hasPendingSeek = true;
  lastSeekInput = std::chrono::steady_clock::now();
}

void audio_player::seekBackward(float seconds) {
  const float duration = song.getDuration().asSeconds();
  if (duration <= 0.0F || std::isnan(duration) || std::isinf(duration))
    return;

  const float current =
      hasPendingSeek ? pendingSeekTarget : song.getPlayingOffset().asSeconds();
  pendingSeekTarget = std::max(0.0F, current - std::max(0.0F, seconds));
  hasPendingSeek = true;
  lastSeekInput = std::chrono::steady_clock::now();
}

std::array<float, audio_player::kBandCount>
audio_player::getSpectrumBands() const {
  if (song.getStatus() != sf::Music::Status::Playing) {
    return {};
  }
  auto bands = song.sampleCurrentWindow();
  const float volScale = std::clamp(song.getVolume() / 100.0F, 0.1F, 1.0F);
  for (float &b : bands) {
    b = std::clamp(b * volScale, 0.0F, 1.0F);
  }
  return bands;
}
