// Embed a file into the binary at build time (.incbin) so a harness does no file I/O at run time.
#pragma once
#define MAYHEM_EMBED(sym, path)                                         \
	__asm__(".section .rodata\n"                                        \
	        ".global " #sym "\n"                                        \
	        ".balign 64\n" #sym ":\n"                                   \
	        ".incbin \"" path "\"\n"                                    \
	        ".global " #sym "_end\n" #sym "_end:\n"                     \
	        ".byte 0\n"                                                 \
	        ".previous\n");                                             \
	extern "C" const unsigned char sym[];                               \
	extern "C" const unsigned char sym##_end[];
