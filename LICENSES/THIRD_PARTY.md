# Third-party notices

PS5 LocalSend's own source code is licensed under the MIT License; see the
repository-root `LICENSE`. The notices below cover external projects only.

This repository vendors the unmodified official GNU libmicrohttpd 1.0.10
release archive at
`third_party/libmicrohttpd/libmicrohttpd-1.0.10.tar.gz` (SHA-256
`04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685`).
Upstream is <https://www.gnu.org/software/libmicrohttpd/> and the matching tag
commit is `7922bbd7a9561fc3f8ebd6f5cecdc288dcd4457f`.

The libmicrohttpd library code is available under LGPL-2.1-or-later or the GNU
GPL with eCos exception. Its complete notice is preserved in
`third_party/libmicrohttpd/COPYING`. This project uses a custom embedded static
build with TLS, built-in authentication, postprocessing, cookies, messages,
examples, documentation and tools disabled. The release source bundle keeps the
exact library archive, application sources and build scripts available for
inspection, modification and relinking. No upstream file is patched.

The PS5 target is compiled against **ps5-payload-dev/sdk v0.43**. The SDK is
maintained at <https://github.com/ps5-payload-dev/sdk> and is predominantly
licensed under GPL-3.0-or-later, with BSD-licensed FreeBSD-derived headers in
`include/freebsd`. Consult the installed SDK's `LICENSE` and per-file notices.
The SDK is not downloaded or redistributed by this repository.
Anyone distributing a PS5 build must independently review the installed SDK,
the generated and linked artifacts, and all applicable SDK and per-file terms
before redistribution. This notice is not legal advice.

The PS5 notification ABI declaration and request layout follow the upstream
SDK `samples/hello_world` example. No implementation source was copied.

The LocalSend protocol documentation informs the high-level architecture but
no LocalSend source code is included. See <https://github.com/localsend/protocol>.
