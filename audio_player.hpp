#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

#include <SFML/Audio.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class audio_player {
public:
  static constexpr std::size_t kBandCount = 16;

private:
  class VisualMusic : public sf::Music {
  public:
    static constexpr std::size_t kRingFrames = 131072; // ~3 seconds at 44.1kHz
    mutable std::mutex ringMutex;
    std::vector<float> monoRing;
    std::int64_t ringEndFrame = 0;
    std::uint64_t seekEpoch = 0;

    VisualMusic() : monoRing(kRingFrames, 0.0F) {}

    void resetRing() {
      std::lock_guard<std::mutex> lock(ringMutex);
      std::fill(monoRing.begin(), monoRing.end(), 0.0F);
      ringEndFrame = 0;
      ++seekEpoch;
    }

    std::array<float, kBandCount> sampleCurrentWindow() const;

  protected:
    bool onGetData(Chunk &data) override;
    void onSeek(sf::Time timeOffset) override;
  };

  VisualMusic song;
  bool hasPendingSeek = false;
  float pendingSeekTarget = 0.0F;
  std::chrono::steady_clock::time_point lastSeekInput{};

public:
  bool load(const std::string &filename);

  void play();
  void pause();
  void resume();
  void stop();

  void update();

  bool isFinished() const;
  sf::Time getPlayingOffset() const;
  sf::Time getDuration() const;

  void increaseVolume();
  void decreaseVolume();

  void seekForward(float seconds = 2.5F);
  void seekBackward(float seconds = 2.5F);

  float getVolume() const;

  std::array<float, kBandCount> getSpectrumBands() const;
};

#endif
