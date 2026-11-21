// Spectrum analyser for the LCD hub: turns mono 16-bit sound into one bar
// height (0..1) per LCD column. No I/O, so it can be tested with made-up sound.
//
//  - 1024-point FFT at 16 kHz (15.6 Hz per bin) so the lowest bars are not
//    smeared into each other; a new picture every 256 samples (16 ms).
//  - bars are spaced evenly on a log scale from 60 Hz to 7.5 kHz.
//  - a gentle tilt lifts the highs (music has much less energy up there).
//  - automatic gain: the bars always use the full height, whether the volume
//    is low or high. The reference level follows the loudest band: it rises
//    quickly and falls slowly (about 4 dB per second).
//  - silence stays silent (a gate), so noise is not blown up to full height.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace spectrum {

inline void fftInPlace(std::vector<std::complex<float>> &a) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) { // bit reversal
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1)
      j ^= bit;
    j ^= bit;
    if (i < j)
      std::swap(a[i], a[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const float ang = -2.0F * 3.14159265358979F / static_cast<float>(len);
    const std::complex<float> wlen(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0F, 0.0F);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

class Analyzer {
public:
  static constexpr int kRate = 16000;
  static constexpr std::size_t kWindow = 1024;
  static constexpr std::size_t kHop = 256;

  explicit Analyzer(int bands) : bands_(std::max(4, bands)), levels_(static_cast<std::size_t>(bands_), 0.0F) {
    window_.resize(kWindow);
    for (std::size_t i = 0; i < kWindow; ++i)
      window_[i] = 0.5F - 0.5F * std::cos(2.0F * 3.14159265F * static_cast<float>(i) / static_cast<float>(kWindow - 1));
    ring_.assign(kWindow, 0);
    // bar edges and the (fractional) FFT bin at the middle of each bar
    const float binHz = static_cast<float>(kRate) / static_cast<float>(kWindow);
    for (int b = 0; b < bands_; ++b) {
      const float f0 = lowHz() * std::pow(highHz() / lowHz(), static_cast<float>(b) / static_cast<float>(bands_));
      const float f1 = lowHz() * std::pow(highHz() / lowHz(), static_cast<float>(b + 1) / static_cast<float>(bands_));
      Band band;
      band.i0 = std::clamp<std::size_t>(static_cast<std::size_t>(std::lround(f0 / binHz)), 1, kWindow / 2 - 1);
      band.i1 = std::clamp<std::size_t>(std::max(static_cast<std::size_t>(std::lround(f1 / binHz)), band.i0 + 1), 2, kWindow / 2);
      band.centre = std::sqrt(f0 * f1) / binHz;
      band.tiltDb = kTiltDbPerBand * static_cast<float>(b) * 16.0F / static_cast<float>(bands_);
      bands_vec_.push_back(band);
    }
  }

  int bands() const { return bands_; }
  const std::vector<float> &levels() const { return levels_; }

  // Feeds sound. Returns true when the picture (levels()) changed.
  bool push(const std::int16_t *samples, std::size_t count) {
    bool changed = false;
    for (std::size_t i = 0; i < count; ++i) {
      ring_[writePos_] = samples[i];
      writePos_ = (writePos_ + 1) % kWindow;
      if (filled_ < kWindow)
        ++filled_;
      if (++sinceHop_ >= kHop && filled_ >= kWindow) {
        sinceHop_ = 0;
        analyse();
        changed = true;
      }
    }
    return changed;
  }

  // Back to silence (when the mode is left).
  void reset() {
    std::fill(levels_.begin(), levels_.end(), 0.0F);
    refDb_ = kMinRefDb;
  }

private:
  struct Band {
    std::size_t i0 = 1, i1 = 2;
    float centre = 1.0F;
    float tiltDb = 0.0F;
  };

  static constexpr float lowHz() { return 60.0F; }
  static constexpr float highHz() { return 7500.0F; }
  static constexpr float kTiltDbPerBand = 1.1F; // lifts the highs
  static constexpr float kRangeDb = 40.0F;      // bar height range under the reference
  static constexpr float kMinRefDb = -70.0F;    // the reference never goes below this
  static constexpr float kGateDb = -85.0F;      // quieter than this is silence
  static constexpr float kFallPerHop = 0.05F;   // a bar can fall 5% per 16 ms
  static constexpr float kRefFallDbPerHop = 0.07F;

  void analyse() {
    std::vector<std::complex<float>> buf(kWindow);
    for (std::size_t i = 0; i < kWindow; ++i)
      buf[i] = std::complex<float>(static_cast<float>(ring_[(writePos_ + i) % kWindow]) * window_[i], 0.0F);
    fftInPlace(buf);
    // 0 dB = a full-scale sine: its peak bin has the magnitude 32768 * N / 4
    const float fullScaleDb = 20.0F * std::log10(32768.0F * static_cast<float>(kWindow) / 4.0F);
    std::vector<float> db(static_cast<std::size_t>(bands_), -200.0F);
    float peak = -200.0F;
    for (std::size_t b = 0; b < db.size(); ++b) {
      const Band &band = bands_vec_[b];
      float mag;
      if (band.i1 - band.i0 <= 1) { // narrower than one bin: read between the two nearest bins
        const std::size_t k = std::min(static_cast<std::size_t>(band.centre), kWindow / 2 - 1);
        const float fr = band.centre - static_cast<float>(k);
        mag = std::abs(buf[k]) * (1.0F - fr) + std::abs(buf[k + 1]) * fr;
      } else {
        mag = 0.0F;
        for (std::size_t i = band.i0; i < band.i1; ++i)
          mag = std::max(mag, std::abs(buf[i]));
      }
      db[b] = 20.0F * std::log10(mag + 1e-6F) - fullScaleDb + band.tiltDb;
      peak = std::max(peak, db[b]);
    }
    if (peak < kGateDb) { // silence: only let the bars fall
      for (float &l : levels_)
        l = std::max(0.0F, l - kFallPerHop);
      refDb_ = std::max(kMinRefDb, refDb_ - kRefFallDbPerHop);
      return;
    }
    // automatic gain: quick up, slow down
    if (peak > refDb_)
      refDb_ += 0.5F * (peak - refDb_);
    else
      refDb_ = std::max(kMinRefDb, refDb_ - kRefFallDbPerHop);
    for (std::size_t b = 0; b < db.size(); ++b) {
      const float now = std::clamp((db[b] - (refDb_ - kRangeDb)) / kRangeDb, 0.0F, 1.0F);
      levels_[b] = std::max(now, levels_[b] - kFallPerHop); // quick up, slow down
    }
  }

  int bands_;
  std::vector<float> levels_;
  std::vector<Band> bands_vec_;
  std::vector<float> window_;
  std::vector<std::int16_t> ring_;
  std::size_t writePos_ = 0, filled_ = 0, sinceHop_ = 0;
  float refDb_ = kMinRefDb;
};

} // namespace spectrum
