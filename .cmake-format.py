# cmake-format / cmake-lint configuration for rocm-ernic
#
# Both cmake-format and cmake-lint from the cmakelang package
# read this file.  The Python format is used instead of YAML
# because cmakelang does not declare pyyaml as a dependency.

with section("format"):
    tab_size = 4
    line_width = 80

with section("lint"):
    disabled_codes = [
        # Line length is impractical for CMake generator
        # expressions and version strings.  Matches the
        # hipFile .cmakelintrc approach.
        "C0301",
    ]
