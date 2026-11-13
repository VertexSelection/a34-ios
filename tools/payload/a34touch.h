#ifndef A34TOUCH_H
#define A34TOUCH_H
#include <stdint.h>

struct a34touch {
    uint32_t stage;
    uint32_t pw_sta, pm_err;
    uint16_t vio_en, vio_mon, vio_vosel, top, vio_en2, vio_mon2;
    uint32_t impw, ifrao0, ifrao1, cfg10, mainpll, univpll;
    uint32_t impw2, ifrao02, ifrao12, cfg10_2;
    uint32_t mode8, mode8_2, mode1, dir0, din0_before, din0_after;
    uint32_t i2c_ctrl, i2c_timing;
    uint32_t st_ver, st_info, st_esd, st_hdr;
    uint8_t ver[28], info_len[2], esd, hdr[8];
};

void a34touch_probe(struct a34touch *t);
#endif
