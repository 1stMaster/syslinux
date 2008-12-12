#ifndef COMBOOT_H
#define COMBOOT_H

/**
 * @file
 *
 * SYSLINUX COMBOOT
 */

#include <stdint.h>
#include <setjmp.h>
#include <gpxe/in.h>

/** Segment used for COMBOOT PSP and image */
#define COMBOOT_PSP_SEG 0x07C0

/** Entry point address of COM32 images */
#define COM32_START_PHYS 0x101000

/** COM32 bounce buffer segment */
#define COM32_BOUNCE_SEG 0x07C0

/** Size of SYSLINUX file block in bytes */
#define COMBOOT_FILE_BLOCKSZ 512

/** COMBOOT feature flags (INT 22h AX=15h) */
#define COMBOOT_FEATURE_LOCAL_BOOT (1 << 0)
#define COMBOOT_FEATURE_IDLE_LOOP  (1 << 1)

/** Maximum number of shuffle descriptors for 
 * shuffle and boot functions
 * (INT 22h AX=0012h, 001Ah, 001Bh)
 */
#define COMBOOT_MAX_SHUFFLE_DESCRIPTORS 682

typedef union {
	uint32_t l;
	uint16_t w[2];
	uint8_t  b[4];
} com32_reg32_t;

typedef struct {
	uint16_t gs;                /* Offset  0 */
	uint16_t fs;                /* Offset  2 */
	uint16_t es;                /* Offset  4 */
	uint16_t ds;                /* Offset  6 */

	com32_reg32_t edi;          /* Offset  8 */
	com32_reg32_t esi;          /* Offset 12 */
	com32_reg32_t ebp;          /* Offset 16 */
	com32_reg32_t _unused_esp;  /* Offset 20 */
	com32_reg32_t ebx;          /* Offset 24 */
	com32_reg32_t edx;          /* Offset 28 */
	com32_reg32_t ecx;          /* Offset 32 */
	com32_reg32_t eax;          /* Offset 36 */

	com32_reg32_t eflags;       /* Offset 40 */
} com32sys_t;

typedef struct {
	uint32_t dest;
	uint32_t src;
	uint32_t len;
} comboot_shuffle_descriptor;

extern void hook_comboot_interrupts ( );

/* These are not the correct prototypes, but it doens't matter, 
 * as we only ever get the address of these functions;
 * they are only called from COM32 code running in PHYS_CODE
 */
extern void com32_intcall_wrapper ( );
extern void com32_farcall_wrapper ( );
extern void com32_cfarcall_wrapper ( );

/* Resolve a hostname to an (IPv4) address */
extern int comboot_resolv ( const char *name, struct in_addr *address );

/* setjmp/longjmp context buffer used to return after loading an image */
extern jmp_buf comboot_return;

/* Command line to execute when returning via comboot_return 
 * with COMBOOT_RETURN_RUN_KERNEL
 */
extern char *comboot_kernel_cmdline;

/* Execute comboot_image_cmdline */
extern void comboot_run_kernel ( );

extern void *com32_external_esp;

#define COMBOOT_RETURN_EXIT 1
#define COMBOOT_RETURN_RUN_KERNEL 2

extern void comboot_force_text_mode ( void );

#define COMBOOT_VIDEO_GRAPHICS    0x01
#define COMBOOT_VIDEO_NONSTANDARD 0x02
#define COMBOOT_VIDEO_VESA        0x04
#define COMBOOT_VIDEO_NOTEXT      0x08

#endif
