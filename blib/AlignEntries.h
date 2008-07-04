#ifndef ALIGNENTRIES_H_
#define ALIGNENTRIES_H_

#include <stdio.h>
#include "BLibDefinitions.h"
#include "AlignEntry.h"
#include "RGBinary.h"

/* TODO */
typedef struct {
	char *readName;
	AlignEntry *entriesOne;
	int numEntriesOne;
	AlignEntry *entriesTwo;
	int numEntriesTwo;
	int pairedEnd;
} AlignEntries;

void AlignEntriesPrint(AlignEntries*, FILE*);
int AlignEntriesRead(AlignEntries*, FILE*);
void AlignEntriesRemoveDuplicates(AlignEntries*, int);
void AlignEntriesQuickSort(AlignEntries*, int, int);
void AlignEntriesMergeSort(AlignEntries*, int, int);
void AlignEntriesAllocate(AlignEntries*, int, int, int);
void AlignEntriesReallocate(AlignEntries*, int, int, int);
void AlignEntriesFree(AlignEntries*);
void AlignEntriesInitialize(AlignEntries*);
#endif
