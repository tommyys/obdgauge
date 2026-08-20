# ESP-IDF component registration. The host test build uses
# firmware/test/host/Makefile instead and ignores this file.
idf_component_register(SRCS "version.cpp" "pid.cpp" "parse.cpp" "poll.cpp" "state.cpp" "metrics.cpp"
                       INCLUDE_DIRS ".")
