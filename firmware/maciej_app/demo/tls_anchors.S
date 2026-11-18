/* tls_anchors.bin, the roots wget's https:// checks certificates against (tools/tls_anchors.py made it from
   curl's cacert.pem; tls_anchors.txt lists them), built into the demo as it is: tls_anchors_bin ..
   tls_anchors_bin_end (build.py passes the demo's folder to the assembler for .incbin) */
    .section .rodata
    .globl tls_anchors_bin
    .globl tls_anchors_bin_end
tls_anchors_bin:
    .incbin "tls_anchors.bin"
tls_anchors_bin_end:
    .byte 0
