#include "fixed-point.h"

int 
IntToFP (int num) 
{
  return num * FP_CONVERSION_CONST;
}

/* Converts an integer to its fixed point representation */
int 
FractionToFP (int numer, int denom) 
{
  return IntToFP(numer) * IntToFP(denom) / FP_CONVERSION_CONST;
}

/* Converts a fixed point to its integer representation. May truncate */
int 
FPToInt (int fp) 
{
  if (fp >= 0)
    return (fp + FP_CONVERSION_CONST/2) / FP_CONVERSION_CONST;
  return (fp - FP_CONVERSION_CONST/2) / FP_CONVERSION_CONST;
}

int 
FPMultiply (int fp1, int fp2) 
{
  return ((int64_t) fp1) * fp2 / FP_CONVERSION_CONST;
}

int 
FPDivide (int fp1, int fp2) 
{
  return (((int64_t) fp1) / fp2) * FP_CONVERSION_CONST;
}
