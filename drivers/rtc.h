#ifndef RTC_H
#define RTC_H

// Exposed variables for the kernel shell to print
extern unsigned char rtc_second;
extern unsigned char rtc_minute;
extern unsigned char rtc_hour;
extern unsigned char rtc_day;
extern unsigned char rtc_month;
extern unsigned int  rtc_year;

// The main hardware ping function
void read_rtc();

#endif
