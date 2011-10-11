#pragma once

const unsigned char *GBCodeToUnicode(unsigned char *gbCode);
const unsigned char *UnicodeToGBCode(unsigned char *unicode);
void UnicodeToUtf8(char* utf8, char *unicode);
void Utf8ToUnicode(char* unicode, char *utf8);
void GB2312StrToUtf8(
        char *utf8Str,        /* Output Utf-8 chars */
        const char* gbStr,        /* Input GB2312 chars */
        int nBytes            /* size of input GB2312 chars */
        );
void Utf8StrToGB2312(
        char *gbStr,        /* Output GB2312 chars */
        char* utf8Str,        /* Input Utf-8 chars */
        int nBytes            /* Size of input GB2312 chars */
        );

