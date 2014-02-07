#pragma once
#include "str.h"
const unsigned char *GBCodeToUnicode(unsigned char *gbCode);
void UnicodeToUtf8(char* utf8, char *unicode);
void GB2312StrToUtf8(
        char *utf8Str,        /* Output Utf-8 chars */
        const char* gbStr,        /* Input GB2312 chars */
        int nBytes            /* size of input GB2312 chars */
        );

int gb2312_convert(const struct charset *cs, char *dst,
                 const char *src, int len, int strict);