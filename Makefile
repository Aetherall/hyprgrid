CXX ?= g++
# The Lua Hyprland is built with (main.cpp refuses to load against another).
LUA_PKG ?= $(shell for p in lua5.5 lua5.4 lua; do pkg-config --exists "$$p >= 5.4" && { echo $$p; break; }; done)
CXXFLAGS ?= -O2 -g

# GCC marks inline functions' statics (e.g. Hyprland's
# __hyprland_api_get_client_hash) as unique symbols: a .so with any can't be
# unloaded, and a later load binds to the old copy's statics. With them, a
# plugin rebuilt for another Hyprland fails its version check until restart.
EXTRA_FLAGS =
ifeq ($(CXX),g++)
    EXTRA_FLAGS += -fno-gnu-unique
endif

SOURCES = main.cpp Grid.cpp View.cpp Lua.cpp Gestures.cpp Layout.cpp TopologyConfig.cpp CApi.cpp Setup.cpp $(wildcard overview/*.cpp)
HEADERS = Grid.hpp View.hpp Lua.hpp Gestures.hpp Layout.hpp TopologyConfig.hpp Topology.hpp Later.hpp Setup.hpp Motion.hpp Gesture.hpp Kinetics.hpp hyprgrid.h $(wildcard overview/*.hpp)
# The overview draws with these too.
PKGS = hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon

.PHONY: all clean test

all: hyprgrid.so

# The default policy, embedded in the plugin as a raw string (Setup.cpp).
.build/hyprgrid.lua.inc: hyprgrid.lua
	mkdir -p .build
	{ printf 'R"HYPRGRID_LUA('; cat hyprgrid.lua; printf ')HYPRGRID_LUA"\n'; } > $@

hyprgrid.so: $(SOURCES) $(HEADERS) .build/hyprgrid.lua.inc
	$(CXX) -shared -fPIC $(CXXFLAGS) $(EXTRA_FLAGS) -std=c++26 -Wno-narrowing -I. -I.build $(SOURCES) -o $@ `pkg-config --cflags $(PKGS) '$(LUA_PKG) >= 5.4'`

# Motion.hpp, Gesture.hpp, Topology.hpp, overview/Scene.hpp and
# overview/Interaction.hpp need no Hyprland: their tests build with the
# compiler alone.
TESTS = .build/motion-test .build/gesture-test .build/topology-test .build/scene-test .build/interaction-test

test: $(TESTS)
	for t in $(TESTS); do $$t || exit 1; done
	lua tests/layout.lua

.build/motion-test: tests/motion.cpp Motion.hpp
	mkdir -p .build
	$(CXX) $(CXXFLAGS) -std=c++26 -I. tests/motion.cpp -o $@

.build/topology-test: tests/topology.cpp Topology.hpp Motion.hpp
	mkdir -p .build
	$(CXX) $(CXXFLAGS) -std=c++26 -I. tests/topology.cpp -o $@

.build/scene-test: tests/scene.cpp overview/Scene.hpp Topology.hpp Motion.hpp
	mkdir -p .build
	$(CXX) $(CXXFLAGS) -std=c++26 -I. tests/scene.cpp -o $@

.build/interaction-test: tests/interaction.cpp overview/Interaction.hpp overview/Scene.hpp Topology.hpp Motion.hpp
	mkdir -p .build
	$(CXX) $(CXXFLAGS) -std=c++26 -I. tests/interaction.cpp -o $@

.build/gesture-test: tests/gesture.cpp Gesture.hpp Motion.hpp
	mkdir -p .build
	$(CXX) $(CXXFLAGS) -std=c++26 -I. tests/gesture.cpp -o $@

clean:
	rm -rf hyprgrid.so .build
