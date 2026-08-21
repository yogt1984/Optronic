optronic_node - sensor node application
build: cmake --preset host-debug && cmake --build --preset host-debug (presets: host-debug/release/asan/tsan, arm64-petalinux, arm64-debian)   run: ./build-host-debug/optronic [-c cfg] [-g gain] [-n frames] [-v]
