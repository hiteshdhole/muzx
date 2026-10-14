#ifndef COVER_HPP
#define COVER_HPP

#include <filesystem>
#include <string>

std::filesystem::path resolveCoverArt(const std::filesystem::path &songPath);
void renderPlayerCover(const std::string &songPath, bool showArt,
                       bool isPlayerTab);
void drawConfirmedKittyCover(const std::string &songPath, bool showArt,
                             bool isPlayerTab);
void clearTerminalCover();

#endif
