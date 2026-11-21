// Tests of the spectrum analyser with made-up sound.
#include "../spectrum.hpp"
#include "check.hpp"
#include <random>

using spectrum::Analyzer;
constexpr int kRate = Analyzer::kRate;
constexpr double kPi = 3.14159265358979;

// Feeds `seconds` of sound made by gen(sampleIndex) and returns the last picture.
template <class Gen> std::vector<float> run(Analyzer &an, double seconds, Gen gen, std::size_t start = 0) {
  const std::size_t n = static_cast<std::size_t>(seconds * kRate);
  std::vector<std::int16_t> buf(n);
  for (std::size_t i = 0; i < n; ++i)
    buf[i] = static_cast<std::int16_t>(std::lround(std::clamp(gen(start + i), -32768.0, 32767.0)));
  an.push(buf.data(), buf.size());
  return an.levels();
}

static double dbToAmp(double db) { return 32768.0 * std::pow(10.0, db / 20.0); }
static int argmax(const std::vector<float> &v) { return static_cast<int>(std::max_element(v.begin(), v.end()) - v.begin()); }
static int bandOf(double hz, int bands) { // which bar should light for this frequency
  return static_cast<int>(std::floor(bands * std::log(hz / 60.0) / std::log(7500.0 / 60.0)));
}

int main() {
  // silence stays silent
  {
    Analyzer an(16);
    const auto lv = run(an, 1.0, [](std::size_t) { return 0.0; });
    for (float l : lv)
      CHECK_NEAR(l, 0.0, 1e-6);
  }
  // a single tone lights the right bar (and not its far neighbours), at every volume
  for (double db : {-60.0, -35.0, -6.0}) {
    for (double hz : {100.0, 250.0, 700.0, 1500.0, 3000.0, 6000.0}) {
      Analyzer an(16);
      const double amp = dbToAmp(db);
      const auto lv = run(an, 1.5, [&](std::size_t i) { return amp * std::sin(2 * kPi * hz * static_cast<double>(i) / kRate); });
      const int want = bandOf(hz, 16);
      CHECK(std::abs(argmax(lv) - want) <= 1); // the middle of a bar can be between two
      CHECK(lv[static_cast<std::size_t>(argmax(lv))] > 0.9F); // auto-gain: full height at any volume
      int lit = 0;
      for (float l : lv)
        lit += l > 0.5F ? 1 : 0;
      CHECK(lit <= 3); // narrow, not smeared over the whole screen
    }
  }
  // quiet and loud versions of the same sound look the same
  {
    std::mt19937 rng(5);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<double> noise(static_cast<std::size_t>(kRate * 4));
    // pink-ish noise: white noise through a simple 1-pole low-pass sum
    double b0 = 0, b1 = 0, b2 = 0;
    for (double &x : noise) {
      const double w = nd(rng);
      b0 = 0.99765 * b0 + w * 0.0990460;
      b1 = 0.96300 * b1 + w * 0.2965164;
      b2 = 0.57000 * b2 + w * 1.0526913;
      x = b0 + b1 + b2 + w * 0.1848;
    }
    Analyzer loud(16), quiet(16);
    const auto a = run(loud, 4.0, [&](std::size_t i) { return noise[i] * dbToAmp(-18) / 2.0; });
    const auto b = run(quiet, 4.0, [&](std::size_t i) { return noise[i] * dbToAmp(-48) / 2.0; });
    for (std::size_t i = 0; i < a.size(); ++i)
      CHECK_NEAR(a[i], b[i], 0.25);
    // pink noise is spread over the whole screen: most bars are well above the floor
    int up = 0;
    for (float l : a)
      up += l > 0.3F ? 1 : 0;
    CHECK(up >= 13);
  }
  // a very quiet hiss does not fill the screen (the gate)
  {
    std::mt19937 rng(9);
    std::normal_distribution<double> nd(0.0, 1.0);
    Analyzer an(16);
    const auto lv = run(an, 1.0, [&](std::size_t) { return nd(rng) * 0.6; }); // about -95 dBFS
    for (float l : lv)
      CHECK(l < 0.05F);
  }
  // a kick drum (60 Hz bursts): the low bars jump, the high bars stay low, and it falls again
  {
    Analyzer an(16);
    auto kick = [&](std::size_t i) {
      const double t = static_cast<double>(i % (kRate / 2)) / kRate; // every 0.5 s
      return dbToAmp(-10) * std::sin(2 * kPi * 70.0 * t) * std::exp(-t * 18.0);
    };
    std::size_t at = 0;
    run(an, 2.0, kick, at);
    at += static_cast<std::size_t>(2.0 * kRate);
    const auto hit = run(an, 0.06, kick, at); // right after a hit
    at += static_cast<std::size_t>(0.06 * kRate);
    CHECK(hit[0] + hit[1] > 0.8F);
    CHECK(hit[12] + hit[13] + hit[14] + hit[15] < 0.4F);
    const auto later = run(an, 0.4, kick, at); // just before the next hit
    CHECK(later[0] < hit[0]);
  }
  // the bars react quickly to a change of sound
  {
    Analyzer an(16);
    run(an, 2.0, [](std::size_t i) { return dbToAmp(-20) * std::sin(2 * kPi * 400.0 * static_cast<double>(i) / kRate); });
    const auto lv = run(an, 0.2, [](std::size_t i) { return dbToAmp(-20) * std::sin(2 * kPi * 3000.0 * static_cast<double>(i) / kRate); }, 32000);
    CHECK(std::abs(argmax(lv) - bandOf(3000.0, 16)) <= 1);
  }
  // 20 columns work too
  {
    Analyzer an(20);
    const auto lv = run(an, 1.5, [](std::size_t i) { return dbToAmp(-20) * std::sin(2 * kPi * 1000.0 * static_cast<double>(i) / kRate); });
    CHECK_EQ(static_cast<int>(lv.size()), 20);
    CHECK(std::abs(argmax(lv) - bandOf(1000.0, 20)) <= 1);
  }
  TEST_MAIN_END();
}
