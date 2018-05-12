
#include <common/string_util.h>

#include <exos/hal.h>
#include <exos/datetime.h>

#define CMOS_CONTROL_PORT 0x70
#define CMOS_DATA_PORT 0x71

#define REG_SECONDS 0x00
#define REG_MINUTES 0x02
#define REG_HOURS 0x04
#define REG_WEEKDAY 0x06
#define REG_DAY 0x07
#define REG_MONTH 0x08
#define REG_YEAR 0x09

#define REG_STATUS_REG_A 0x0A
#define REG_STATUS_REG_B 0x0B

#define STATUS_REG_A_UPDATE_IN_PROGRESS 0x80

static inline u32 bcd_to_dec(u32 bcd)
{
   return ((bcd & 0xF0) >> 1) + ((bcd & 0xF0) >> 3) + (bcd & 0xf);
}

static inline u32 cmos_read_reg(u32 reg)
{
   int NMI_disable_bit = 0; // temporary
   outb(CMOS_CONTROL_PORT, (NMI_disable_bit << 7) | reg);
   return inb(CMOS_DATA_PORT);
}

static inline bool cmos_is_update_in_progress(void)
{
   return cmos_read_reg(REG_STATUS_REG_A) & STATUS_REG_A_UPDATE_IN_PROGRESS;
}

static void cmod_read_datetime_raw(datetime_t *d)
{
   d->sec = cmos_read_reg(REG_SECONDS);
   d->min = cmos_read_reg(REG_MINUTES);
   d->hour = cmos_read_reg(REG_HOURS);
   d->weekday = cmos_read_reg(REG_WEEKDAY);
   d->day = cmos_read_reg(REG_DAY);
   d->month = cmos_read_reg(REG_MONTH);
   d->year = cmos_read_reg(REG_YEAR);
}

void cmos_read_datetime(datetime_t *out)
{
   datetime_t d, dlast;
   u32 reg_b;
   bool use_24h;
   bool use_binary;
   bool hour_pm_bit;

   reg_b = cmos_read_reg(REG_STATUS_REG_B);
   use_24h = !!(reg_b & (1 << 1));
   use_binary = !!(reg_b & (1 << 2));

   while (cmos_is_update_in_progress()); // wait an eventual update to complete
   cmod_read_datetime_raw(&d);

   do {

      dlast = d;
      while (cmos_is_update_in_progress()); // wait an eventual update to complete
      cmod_read_datetime_raw(&d);

      /*
       * Read until we get the same result twice: this is necessary to get a
       * consistent set of values.
       */

   } while (dlast.raw != d.raw);

   hour_pm_bit = d.hour & 0x80;
   d.hour &= ~0x80;

   if (!use_binary) {
      d.sec = bcd_to_dec(d.sec);
      d.min = bcd_to_dec(d.min);
      d.hour = bcd_to_dec(d.hour);
      d.day = bcd_to_dec(d.day);
      d.month = bcd_to_dec(d.month);
      d.year = bcd_to_dec(d.year);
   }

   if (!use_24h) {
      if (d.hour == 12) {
         if (!hour_pm_bit)
            d.hour = 0; /* 12 am is midnight => hour 0 */
      } else {
         if (hour_pm_bit)
            d.hour = (d.hour + 12) % 24;
      }
   }

   /*
    * This allows to support years from 1970 to 2069,
    * without knowing the century. Yes, knowing the century is a mess and
    * requires asking through ACPI (if supported) for the "century" register.
    * See: https://wiki.osdev.org/CMOS.
    */

   if (d.year < 70)
      d.year += 2000;
   else
      d.year += 1900;

   *out = d;
}