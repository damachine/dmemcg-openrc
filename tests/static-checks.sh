#!/bin/sh
set -eu

if ./dmem-run >/dev/null 2>&1; then exit 1; else status=$?; fi
[ "$status" -eq 1 ]
if ./dmemcg-openrcd --invalid >/dev/null 2>&1; then exit 1; else status=$?; fi
[ "$status" -eq 1 ]
if command -v shellcheck >/dev/null 2>&1; then
	shellcheck -s sh openrc/dmemcg-openrc openrc/dmemcg-openrc.conf tests/static-checks.sh
fi
