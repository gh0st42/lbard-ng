#ifndef UTIL_H
#define UTIL_H


void sleep_ms(int milliseconds);

long long gettime_us();
long long gettime_ms();

int chartohex(int c);
int hextochar(int h);

int dump_bytes(char *msg, unsigned char *bytes, int length);

#endif