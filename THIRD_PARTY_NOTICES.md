# Third-party notices

This repository does not vendor third-party source. The build downloads the components below
(SHA-256 pinned in `CMakeLists.txt`, `engine/build_host.ps1`, and `android/app/build.gradle.kts`),
so binaries built from this tree must be distributed with this notice. Lua, `stb_vorbis`,
and `dr_mp3` are linked statically into engine/test binaries; SDL3 is packaged only in the
Android APK (`libSDL3.so` plus its Java activity classes).

## Lua 5.1.5

Source: <https://www.lua.org/ftp/lua-5.1.5.tar.gz> (unmodified; compiled as C++).

```
Copyright (C) 1994-2012 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## SDL 3.4.20

Source: <https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20> (unmodified prebuilt
Android AAR from `SDL3-devel-3.4.20-android.zip`).

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>
  
This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:
  
1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required. 
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## stb_vorbis v1.24

Source: <https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c/stb_vorbis.c> (unmodified; compiled inside `wse_audio_vendor`).

```
Copyright (c) 2017 Sean Barrett

Dual-licensed under MIT or Public Domain (Unlicense).

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## dr_mp3 v0.7.5 (and minimp3)

Source: <https://raw.githubusercontent.com/mackron/dr_libs/f12e197fd462e4c22873fbd70d11b1d2f2c1dc3c/dr_mp3.h> (unmodified; compiled inside `wse_audio_vendor`).

```
Copyright 2023 David Reid

Dual-licensed under Public Domain (Unlicense) or MIT No Attribution (MIT-0).

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Based on minimp3 (https://github.com/lieff/minimp3), dedicated to the public
domain under CC0 1.0 (<http://creativecommons.org/publicdomain/zero/1.0/>).
```

## Khronos `glslang` 16.6.0 (`wse_glslang`)

- Upstream: <https://github.com/KhronosGroup/glslang> (tag `16.6.0`)
- Archive SHA-256: `9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2`
- Used in: `wse_glslang` / `wse_shader` (`CMakeLists.txt`, `engine/build_host.ps1`, `engine/src/shader/wse_shader_compiler.cpp`) to compile lowered Warband `#version 450` GLSL into Vulkan SPIR-V 1.0 (slice P5).
- License: Multi-licensed (BSD 3-Clause, Apache License 2.0, MIT, and 3Dlabs/Khronos permissive notices — see `LICENSE.txt` inside the verified 16.6.0 archive).

