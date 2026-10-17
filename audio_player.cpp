#include "audio_player.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

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

} // namespace

bool audio_player::load(const std::string &filename) {
  hasPendingSeek = false;
  pendingSeekTarget = 0.0F;
  song.stop();

  // Silence SFML's internal sf::err() stream so corrupt headers never print
  // onto the TUI
  static std::ostringstream nullStream;
  sf::err().rdbuf(nullStream.rdbuf());

  bool opened = false;
  try {
    opened = song.openFromFile(filename);
  } catch (...) {
    opened = false;
  }

  // If SFML opened it, verify duration is valid (> 0 seconds)
  if (opened && song.getDuration().asSeconds() > 0.1F) {
    return true;
  }

  // Fallback: Song is AAC/M4A/Opus disguised as .mp3 or has corrupt ID3 tags.
  // Transcode via ffmpeg to a clean PCM WAV so MUZX never fails or crashes.
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
}

void audio_player::update() {
  if (!hasPendingSeek)
    return;

  auto now = std::chrono::steady_clock::now();
  auto elapsedMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeekInput)
          .count();

  // Wait 110ms after the last Left/Right key repeat before committing hardware
  // seek
  if (elapsedMs >= 110) {
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
  // Keep 0.3s margin from the end so seeking forward doesn't accidentally stop
  // the track
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
