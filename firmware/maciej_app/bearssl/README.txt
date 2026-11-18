BearSSL 0.6 (https://bearssl.org/, Thomas Pornin; MIT licence: LICENSE.txt) for the v4 demo's TLS client
(demo/tls.c: wget https://).

From bearssl-0.6.tar.gz (SHA-256 6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14, the
release's), unchanged: inc/ whole, src/inner.h and src/config.h, and the 86 of src/'s 277 files that a full
client needs -- what br_ssl_client_init_full, br_sslio and br_x509_decoder pull in when tls.c is linked
against all of them.  shim/string.h is ours: the five string.h functions BearSSL calls (the demo has no C
library; disk.c and tls.c define them).

build.py compiles every src/**/*.c here into a library (warnings off: it is not our code) and links the demo
against it when demo/tls.c is one of its sources.  A function another file holds shows up as an undefined
reference: copy that file from the release's src/ into the same folder here.
