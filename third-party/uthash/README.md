# uthash

`uthash.h` is uthash 2.4.0, copied unchanged from the upstream release:

- Project: <https://troydhanson.github.io/uthash/>
- Source: <https://github.com/troydhanson/uthash>, tag `v2.4.0` (commit
  `a49bed0b4abb7dff16c73906dcdc8a9718d582d2`), file `src/uthash.h`
- License: `BSD-1-Clause`, in [`LICENSE`](LICENSE), also copied unchanged

Only `uthash.h` is taken. The release's other headers (`utlist.h`,
`utarray.h`, ...) aren't used.

Only `src/ernic_map.c` includes it, as a `SYSTEM` include set on that one
file in `CMakeLists.txt` and `tests/CMakeLists.txt`; the rest of the code
uses the typed maps in `src/ernic_map.h`. The uthash settings (its hash
function and what it does when out of memory) are in `src/ernic_map.c`.

Don't edit the files here. To update, replace `uthash.h` and `LICENSE` with
a new release's, and change the version, tag, and commit above.
