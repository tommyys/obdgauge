# Host tests for `gauge_core`

`gauge_core` is dependency-free C++17 with no ESP-IDF and no LVGL, so it
compiles and runs on the Mac. That is deliberate: the maths is proven before
it ever runs on the board (`SPEC.md` §5).

## Run the tests

```bash
cd firmware/test/host
make test
```

Builds every `test_*.cpp` against the component sources and runs them. No
CMake, no toolchain, no board — just a C++17 compiler.

## Cross-validate against the simulator

The strongest check available: feed the same capture through both the Python
and the C++ and diff every sample.

```bash
make replay_check
.venv/bin/python tools/dump_python_states.py logs/<drive>.csv > /tmp/ref.txt
./build/replay_check logs/<drive>.csv /tmp/ref.txt
```

Every capture in `logs/`:

```bash
for f in logs/*.csv; do
  .venv/bin/python tools/dump_python_states.py "$f" > /tmp/ref.txt
  ./firmware/test/host/build/replay_check "$f" /tmp/ref.txt
done
```

A divergence names the sample index, the field and both values. Where C++ and
Python disagree, **the Python is right** until proven otherwise — it is the
implementation that has actually driven the car.
