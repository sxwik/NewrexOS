#include "rtc.h"

unsigned char rtc_second;
unsigned char rtc_minute;
unsigned char rtc_hour;
unsigned char rtc_day;
unsigned char rtc_month;
unsigned int  rtc_year;

// Local port communication wrappers
static void outb_rtc(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static unsigned char inb_rtc(unsigned short port) {
    unsigned char result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

// Check if the RTC is currently updating the time internally
int get_update_in_progress_flag() {
    outb_rtc(0x70, 0x0A);
    return (inb_rtc(0x71) & 0x80);
}

// Fetch a specific register from the CMOS chip
unsigned char get_rtc_register(int reg) {
    outb_rtc(0x70, reg);
    return inb_rtc(0x71);
}

void read_rtc() {
    unsigned char registerB;
    unsigned char century;
    unsigned char last_second, last_minute, last_hour, last_day, last_month, last_year, last_century;

    // Wait until the chip is not busy updating
    while (get_update_in_progress_flag());

    rtc_second = get_rtc_register(0x00);
    rtc_minute = get_rtc_register(0x02);
    rtc_hour   = get_rtc_register(0x04);
    rtc_day    = get_rtc_register(0x07);
    rtc_month  = get_rtc_register(0x08);
    rtc_year   = get_rtc_register(0x09);
    century    = get_rtc_register(0x32); 

    // Read a second time to guarantee we didn't read during a micro-tick crossover
    do {
        last_second = rtc_second;
        last_minute = rtc_minute;
        last_hour   = rtc_hour;
        last_day    = rtc_day;
        last_month  = rtc_month;
        last_year   = rtc_year;
        last_century = century;

        while (get_update_in_progress_flag());
        rtc_second = get_rtc_register(0x00);
        rtc_minute = get_rtc_register(0x02);
        rtc_hour   = get_rtc_register(0x04);
        rtc_day    = get_rtc_register(0x07);
        rtc_month  = get_rtc_register(0x08);
        rtc_year   = get_rtc_register(0x09);
        century    = get_rtc_register(0x32);
    } while (last_second != rtc_second || last_minute != rtc_minute || last_hour != rtc_hour ||
             last_day != rtc_day || last_month != rtc_month || last_year != rtc_year || last_century != century);

    registerB = get_rtc_register(0x0B);

    // Decode BCD format back into standard binary numbers
    if (!(registerB & 0x04)) {
        rtc_second = (rtc_second & 0x0F) + ((rtc_second / 16) * 10);
        rtc_minute = (rtc_minute & 0x0F) + ((rtc_minute / 16) * 10);
        rtc_hour   = ((rtc_hour & 0x0F) + (((rtc_hour & 0x70) / 16) * 10)) | (rtc_hour & 0x80);
        rtc_day    = (rtc_day & 0x0F) + ((rtc_day / 16) * 10);
        rtc_month  = (rtc_month & 0x0F) + ((rtc_month / 16) * 10);
        rtc_year   = (rtc_year & 0x0F) + ((rtc_year / 16) * 10);
        century    = (century & 0x0F) + ((century / 16) * 10);
    }

    // Convert to 24-hour time if it's currently on a 12-hour cycle
    if (!(registerB & 0x02) && (rtc_hour & 0x80)) {
        rtc_hour = ((rtc_hour & 0x7F) + 12) % 24;
    }

    // Combine century and year for the final value
    if (century != 0) {
        rtc_year += century * 100;
    } else {
        rtc_year += (rtc_year > 69) ? 1900 : 2000;
    }
}
