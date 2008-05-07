#ifndef BLIB_H_
#define BLIB_H_

#include <stdint.h>

char ToLower(char);
char ToUpper(char);
void GetReverseComplimentAnyCase(char*, char*, int);
char GetReverseComplimentAnyCaseBase(char);
int ValidateBasePair(char);
int IsAPowerOfTwo(unsigned int);
char TransformFromIUPAC(char);
void CheckRGIndexes(char**, int, char**, int, int, int32_t*, int32_t*, int32_t*, int32_t*);

#endif