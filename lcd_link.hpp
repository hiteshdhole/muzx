#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

namespace lcdtext {

inline std::size_t decode(const std::string &s, std::size_t i, unsigned &cp) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  std::size_t n = c < 0x80         ? 1
                  : (c >> 5) == 6  ? 2
                  : (c >> 4) == 14 ? 3
                  : (c >> 3) == 30 ? 4
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

inline bool hasJapanese(const std::string &in) {
  for (std::size_t i = 0; i < in.size();) {
    unsigned cp = 0;
    i += decode(in, i, cp);
    if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xFF66 && cp <= 0xFF9F))
      return true;
  }
  return false;
}

inline std::string kakasiRomaji(const std::string &in) {
  if (!hasJapanese(in))
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

// ===========================================================================
// Universal Brahmic Indic Engine:
// Covers Hindi, Marathi, Haryanvi, Bhojpuri (Devanagari: U+0900..097F),
// Bengali (U+0980..09FF), Punjabi (Gurmukhi: U+0A00..0A7F),
// plus Gujarati, Odia, Tamil, Telugu, Kannada, Malayalam (U+0A80..0D7F)
// ===========================================================================

inline bool isIndicCp(unsigned cp) { return cp >= 0x0900 && cp <= 0x0D7F; }

inline bool isIndicConsonantOffset(unsigned off, unsigned block,
                                   const char *&rom) {
  switch (off) {
  case 0x15:
    rom = "k";
    return true;
  case 0x16:
    rom = "kh";
    return true;
  case 0x17:
    rom = "g";
    return true;
  case 0x18:
    rom = "gh";
    return true;
  case 0x19:
    rom = "ng";
    return true;
  case 0x1A:
    rom = "ch";
    return true;
  case 0x1B:
    rom = "chh";
    return true;
  case 0x1C:
    rom = "j";
    return true;
  case 0x1D:
    rom = "jh";
    return true;
  case 0x1E:
    rom = "ny";
    return true;
  case 0x1F:
    rom = "t";
    return true;
  case 0x20:
    rom = "th";
    return true;
  case 0x21:
    rom = "d";
    return true;
  case 0x22:
    rom = "dh";
    return true;
  case 0x23:
    rom = "n";
    return true;
  case 0x24:
    rom = "t";
    return true;
  case 0x25:
    rom = "th";
    return true;
  case 0x26:
    rom = "d";
    return true;
  case 0x27:
    rom = "dh";
    return true;
  case 0x28:
    rom = "n";
    return true;
  case 0x29:
    rom = "n";
    return true;
  case 0x2A:
    rom = "p";
    return true;
  case 0x2B:
    rom = "ph";
    return true;
  case 0x2C:
    rom = "b";
    return true;
  case 0x2D:
    rom = "bh";
    return true;
  case 0x2E:
    rom = "m";
    return true;
  case 0x2F:
    rom = "y";
    return true;
  case 0x30:
    rom = "r";
    return true;
  case 0x31:
    rom = "r";
    return true;
  case 0x32:
    rom = "l";
    return true;
  case 0x33:
    rom = "l";
    return true; // Marathi ळ / Punjabi ਲ਼
  case 0x34:
    rom = "l";
    return true;
  case 0x35:
    rom = (block == 0x0980) ? "b" : "v";
    return true;
  case 0x36:
    rom = "sh";
    return true;
  case 0x37:
    rom = "sh";
    return true;
  case 0x38:
    rom = "s";
    return true;
  case 0x39:
    rom = "h";
    return true;
  // Precomposed Nukta / Regional extensions
  case 0x58:
    rom = "q";
    return true;
  case 0x59:
    rom = "kh";
    return true;
  case 0x5A:
    rom = "gh";
    return true;
  case 0x5B:
    rom = "z";
    return true;
  case 0x5C:
    rom = "r";
    return true; // ड़ / ড় / ੜ
  case 0x5D:
    rom = "rh";
    return true;
  case 0x5E:
    rom = "f";
    return true;
  case 0x5F:
    rom = "y";
    return true; // Bengali য়
  case 0x70:
    if (block == 0x0980) {
      rom = "r";
      return true;
    } // Assamese র
    return false;
  case 0x71:
    if (block == 0x0980) {
      rom = "v";
      return true;
    } // Assamese ৱ
    return false;
  case 0x79:
    rom = "z";
    return true;
  case 0x7A:
    rom = "y";
    return true;
  default:
    return false;
  }
}

inline const char *indicNuktaOverride(unsigned off, unsigned block) {
  switch (off) {
  case 0x15:
    return "q";
  case 0x16:
    return "kh";
  case 0x17:
    return "gh";
  case 0x1C:
    return "z";
  case 0x21:
    return "r";
  case 0x22:
    return "rh";
  case 0x2B:
    return "f";
  case 0x2F:
    return (block == 0x0980) ? "y" : nullptr;
  case 0x38:
    return (block == 0x0A00) ? "sh" : nullptr; // Punjabi ਸ਼ (sha)
  default:
    return nullptr;
  }
}

inline bool isIndicMatraOffset(unsigned off, const char *&matra) {
  switch (off) {
  case 0x3E:
    matra = "a";
    return true; // ा / া / ਾ
  case 0x3F:
    matra = "i";
    return true; // ि / ি / ਿ
  case 0x40:
    matra = "ee";
    return true; // ी / ী / ੀ
  case 0x41:
    matra = "u";
    return true; // ु / ু / ੁ
  case 0x42:
    matra = "oo";
    return true; // ू / ূ / ੂ
  case 0x43:
  case 0x44:
    matra = "ri";
    return true; // ृ / ৃ
  case 0x45:
  case 0x46:
  case 0x47:
    matra = "e";
    return true; // े / ে / ੇ / ॲ
  case 0x48:
    matra = "ai";
    return true; // ै / ৈ / ੈ
  case 0x49:
  case 0x4A:
  case 0x4B:
    matra = "o";
    return true; // ो / ো / ੋ / ऑ
  case 0x4C:
  case 0x57:
    matra = "au";
    return true; // ौ / ৌ / ੌ
  default:
    return false;
  }
}

inline bool isIndicVowelOffset(unsigned off, const char *&v) {
  switch (off) {
  case 0x04:
  case 0x05:
    v = "a";
    return true; // अ / অ / ਅ
  case 0x06:
    v = "aa";
    return true; // आ / আ / ਆ
  case 0x07:
    v = "i";
    return true; // इ / ই / ਇ
  case 0x08:
    v = "ee";
    return true; // ई / ঈ / ਈ
  case 0x09:
    v = "u";
    return true; // उ / উ / ਉ
  case 0x0A:
    v = "oo";
    return true; // ऊ / ঊ / ਊ
  case 0x0B:
  case 0x0C:
  case 0x60:
  case 0x61:
    v = "ri";
    return true; // ऋ / ঋ
  case 0x0D:
  case 0x0E:
  case 0x0F:
    v = "e";
    return true; // ए / এ / ਏ / ॲ
  case 0x10:
    v = "ai";
    return true; // ऐ / ঐ / ਐ
  case 0x11:
  case 0x12:
  case 0x13:
    v = "o";
    return true; // ओ / ও / ਓ / ऑ
  case 0x14:
    v = "au";
    return true; // औ / ঔ / ਔ
  case 0x72:
    v = "i";
    return true; // Gurmukhi Iri (ੲ)
  case 0x73:
    v = "u";
    return true; // Gurmukhi Ura (ੳ)
  default:
    return false;
  }
}

inline bool isIndicWordChar(unsigned cp) {
  if (!isIndicCp(cp))
    return false;
  const unsigned off = cp & 0x7F;
  return off != 0x64 && off != 0x65 && !(off >= 0x66 && off <= 0x6F);
}

// ===========================================================================
// Urdu / Punjabi Shahmukhi (Perso-Arabic: U+0600..06FF)
// ===========================================================================

inline bool isUrduChar(unsigned cp, const char *&rom) {
  switch (cp) {
  case 0x0622:
    rom = "aa";
    return true;
  case 0x0623:
  case 0x0625:
  case 0x0627:
    rom = "a";
    return true;
  case 0x0628:
    rom = "b";
    return true;
  case 0x067E:
    rom = "p";
    return true;
  case 0x062A:
    rom = "t";
    return true;
  case 0x0679:
    rom = "t";
    return true;
  case 0x062B:
    rom = "s";
    return true;
  case 0x062C:
    rom = "j";
    return true;
  case 0x0686:
    rom = "ch";
    return true;
  case 0x062D:
    rom = "h";
    return true;
  case 0x062E:
    rom = "kh";
    return true;
  case 0x062F:
    rom = "d";
    return true;
  case 0x0688:
    rom = "d";
    return true;
  case 0x0630:
    rom = "z";
    return true;
  case 0x0631:
    rom = "r";
    return true;
  case 0x0691:
    rom = "r";
    return true;
  case 0x0632:
    rom = "z";
    return true;
  case 0x0698:
    rom = "zh";
    return true;
  case 0x0633:
    rom = "s";
    return true;
  case 0x0634:
    rom = "sh";
    return true;
  case 0x0635:
    rom = "s";
    return true;
  case 0x0636:
    rom = "z";
    return true;
  case 0x0637:
    rom = "t";
    return true;
  case 0x0638:
    rom = "z";
    return true;
  case 0x0639:
    rom = "a";
    return true;
  case 0x063A:
    rom = "gh";
    return true;
  case 0x0641:
    rom = "f";
    return true;
  case 0x0642:
    rom = "q";
    return true;
  case 0x0643:
  case 0x06A9:
    rom = "k";
    return true;
  case 0x06AF:
    rom = "g";
    return true;
  case 0x0644:
    rom = "l";
    return true;
  case 0x0645:
    rom = "m";
    return true;
  case 0x0646:
    rom = "n";
    return true;
  case 0x06BA:
    rom = "n";
    return true;
  case 0x06BB:
    rom = "rn";
    return true; // Shahmukhi ݨ
  case 0x0648:
  case 0x0624:
    rom = "o";
    return true;
  case 0x0647:
  case 0x06C1:
  case 0x06C2:
  case 0x06C3:
  case 0x0629:
    rom = "h";
    return true;
  case 0x06BE:
    rom = "h";
    return true;
  case 0x0621:
  case 0x0626:
  case 0x06D2:
    rom = "e";
    return true;
  case 0x0649:
  case 0x064A:
  case 0x06CC:
    rom = "i";
    return true;
  case 0x064E:
    rom = "a";
    return true;
  case 0x0650:
    rom = "i";
    return true;
  case 0x064F:
    rom = "u";
    return true;
  case 0x0670:
    rom = "a";
    return true;
  case 0x064B:
  case 0x064C:
  case 0x064D:
  case 0x0651:
  case 0x0652:
  case 0x0654:
  case 0x0655:
    rom = "";
    return true;
  case 0x06D4:
    rom = ".";
    return true;
  case 0x060C:
    rom = ",";
    return true;
  case 0x061F:
    rom = "?";
    return true;
  default:
    if (cp >= 0x0660 && cp <= 0x0669) {
      static const char *digits[10] = {"0", "1", "2", "3", "4",
                                       "5", "6", "7", "8", "9"};
      rom = digits[cp - 0x0660];
      return true;
    }
    if (cp >= 0x06F0 && cp <= 0x06F9) {
      static const char *digits[10] = {"0", "1", "2", "3", "4",
                                       "5", "6", "7", "8", "9"};
      rom = digits[cp - 0x06F0];
      return true;
    }
    return false;
  }
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

  std::vector<unsigned> cps;
  for (std::size_t i = 0; i < in.size();) {
    unsigned cp = 0;
    i += decode(in, i, cp);
    cps.push_back(cp);
  }

  std::string out;
  bool doubleNext = false;
  bool geminateIndic = false; // For Punjabi Addak (ੱ U+0A71)

  for (std::size_t i = 0; i < cps.size(); ++i) {
    unsigned cp = cps[i];

    // 1. All Brahmic Indic Scripts (Hindi, Marathi, Haryanvi, Bhojpuri,
    // Punjabi, Bengali, etc.)
    if (isIndicCp(cp)) {
      const unsigned block = cp & 0xFF80;
      const unsigned off = cp & 0x7F;

      const char *cons = nullptr;
      if (isIndicConsonantOffset(off, block, cons)) {
        // Check for Nukta (़ / ় / ਼ at offset 0x3C)
        if (i + 1 < cps.size() && (cps[i + 1] & 0x7F) == 0x3C &&
            isIndicCp(cps[i + 1])) {
          if (const char *nk = indicNuktaOverride(off, block))
            cons = nk;
          ++i;
        }

        // If preceded by Punjabi Addak (ੱ), double the initial consonant sound
        if (geminateIndic && cons[0] != '\0') {
          out += cons[0];
          geminateIndic = false;
        }
        out += cons;

        // Check what follows the consonant: Halant, Matra, or Inherent vowel
        if (i + 1 < cps.size() && isIndicCp(cps[i + 1])) {
          const unsigned nextOff = cps[i + 1] & 0x7F;
          const char *matra = nullptr;
          if (nextOff == 0x4D) { // Virama / Halant (् / ্ / ੍)
            ++i;
          } else if (isIndicMatraOffset(nextOff, matra)) {
            out += matra;
            ++i;
          } else if (isIndicWordChar(cps[i + 1])) {
            out += 'a';
          }
        }
        continue;
      }

      const char *vowel = nullptr;
      if (isIndicVowelOffset(off, vowel)) {
        out += vowel;
        continue;
      }

      const char *matra = nullptr;
      if (isIndicMatraOffset(off, matra)) {
        out += matra;
        continue;
      }

      switch (off) {
      case 0x01: // Chandrabindu / Adak Bindi (ँ / ঁ / ਁ)
      case 0x02: // Anusvara / Bindi (ं / ং / ਂ)
        out += (block == 0x0980 && off == 0x02) ? "ng" : "n";
        break;
      case 0x03: // Visarga (ः / ঃ / ਃ)
        out += 'h';
        break;
      case 0x4E: // Bengali Khanda Ta (ৎ U+09CE)
        out += 't';
        break;
      case 0x70: // Punjabi Gurmukhi Tippi (ੰ U+0A70)
        if (block == 0x0A00)
          out += 'n';
        break;
      case 0x71: // Punjabi Gurmukhi Addak (ੱ U+0A71) -> doubles next consonant
        if (block == 0x0A00)
          geminateIndic = true;
        break;
      case 0x50: // Om (ॐ / ੴ Ek Onkar)
        out += (block == 0x0A00) ? "ek onkar" : "om";
        break;
      case 0x64: // Danda (।)
      case 0x65: // Double Danda (॥)
        out += '.';
        break;
      default:
        if (off >= 0x66 && off <= 0x6F) // Indic digits ०-९ / ০-৯ / ੦-੯
          out += static_cast<char>('0' + (off - 0x66));
        break;
      }
      continue;
    }

    // 2. Urdu & Punjabi Shahmukhi (Perso-Arabic Block: U+0600 .. U+06FF)
    if (cp >= 0x0600 && cp <= 0x06FF) {
      const char *urom = nullptr;
      if (isUrduChar(cp, urom)) {
        if (urom[0] != '\0' && urom[1] == '\0' && isVowel(urom[0]) &&
            !out.empty() && out.back() == urom[0]) {
          continue;
        }
        out += urom;
      }
      continue;
    }

    // 3. Standard ASCII & Japanese Handling
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
    case 0x201C:
    case 0x201D:
      out += '"';
      break;
    case 0x2018:
    case 0x2019:
      out += '\'';
      break;
    case 0x2013:
    case 0x2014:
      out += '-';
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
  ~LcdLink() {
    close_port();
    if (daemonFd_ >= 0)
      ::close(daemonFd_);
  }

  bool enabled() const { return enabled_; }
  // True when the screen is reachable: directly, or through the muzxlcd
  // program.
  bool connected() const { return fd_ >= 0 || viaDaemon_; }
  void setEnabled(bool on) {
    if (on != enabled_)
      toggle();
  }
  bool kana() const { return kanaMode_; }
  void setKana(bool on) {
    kanaMode_ = on;
    last_.clear();
    cachedOut_.clear();
  }
  const std::string &port() const { return port_; }

  // Sends a command to the muzxlcd program, e.g. "mode next".
  // Returns false when muzxlcd is not running.
  bool command(const std::string &cmd) { return sendRaw(cmd) >= 0; }

  // Asks muzxlcd something and waits a moment for the answer ("status", "set
  // volume 1" ...).
  bool query(const std::string &cmd, std::string &reply,
             int timeoutMs = 400) const {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return false;
    sockaddr_un me{};
    me.sun_family = AF_UNIX;
    ::bind(fd, reinterpret_cast<sockaddr *>(&me),
           sizeof(sa_family_t)); // automatic address for the answer
    sockaddr_un to{};
    to.sun_family = AF_UNIX;
    const std::string path = daemonSocketPath();
    bool ok = false;
    if (path.size() < sizeof(to.sun_path)) {
      std::copy(path.begin(), path.end(), to.sun_path);
      if (::sendto(fd, cmd.data(), cmd.size(), 0,
                   reinterpret_cast<sockaddr *>(&to), sizeof(to)) >= 0) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, timeoutMs) > 0) {
          char buf[2048];
          const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
          if (n >= 0) {
            reply.assign(buf, static_cast<std::size_t>(n));
            ok = true;
          }
        }
      }
    }
    ::close(fd);
    return ok;
  }

  // True while the lines go through the muzxlcd program.
  bool viaHub() const { return viaDaemon_; }

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

    // If the muzxlcd program is running it owns the screen: hand the line to
    // it (it also switches between lyrics / clock / PC status ...).
    if (viaDaemon_ || now >= nextDaemonTry_) {
      if (sendToDaemon(text, now))
        return;
      nextDaemonTry_ = now + std::chrono::milliseconds(1000);
    }

    if (fd_ >= 0 && now >= nextCheck_) {
      nextCheck_ = now + std::chrono::milliseconds(250);
      pollfd pfd{fd_, POLLOUT, 0};
      struct stat stPath{}, stFd{};
      int mstat = 0;
      const bool dead = (::poll(&pfd, 1, 0) < 0) ||
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
  static std::string daemonSocketPath() {
    if (const char *o = std::getenv("MUZXLCD_SOCKET"); o && *o)
      return o;
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt && *rt)
      return std::string(rt) + "/muzxlcd.sock";
    return "/tmp/muzxlcd-" + std::to_string(getuid()) + ".sock";
  }

  // One datagram to muzxlcd. Returns the sendto() result (-1 = not running).
  ssize_t sendRaw(const std::string &msg) {
    if (daemonFd_ < 0)
      daemonFd_ =
          ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (daemonFd_ < 0)
      return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string path = daemonSocketPath();
    if (path.size() >= sizeof(addr.sun_path))
      return -1;
    std::copy(path.begin(), path.end(), addr.sun_path);
    const ssize_t n =
        ::sendto(daemonFd_, msg.data(), msg.size(), MSG_DONTWAIT,
                 reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    return (n < 0 && errno == EAGAIN) ? 0
                                      : n; // a full queue is not "not running"
  }

  // true = the line went to muzxlcd (or was already sent recently).
  bool sendToDaemon(const std::string &text,
                    std::chrono::steady_clock::time_point now) {
    if (viaDaemon_ && text == daemonText_ &&
        now - daemonSentAt_ < std::chrono::milliseconds(1500))
      return true;
    std::string msg = "lyric " + text;
    for (char &c : msg)
      if (c == '\n' || c == '\r')
        c = ' ';
    // the character-set setting travels with every line (cheap, and it also
    // reaches a muzxlcd that was restarted)
    if (sendRaw(std::string("kana ") + (kanaMode_ ? "1" : "0")) < 0 ||
        sendRaw(msg) < 0) {
      viaDaemon_ = false;
      return false;
    }
    if (!viaDaemon_) { // switch over: release the serial port for muzxlcd
      close_port();
      viaDaemon_ = true;
    }
    daemonText_ = text;
    daemonSentAt_ = now;
    return true;
  }

  bool open_port() {
    const char *env = std::getenv("MUZX_LCD_PORT");
    const std::string candidates[] = {
        env ? env : "", "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2",
        "/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"};
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
  bool viaDaemon_ = false;
  int daemonFd_ = -1;
  std::string daemonText_;
  std::chrono::steady_clock::time_point nextDaemonTry_{}, daemonSentAt_{};
  int fd_ = -1;
  std::string port_;
  std::string last_;
  std::string cachedOut_;
  std::chrono::steady_clock::time_point nextTry_{}, openedAt_{}, nextCheck_{},
      lastWrite_{};
};
