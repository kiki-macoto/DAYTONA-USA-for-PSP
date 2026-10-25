# Third-party components: PSP build

In addition to the main tree's (`THIRD_PARTY.md`: SoftFloat, ymfm, the LZMA
SDK), the PSP build links:

| Component | Upstream | Version | Licence | Changed |
| --- | --- | --- | --- | --- |
| PSPSDK (`libpspgu`, `libpspgum`, `libpspaudio`, `libpspctrl`, `libpspdisplay`, `libpspge`, `libpsppower`, `libpsprtc`, `libpspdebug`, the import stubs `libpspkubridge` and `libpspsystemctrl_user` for the custom firmware's calls, start-up code) | https://github.com/pspdev/pspsdk | `314b2083` (2026-07-28), from pspdev | BSD-3-Clause | No |
| newlib (C library) | https://github.com/pspdev/newlib | `9e0a0736` (2025-05-17), from pspdev | BSD-style (newlib's COPYING.NEWLIB) | No |
| GCC's libstdc++ and libgcc (C++ library, soft `double`) | https://github.com/pspdev/gcc | `1a339979` (2026-05-11), GCC 15.2.0 | GPL-3 with the GCC Runtime Library Exception | No |
| pthread-embedded (pspdev's threads, pulled in by libstdc++) | https://github.com/pspdev/pthread-embedded | `97fe4ce0` (2026-05-12) | LGPL-2.1 (its PS Vita port's parts MIT) | No |

`game/media_engine.h` adapts the Media Engine start-up of
[psp-media-engine-custom-core](https://github.com/mcidclan/psp-media-engine-custom-core)
by mcidclan (read at `0197c99e`, MIT License, Copyright (c) 2026 mcidclan
(m-c/d, m-cid)): the reset-vector handler (the register writes and the
stack choice) and the cache routines, with the MIT notice in the file. The
library itself (its kernel module, mapping tables and VME code) is not used
or linked.

The toolchain itself is not part of this repository; the versions are those
of the pspdev build recorded in `~/pspdev/build.txt` on the machine the port
was built on. The runtime library exception covers linking libstdc++ and
libgcc into this BSD-licensed program. pthread-embedded is linked
statically (11 of its `pthread_` functions are in the self-test's ELF,
through libstdc++): anyone distributing an `EBOOT.PBP` must meet LGPL-2.1's
terms for it (its source, and this program's objects so it can be relinked).
Nothing is copied from these into the repository.
