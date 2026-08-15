/*
******************************************************************************
**
**  File        : LinkerScript.ld
**
**  Author      : STM32CubeIDE
**
**  Abstract    : Linker script for STM32H7 series
**                128Kbytes FLASH and 1056Kbytes RAM
**
**                Set heap size, stack size and stack location according
**                to application requirements.
**
**                Set memory bank area and size if external memory is used.
**
**  Target      : STMicroelectronics STM32
**
**  Distribution: The file is distributed as is, without any warranty
**                of any kind.
**
*****************************************************************************
** @attention
**
** Copyright (c) 2025 STMicroelectronics.
** All rights reserved.
**
** This software is licensed under terms that can be found in the LICENSE file
** in the root directory of this software component.
** If no LICENSE file comes with this software, it is provided AS-IS.
**
****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM_D1) + LENGTH(RAM_D1);    /* end of RAM */
/* Generate a link error if heap and stack don't fit into RAM */
_Min_Heap_Size = 0x200;      /* required amount of heap  */
_Min_Stack_Size = 0x400; /* required amount of stack */

/* Specify the memory areas */
MEMORY
{
  FLASH (rx)     : ORIGIN = 0x90000000, LENGTH = 1M
  DTCMRAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 128K
  RAM_D1 (xrw)   : ORIGIN = 0x24000000, LENGTH = 512K
  RAM_D2 (xrw)   : ORIGIN = 0x30000000, LENGTH = 288K
  RAM_D3 (xrw)   : ORIGIN = 0x38000000, LENGTH = 64K
  ITCMRAM (xrw)  : ORIGIN = 0x00000000, LENGTH = 64K
}

/* Define output sections */
SECTIONS
{
  /* The startup code goes first into FLASH */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* The program code and other data goes into FLASH */
  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
    . = ALIGN(4);
  } >FLASH

  .ARM.extab (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH
  .ARM (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array (READONLY) : /* The READONLY keyword is only supported in GCC11 and later, remove it if using GCC10 or earlier. */
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* Non-cacheable DMA bounce buffer for eMMC (SDMMC1 IDMA) transfers - see
   * mmc_transfer.c. Pinned to the very start of RAM_D1 (AXI SRAM) because
   * MPU region 3 (MPU_Config() in main.c, mirrored in the .ioc) marks exactly
   * 0x24000000..0x24001FFF as non-cacheable, and an ARMv7-M MPU region must be
   * aligned to its own size - 0x24000000 is the only 8KB-aligned spot that
   * doesn't strand usable RAM. RAM_D1 (not RAM_D2 like .mmc_dma_sec below) is
   * deliberate: SDMMC1's IDMA contends with the Ethernet DMA on the AHB
   * matrix when its buffer sits in RAM_D2, which showed up as SDMMC
   * RX_OVERRUN errors.
   *
   * Declared before .data/.bss so those start at 0x24002000 instead; the
   * trailing ABSOLUTE() below reserves the full 8KB, so if .mmc_bounce_sec
   * ever outgrows the MPU region the link fails ("cannot move location counter
   * backwards") rather than silently leaving part of the buffer cacheable. */
  .mmc_bounce_sec (NOLOAD) : {
    . = ABSOLUTE(0x24000000);
    *(.mmc_bounce_sec)
    . = ABSOLUTE(0x24002000);
  } >RAM_D1

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */
    *(.RamFunc)        /* .RamFunc sections */
    *(.RamFunc*)       /* .RamFunc* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >RAM_D1 AT> FLASH

  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss section */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM_D1

  /* User_heap_stack section, used to check that there is enough RAM left */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM_D1

  /* Remove information from the standard libraries */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }
    .lwip_sec (NOLOAD) : {
    . = ABSOLUTE(0x30000000);
    *(.RxDecripSection) 
    
    . = ABSOLUTE(0x30000080);
    *(.TxDecripSection)
    
    . = ABSOLUTE(0x30000100);
    *(.Rx_PoolSection)
  } >RAM_D2

  /* lwIP raw-heap anchor (mem_malloc/mem_free; MEM_SIZE and
   * LWIP_RAM_HEAP_POINTER in lwipopts.h). This used to be the hand-picked
   * constant 0x30004000, which silently overlapped the tail of .lwip_sec
   * above: with ETH_RX_BUFFER_CNT=12 * ETH_RX_BUFFER_SIZE=1536 (plus
   * pbuf_custom overhead), .lwip_sec actually runs 0x30000000-0x30004a83
   * (confirmed via the .map file) - about 2.6KB past 0x30004000. Every
   * mem_malloc()/mem_free() (httpd TX buffers, fsdata.c's per-connection
   * FIL) was therefore scribbling over live Ethernet RX DMA buffers,
   * surfacing as intermittent HardFaults in ethernet_input() reading a
   * corrupted pbuf - worse under load since more heap churn means more
   * chances to hit the overlapping RX buffer slots.
   *
   * Placed by the linker right after .lwip_sec instead, so it can't
   * silently regress the same way again if ETH_RX_BUFFER_CNT/SIZE ever
   * changes - the ASSERT below turns a future collision with .mmc_dma_sec
   * into a build error instead of silent RAM corruption. Reserves 12KB,
   * comfortably above MEM_SIZE (8192B) plus mem.c's ~24B bookkeeping
   * overhead; keep MEM_SIZE under that. */
  .lwip_heap_sec (NOLOAD) : {
    . = ALIGN(32);
    PROVIDE(_lwip_heap_start = .);
    . = . + 12K;
    PROVIDE(_lwip_heap_end = .);
  } >RAM_D2

  ASSERT(_lwip_heap_end <= 0x30008000, "lwIP heap (.lwip_heap_sec) overlaps .mmc_dma_sec (USB MSC bot_data) at 0x30008000 - shrink MEM_SIZE/ETH_RX_BUFFER_CNT or move .mmc_dma_sec")

  /* Non-cacheable home of the USB MSC BOT handle (USBD_static_malloc() in
   * USB_DEVICE/Target/usbd_conf.c), which embeds the bot_data[] transfer
   * buffer. Anchored well past the lwIP Ethernet descriptors/RX pool above
   * (0x30000000-~0x30005000) and inside the non-cacheable MPU region 2
   * (0x30004000, 64KB - see MPU_Config() in main.c).
   *
   * Note: no DMA engine reaches bot_data[] anymore - mmc_transfer.c memcpy()s
   * between it and .mmc_bounce_sec above, and only that bounce buffer is
   * handed to HAL_MMC_*Blocks_DMA(). Keeping this section non-cacheable is
   * therefore no longer strictly required, just harmless. */
  .mmc_dma_sec (NOLOAD) : {
    . = ABSOLUTE(0x30008000);
    *(.mmc_dma_sec)
  } >RAM_D2

  .ARM.attributes 0 : { *(.ARM.attributes) }
}


