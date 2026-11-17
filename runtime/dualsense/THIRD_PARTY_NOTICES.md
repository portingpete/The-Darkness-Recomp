# DualSense protocol references

Report layouts and legacy-rumble compatibility behavior are based on SDL's PS5 HIDAPI driver, pinned at [c917ba60c16ed745604cafaeaa05ca902a7346d9](https://github.com/libsdl-org/SDL/blob/c917ba60c16ed745604cafaeaa05ca902a7346d9/src/joystick/hidapi/SDL_hidapi_ps5.c). This Rust implementation is an independent adaptation for DarkRecomp's existing input worker, not the SDL driver.

SDL license notice:

Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

The transport uses hidapi-rs2.6.7 with its native Windows backend. Cargo.lock pins its dependencies; their original license notices apply. No SDL binary or original game assets are included.
