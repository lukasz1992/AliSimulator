/* web.html, the page http.c serves, built into the demo as it is: web_page .. web_page_end
   (build.py passes the demo's folder to the assembler for .incbin) */
    .section .rodata
    .globl web_page
    .globl web_page_end
web_page:
    .incbin "web.html"
web_page_end:
    .byte 0
