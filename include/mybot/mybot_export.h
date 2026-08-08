/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MYBOT_EXPORT_H_
#define MYBOT_EXPORT_H_

/*
 * Symbol visibility / export macro for the mybot public API.
 *
 * Annotate every public function declaration with MYBOT_API so the SDK can
 * be built as a shared library (Linux/macOS visibility) or a DLL (Windows)
 * and consumed from both static and shared builds. The library build defines
 * MYBOT_BUILDING_LIBRARY (set by CMake on the mybot_sdk target); consumers
 * never define it. Building a shared library also requires a position-
 * independent Agora RTSA archive; the bundled x86_64 static library does not
 * qualify, so the current release remains static-only.
 */

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(MYBOT_BUILDING_LIBRARY)
#define MYBOT_API __declspec(dllexport)
#else
#define MYBOT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define MYBOT_API __attribute__((visibility("default")))
#else
#define MYBOT_API
#endif

#endif /* MYBOT_EXPORT_H_ */
