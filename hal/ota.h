#ifndef OTA_INCLUDED
#define OTA_INCLUDED
/**
 * @file ota.h
 * Over the air update of an OTA build (make OTA=1; port/xtensa/ota_layout.h, the selector
 * in boot/): the application streams a new image into the slot it is not running from,
 * marks it as the one to try and resets; the selector boots it, and the new image calls
 * ota_confirm() once it has proven itself (joined its network, reached its server). An
 * image that never confirms is booted OTA_TRIALS times and then rejected: the selector
 * goes back to the confirmed one. Where the bytes come from is the application's: an HTTP
 * GET with nornHTTP's body callback straight into ota_write() is the shape examples/ota
 * has.
 *
 * The image file (tools/ota_image.py --key KEY.pem RAM.bin FLASH.bin OUT.nota) is a 112
 * byte header, the RAM image (esptool's format, what the ROM or the selector loads) and
 * the flash image (the code and constants mapped through the cache): magic "NOTA",
 * version 2, the two lengths, the SHA-256 over the two images and its ed25519 signature,
 * little endian. The signature is checked by the selector, not here: an image whose
 * signature does not match the key the selector holds is written, booted once, rejected
 * at that boot ("bad signature") and rolled back. ota_write() parses the header
 * as it streams, erases each flash sector just before the first byte lands in it (so a
 * write call stalls interrupts for at most one sector erase, as a nornStore save does) and
 * hashes what it wrote; ota_end() compares, writes the slot table and returns.
 *
 *   ota_runningSlot()        0 or 1; -1 when the ROM loaded the image itself (no selector)
 *   ota_runningState()       OTA_SLOT_TRIAL or OTA_SLOT_CONFIRMED of the running slot
 *   ota_confirm()            the running slot is good: the selector will keep booting it
 *   ota_begin()              start writing the other slot
 *   ota_write(p, n)          the next bytes of the image file, any size
 *   ota_end()                the image is complete and its hash holds: the other slot
 *                            becomes the one to try on the next reset (rtos_port_reset)
 *   ota_abort()              forget a started write
 *
 * ESP32 only, and only in an OTA build (PORT_OTA_SLOTS); without it the functions return
 * OTA_ERR_NO_SELECTOR. Flash reads of the table go through the ROM's SPI driver, which
 * reads cipher text once flash encryption is on: step 4 of the plan moves them to the
 * cache window.
 */

#include <stdint.h>
#include <stddef.h>

enum
{
    OTA_OK = 0,
    OTA_ERR_NO_SELECTOR = -1,   /**< not an OTA build, or the ROM loaded the image itself */
    OTA_ERR_STATE = -2,         /**< a call out of order (write without begin, end before the header) */
    OTA_ERR_FORMAT = -3,        /**< not an image file, or one that does not fit the slot */
    OTA_ERR_FLASH = -4,         /**< the ROM's flash driver reported an error */
    OTA_ERR_HASH = -5,          /**< the bytes written do not hash to the header's digest */
    OTA_ERR_TABLE = -6          /**< no valid slot table */
};

/** The states ota_runningState() returns, the slot table's values. */
#define OTA_STATE_TRIAL         1
#define OTA_STATE_CONFIRMED     2

int ota_runningSlot(void);
int ota_runningState(void);
int ota_confirm(void);
int ota_begin(void);
int ota_write(const void *data, size_t n);
int ota_end(void);
void ota_abort(void);
/** Bytes of the image written so far (both parts), for progress output. */
uint32_t ota_written(void);

#endif  /* OTA_INCLUDED */
