# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT
if length != 1 then error("semgrep: expected one JSON document") else .[0] end |
if type != "object" then error("semgrep: expected an object")
elif (.results | type) != "array" then error("semgrep: missing results")
elif (.errors | type) != "array" then error("semgrep: missing errors")
# PartialParsing warnings are coverage limitations, not engine failures.
# The runner publishes them prominently; every other diagnostic fails closed.
elif any(.errors[]; .level != "warn" or (.type | type) != "array" or .type[0] != "PartialParsing")
then error("semgrep: engine errors: \(.errors)")
elif (.paths.scanned | type) != "array" or (.paths.scanned | length) == 0
then error("semgrep: no files scanned")
elif any(.results[]; (.check_id | type) != "string")
then error("semgrep: malformed finding")
else .results | length end
