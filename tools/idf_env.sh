# Source this, don't run it:  . tools/idf_env.sh
#
# The project's .venv is Python 3.9 and sits ahead of Homebrew on PATH, so a
# bare `. ~/esp/esp-idf/export.sh` picks 3.9 and then cannot find the toolchain
# venv, which was built against Python 3.14. Put Homebrew first for the
# duration of the ESP-IDF setup.
export PATH="/opt/homebrew/bin:$PATH"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.14_env"
. "$HOME/esp/esp-idf/export.sh"
