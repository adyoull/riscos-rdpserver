#!/bin/bash
# rdesktop, answering "yes" when it asks whether to trust the server's
# certificate (each test run makes a new self-signed one), with its own
# known_certs store so the user's isn't touched. exec: the PID the test
# holds is rdesktop's own.
# Copyright (c) 2026 Andrew Youll. MIT licence (see LICENSE).
export HOME="${RDESKTOP_HOME:-${TMPDIR:-/tmp}/rdps-rdesktop-home}"
mkdir -p "$HOME"
exec rdesktop "$@" < <(yes yes)
