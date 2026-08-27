# ESP-IDF component registration. The host test build uses
# firmware/test/host/Makefile instead and ignores this file.
idf_component_register(SRCS "version.cpp" "pid.cpp" "parse.cpp" "poll.cpp" "state.cpp" "metrics.cpp" "glow.cpp" "vehicle.cpp" "ignition.cpp" "elm327.cpp" "replay.cpp" "avail.cpp" "logbuf.cpp" "crc32.cpp"
                       INCLUDE_DIRS ".")
