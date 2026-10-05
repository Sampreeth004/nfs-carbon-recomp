# Carbon render-mode addresses for our XEX build

Produced by `scripts/find_fingerprints.py` (instruction-shape search over `guest_image.bin`, base 0x82000000,
mapped to function starts in `generated/default/nfscarbon_init.cpp`). The NFS-CARBON-360-DECOMP addresses
do not match our build; code in this region is shifted by **-0x150**. Data globals are unchanged.

| role | decomp | ours | confidence |
|---|---|---|---|
| renderer constructor | 0x824FFD30 | **0x824FFBE0** | high (unique hit, function start) |
| mode select | 0x8250B350 | **0x8250B200** | med (pattern loose: 403 hits; this one sits exactly at the -0x150 shift) |
| output size | 0x825003E8 | **0x82500298** | high (unique hit) |
| renderer pointer global | 0x82C65304 | 0x82C65304 | high (lwz in mode select) |
| output size globals | 0x82C651E8..F4 | 0x82C651E8..F4 | high (stw in output size) |
| ring wait watchdog | 0x826DEFD0 | 0x826E0250 | high; this is exactly the function `ring_wait_hook.cpp` already hooks, which validates the method |

Layout confirmed in the generated C++ of `sub_824FFBE0` (`nfscarbon_recomp.81.cpp:14456`): per-mode arrays of u32 at
+4 (tile count), +28 (width), +52 (height), +76 (MSAA), at least 6 entries each; rect tables at +100 and +484
(first table starts with the 640x480 mode). Mode 2 already has 1 tile and MSAA 0 in the constructor.

Implemented in `src/render_mode_hook.cpp` (hooks `sub_824FFBE0`, `sub_8250B200`, `sub_82500298`), cvars
`carbon_single_pass` (default off) and `carbon_scene_resolution`. The hook checks the code words at these addresses
on first use and does nothing (logging `[render_mode] fingerprint mismatch`) if they differ.
