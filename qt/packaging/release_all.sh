#!/bin/bash
# Entry point for release_driver.py; all inputs and work locations are explicit arguments.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd -P)
exec nice -n 10 "${PYTHON:-python3}" "$HERE/release_driver.py" "$@"
