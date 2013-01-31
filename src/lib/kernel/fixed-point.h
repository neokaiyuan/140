#ifndef __LIB_KERNEL_FIXED_POINT_H 
#define __LIB_KERNEL_FIXED_POINT_H 

#include <stdint.h>

/* This file implements utilities involved with fixed point real arithmetic */
#define FP_CONVERSION_CONST 16384	/* 2^14 */

int IntToFP (int num);

/* Converts an integer to its fixed point representation */
int FractionToFP (int numer, int denom);

/* Converts a fixed point to its integer representation. May truncate */
int FPToInt (int fp);

int FPMultiply (int fp1, int fp2);

int FPDivide (int fp1, int fp2);

#endif /* threads/fixed-point.h */
