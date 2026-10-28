// Battery clock: an OKI MSM6242B at $DC0000, as on the A500+ board and the
// A501 trapdoor card.  Sixteen 4-bit registers, one per longword (the low
// nibble of the word at $DC0000 + 4n + 2):
//   0-1 seconds, 2-3 minutes, 4-5 hours (bit 2 of 5 = PM in 12-hour mode),
//   6-7 day, 8-9 month, 10-11 year (BCD digits, units first), 12 weekday,
//   13-15 control D, E, F (D: HOLD 1, BUSY 2, IRQ 4, 30-second adjust 8;
//   F: 24-hour mode 4, STOP 2, RESET 1).
// The time is the platform's (cfg.rtc_time), read whenever a time register is;
// writes to the time registers are ignored (setting the clock from the Amiga
// does not reach the platform), the control registers keep what is written,
// BUSY always reads 0.  Two-digit years: 78-99 are 19xx, 00-77 20xx, as
// battclock.resource reads them.
#include "amiga_int.h"

static uint8_t ctl_d, ctl_e, ctl_f = 4;
static uint8_t t[13];                     // the time registers, as of the last read

// Days since 1970-01-01 -> year, month (1-12), day (Howard Hinnant's algorithm).
static void civil(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static void latch(void) {
    int64_t s = A.cfg.rtc_time ? A.cfg.rtc_time(A.cfg.ctx) : 0;
    if (s < 0) s = 0;
    const int64_t days = s / 86400;
    int sec = (int)(s % 86400), y, mo, d;
    civil(days, &y, &mo, &d);
    int h = sec / 3600, mi = sec / 60 % 60, se = sec % 60;
    bool pm = false;
    if (!(ctl_f & 4)) { pm = h >= 12; h %= 12; if (!h) h = 12; }    // 12-hour mode
    t[0] = (uint8_t)(se % 10);  t[1] = (uint8_t)(se / 10);
    t[2] = (uint8_t)(mi % 10);  t[3] = (uint8_t)(mi / 10);
    t[4] = (uint8_t)(h % 10);   t[5] = (uint8_t)(h / 10 | (pm ? 4 : 0));
    t[6] = (uint8_t)(d % 10);   t[7] = (uint8_t)(d / 10);
    t[8] = (uint8_t)(mo % 10);  t[9] = (uint8_t)(mo / 10);
    t[10] = (uint8_t)(y % 10);  t[11] = (uint8_t)(y / 10 % 10);
    t[12] = (uint8_t)((days + 4) % 7);                               // 1970-01-01 was a Thursday
}

uint8_t rtc_read(int reg) {
    switch (reg) {
    case 13: return ctl_d & ~2u;
    case 14: return ctl_e;
    case 15: return ctl_f;
    default:
        latch();
        return t[reg];
    }
}

void rtc_write(int reg, uint8_t v) {
    v &= 15;
    if (reg == 13) ctl_d = v;
    else if (reg == 14) ctl_e = v;
    else if (reg == 15) ctl_f = v;
}

void rtc_reset(void) {
    ctl_d = ctl_e = 0;
    ctl_f = 4;
    latch();
}
