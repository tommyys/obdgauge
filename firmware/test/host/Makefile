CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g

CORE_DIR := ../../components/gauge_core
CORE_SRC := $(wildcard $(CORE_DIR)/*.cpp)
TESTS    := $(patsubst %.cpp,%,$(wildcard test_*.cpp))
BUILD    := build

.PHONY: all test clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%: %.cpp $(CORE_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I. -I$(CORE_DIR) $< $(CORE_SRC) -o $@

replay_check: $(BUILD)/replay_check

test: $(addprefix $(BUILD)/,$(TESTS))
	@fail=0; for t in $^; do echo "=== $${t##*/} ==="; ./$$t || fail=1; echo; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL SUITES PASSED"; else echo "SUITE FAILURES"; fi; exit $$fail

clean:
	@rm -rf $(BUILD)
