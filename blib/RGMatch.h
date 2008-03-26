#ifndef RGMATCH_H_
#define RGMATCH_H_

#include <stdio.h>
#include "BLibDefinitions.h"
#include "RGTree.h"
#include "RGSeqPair.h"

void RGMatchRemoveDuplicates(RGMatch*);
void RGMatchMergeSort(RGMatch*, int, int);
void RGMatchOutputToFile(FILE*, char*, char*, char*, RGMatch*, RGMatch*, int);
void RGMatchMergeFilesAndOutput(FILE**, int, FILE*, int);
int RGMatchGetNextFromFile(FILE*, char*, char*, char*, RGMatch*, RGMatch*, int);

#endif

