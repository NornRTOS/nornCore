#ifndef OTA_LAYOUT_INCLUDED
#define OTA_LAYOUT_INCLUDED
/**
 * @file ota_layout.h
 * The flash layout of an OTA build (make OTA=1, nornInternal/nornCore/ota_plan.md), shared
 * by the selector (boot/), the port (rtos_port_flashInit) and tools/ota_table.py, which
 * reads the values from here. Offsets in the 4 MB flash:
 *
 *   0x000000  secure boot digest (the ROM's, only with the eFuse)
 *   0x001000  the selector: the RAM image the ROM loads, under 8 KB
 *   0x00E000  slot table, copy 0; 0x00F000 copy 1 (one sector each)
 *   0x010000  slot A: RAM image at +0, flash image at +OTA_SLOT_FLASH_OFF
 *   0x190000  slot B: the same
 *   0x3FE000  nornStore (hal/flash.h), as without OTA
 *
 * An image is accepted only if its SHA-256 digest carries a valid ed25519 signature by the
 * key of tools/ota_keygen.py (the public key in boot/ota_pubkey.h): the selector checks the
 * hash with the chip's SHA block and then the signature over the 32 digest bytes with
 * TweetNaCl, so the signing cost does not grow with the image.
 *
 * The selector leaves the running slot in two RTC store registers so that the port maps
 * the right flash pages: OTA_RTC_MAGIC (with the slot number in the low byte) in STORE2
 * and the physical base of the slot's flash image in STORE3.
 */

#define OTA_SELECTOR_ADDR       0x001000ul
#define OTA_TABLE_ADDR          0x00E000ul      /* copy 0; copy 1 one sector up */
#define OTA_TABLE_COPIES        2
#define OTA_SECTOR              0x1000ul
#define OTA_SLOT_A              0x010000ul
#define OTA_SLOT_B              0x190000ul
#define OTA_SLOT_SIZE           0x180000ul
#define OTA_SLOT_FLASH_OFF      0x080000ul      /* the flash image inside a slot, 64 KB aligned */
#define OTA_SLOTS               2
#define OTA_TRIALS              3               /* boots a new image gets before the roll back */

#define OTA_TABLE_MAGIC         0x544C534Eul    /* "NSLT" */
#define OTA_TABLE_VERSION       2               /* 2: the signature in every slot entry */
#define OTA_RTC_MAGIC           0x4F544100ul    /* "OTA\0" | slot */

/** Slot states in the table. */
#define OTA_SLOT_EMPTY          0
#define OTA_SLOT_TRIAL          1               /* new, not yet confirmed */
#define OTA_SLOT_CONFIRMED      2
#define OTA_SLOT_REJECTED       3

#ifndef __ASSEMBLER__
#include <stdint.h>

/** One slot's entry. */
typedef struct
{
    uint32_t state;             /**< OTA_SLOT_* */
    uint32_t ramLength;         /**< bytes of the RAM image at the slot base */
    uint32_t flashLength;       /**< bytes of the flash image at base + OTA_SLOT_FLASH_OFF, 0 if none */
    uint8_t  sha256[32];        /**< over the RAM image bytes followed by the flash image bytes */
    uint8_t  signature[64];     /**< ed25519 over the 32 digest bytes, by the key whose public half the selector holds */
} ota_slot_t;

/** The table, one copy per sector; the copy with the highest sequence number and a valid
    CRC rules. The CRC covers everything before it. */
typedef struct
{
    uint32_t magic;             /**< OTA_TABLE_MAGIC */
    uint32_t version;           /**< OTA_TABLE_VERSION */
    uint32_t sequence;          /**< grows with every write */
    uint32_t active;            /**< the slot to boot, 0 or 1 */
    uint32_t trialsLeft;        /**< boots left for a trial slot before the roll back */
    ota_slot_t slot[OTA_SLOTS];
    uint32_t crc32;             /**< of the bytes above, IEEE, init 0xFFFFFFFF, final xor */
} ota_table_t;
#endif

#endif  /* OTA_LAYOUT_INCLUDED */
