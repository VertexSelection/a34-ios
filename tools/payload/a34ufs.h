#pragma once
#include <stdint.h>

// error codes from ufs_open() and the SCSI path
#define UFS_E_NOTREADY  0x100
#define UFS_E_BUSY      0x200
#define UFS_E_LIST      0x300
#define UFS_E_TIMEOUT   0x400
#define UFS_E_OCS       0x500   // | OCS value
#define UFS_E_RESP      0x600
#define UFS_E_STATUS    0x700   // | SCSI status

// ufs_bcb_clear() results
#define UFS_BCB_NOUFS       1   // ufs_open() failed or not called
#define UFS_BCB_READFAIL    2
#define UFS_BCB_EMPTY       3   // command field empty: nothing to do
#define UFS_BCB_OTHER       4   // some other command: left alone
#define UFS_BCB_FOUND       5   // "boot-recovery" present, read-only run
#define UFS_BCB_WRITEFAIL   6
#define UFS_BCB_VERIFYFAIL  7
#define UFS_BCB_CLEARED     8

struct ufs_diag {
    uint32_t sense;     // last CHECK CONDITION: key << 16 | ASC << 8 | ASCQ
    uint32_t tur1;      // TEST UNIT READY result before START STOP UNIT
    uint32_t sense1;    // its sense data
    uint32_t ssu;       // START STOP UNIT result (only sent if tur1 failed)
    uint32_t tur2;      // TEST UNIT READY result after it
    uint32_t head;      // first 4 bytes of the misc block, big endian
    uint32_t resp6;     // response byte, data-segment length, sense-length hi byte
};
extern struct ufs_diag ufs_diag;

uint32_t ufs_open(uint32_t *hcs_out);
uint32_t ufs_bcb_clear(int do_write, uint32_t *err);
