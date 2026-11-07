#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Stands in for glassine-shell and glassine-swayd in test_home: logs its words, one line per call.
echo "$*" >> "$GLASSINE_HOME_TEST_LOG"
