# GNU libmicrohttpd 1.0.10

This directory contains the unmodified official release archive downloaded from:

<https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.10.tar.gz>

- Version: 1.0.10
- Upstream tag commit: `7922bbd7a9561fc3f8ebd6f5cecdc288dcd4457f`
- SHA-256: `04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685`
- License: LGPL-2.1-or-later or the GNU GPL with eCos exception (see `COPYING`)

The Makefile extracts and configures this archive locally. It never downloads a
dependency. Host and PS5 builds disable TLS, built-in authentication,
postprocessing, cookies, messages, examples, documentation and tools because
this project uses only the bounded HTTP/1.x daemon API. The PS5 configure step
uses the compatible `x86_64-unknown-freebsd` Autoconf host triplet while the
actual compiler target remains `x86_64-sie-ps5`; no upstream source patches are
applied.
