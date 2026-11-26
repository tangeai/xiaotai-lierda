#include "mem_map.h"

/* Native F9D_D SDK 1.7.0 map. Keep the entire FS intact.
 * The ABI gate independently verifies the matching vendor artifacts. */
ASSERT(PKGFLXAPP_APP0_LNA == 0xAC1000, "TiRTC F9: unexpected application address")
ASSERT(PKGFLXAPP_APP0_SIZE == 0x2B4000, "TiRTC F9: unexpected application capacity")
ASSERT(PKGFLXAPP_RAM_ADDR == 0x0C600000 && PKGFLXAPP_RAM_SIZE == 0x200000,
       "TiRTC F9: unexpected application RAM layout")
ASSERT(PKGFLXFS_IMG_ADDR == 0x575000 && PKGFLXFS_IMG_SIZE == 0x262000,
       "TiRTC F9: native filesystem layout changed")

MEMORY
{
  FLASH (rx) : ORIGIN = PKGFLXAPP_APP0_LNA, LENGTH = PKGFLXAPP_APP0_SIZE
  RAM (rwx)  : ORIGIN = PKGFLXAPP_RAM_ADDR, LENGTH = PKGFLXAPP_RAM_SIZE
}

SECTIONS
{
  .app_header : ALIGN(4)
  {
    KEEP(*(.app_header))
    KEEP(*(.iomap))
    KEEP(*(.cfgmap))
  } > FLASH

  .app_nvram : ALIGN(0x1000)
  {
    KEEP(*(.nvramcfg))
  } > FLASH

	.text :
	{
		*(.text.startup.main)
		*(.text*)
		*(.rodata*)
    /* Match the vendor application's normal-mode FLASH placement. These
     * sleep-vote/SIPC entries are not qualified for Flash-off deep sleep;
     * do not move them into another RAM domain without its full contract. */
    *(.sect_platPARamcode_text.*)
	} > FLASH

  .data : ALIGN(4)
  {
    . = ALIGN(4);
    Load$$LOAD_DRAM_SHARED$$Base = LOADADDR(.data);
    Image$$LOAD_DRAM_SHARED$$Base = .;
    *(.data*)
    *(.sect_ap_psram_p0_data.*)
    *(.sect_ap_ps_psram_p0_data.*)
    *(.sect_platFPSRAMRamcode_text_cust.*)
    *(.sect_platFPSRAMRWData_data_cust.*)
    *(.sect_platFMRamcode_text.*)
    . = ALIGN(4);
  } >RAM AT>FLASH

  Image$$LOAD_DRAM_SHARED$$Length = SIZEOF(.data);

  .load_dram_shared_zi (NOLOAD):
  {
    . = ALIGN(4);
    Image$$LOAD_DRAM_SHARED$$ZI$$Base = .;
		__bss_start__ = .;
		*(.bss*)
    *(.sect_platFMZIData_bss.*)
    *(.sect_*_bss* .bss*)
    *(.sect_ap_psram_p0_bss.*)
    *(.sect_ap_ps_psram_p0_bss.*)
    /* These target archive sections are zero-initialized vendor buffers.
     * Keep them within startup's explicit zero range, rather than orphaning
     * uninitialized RAM beyond the .data copy boundary. */
    *(.cust_sect_ap_psram_p2_bss.*)
    *(.custFAZIData.*)
    *(COMMON)
    *(.sect_decompress_bss.*)
    . = ALIGN(4);
    *(.stack*)               /* stack should be 4 byte align */
    __bss_end__ = .;
    Image$$LOAD_DRAM_SHARED$$ZI$$Limit = .;
    *(.USB_NOINIT_DATA_BUF)
  } >RAM

  /* Read-only layout statistics. No output section placement is changed.
   * The NOLOAD section's load address follows all Flash load images, including
   * SDK orphan data sections. The release audit checks it against every LOAD
   * section and the actual APP binary before this value is shipped. */
  __tirtc_app_flash_origin = ABSOLUTE(ORIGIN(FLASH));
  __tirtc_app_flash_capacity = ABSOLUTE(LENGTH(FLASH));
  __tirtc_app_flash_used = ABSOLUTE(LOADADDR(.load_dram_shared_zi) - ORIGIN(FLASH));
  __tirtc_app_ram_origin = ABSOLUTE(ORIGIN(RAM));
  __tirtc_app_ram_capacity = ABSOLUTE(LENGTH(RAM));
  __tirtc_app_static_ram_used = ABSOLUTE(ADDR(.load_dram_shared_zi) + SIZEOF(.load_dram_shared_zi) - ORIGIN(RAM));
}
