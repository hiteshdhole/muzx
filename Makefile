# MUZX build.
#   make              build ./player
#   make run          build and start it
#   make muzxlcd      build the LCD hub (lyrics / clock / PC status / spectrum ...)
#   make install-lcd  install muzxlcd and start it at login
#   make test         unit tests (no sound card, Arduino or network needed)
#   make test-e2e     end-to-end test of the LCD hub with fake devices
#   make lcdtest      build the LCD check tool
#   make clean        remove build files
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LDLIBS   := -lsfml-audio -lsfml-system -lsystemd

# The player. muzx.hpp holds the shared declarations; the code is split by topic:
#   terminal  colours, keys, mouse, raw terminal      util      strings, folders
#   state     app state and statistics                playback  shuffle, resume, seek
#   draw      the tabs                                settings  Settings tab, LCD hub rows
#   lyrics_panel  lyrics panel, LCD updates           screen    buttons, whole screen
#   input     key and mouse handling                  integration  MPRIS, notifications, offsets
#   main      run() and main()
PLAYER_SRC := terminal.cpp util.cpp state.cpp playback.cpp draw.cpp integration.cpp \
              settings.cpp lyrics_panel.cpp screen.cpp input.cpp
SRC := main.cpp $(PLAYER_SRC) lyrics.cpp audio_player.cpp cover.cpp queue.cpp selection.cpp config.cpp
OBJ := $(SRC:%.cpp=build/%.o)
LIBOBJ := $(filter-out build/main.o,$(OBJ))

all: player

player: $(OBJ)
	$(CXX) $(CXXFLAGS) -pthread $(OBJ) -o $@ $(LDLIBS)

# -MMD writes header dependencies, so changing a .hpp rebuilds what uses it.
build/%.o: %.cpp | build
	$(CXX) $(CXXFLAGS) -pthread -MMD -MP -c $< -o $@

build:
	mkdir -p build

lcdtest: lcdtest.cpp lcd_link.hpp
	$(CXX) $(CXXFLAGS) lcdtest.cpp -o $@

muzxlcd: muzxlcd.cpp lcd_link.hpp spectrum.hpp
	$(CXX) $(CXXFLAGS) -pthread muzxlcd.cpp -o $@ -lsystemd

# Installs to ~/.local/bin and starts it with your desktop session.
install-lcd: muzxlcd
	install -Dm755 muzxlcd $(HOME)/.local/bin/muzxlcd
	install -Dm644 muzxlcd.service $(HOME)/.config/systemd/user/muzxlcd.service
	systemctl --user daemon-reload
	systemctl --user enable --now muzxlcd.service
	@echo "muzxlcd installed. Try:  muzxlcd ctl mode clock"

run: player
	./player

# ---- tests -----------------------------------------------------------------
# The test programs include the code they test, so they do not need a sound
# card, a screen or an Arduino. Each prints "N checks, M failed".
tests/test_spectrum: tests/test_spectrum.cpp spectrum.hpp tests/check.hpp
	$(CXX) $(CXXFLAGS) $< -o $@

tests/test_hub: tests/test_hub.cpp muzxlcd.cpp lcd_link.hpp spectrum.hpp tests/check.hpp
	$(CXX) $(CXXFLAGS) -pthread $< -o $@ -lsystemd

build/main_test.o: main.cpp | build
	$(CXX) $(CXXFLAGS) -pthread -DMUZX_TEST -MMD -MP -c $< -o $@

tests/test_player: tests/test_player.cpp muzx.hpp tests/check.hpp $(LIBOBJ) build/main_test.o
	$(CXX) $(CXXFLAGS) -pthread tests/test_player.cpp $(LIBOBJ) build/main_test.o -o $@ $(LDLIBS)

test: tests/test_spectrum tests/test_hub tests/test_player
	tests/test_spectrum
	tests/test_hub
	tests/test_player

# Needs python3 + dbus-next (pip install dbus-next) and dbus-run-session; it
# prints SKIP when they are missing.
test-e2e: muzxlcd
	tests/e2e/hub_e2e.sh

clean:
	rm -rf build player lcdtest muzxlcd tests/test_spectrum tests/test_hub tests/test_player

-include $(OBJ:.o=.d) build/main_test.d

.PHONY: all run clean install-lcd test test-e2e
