#ifndef BGETERRORDISTRIBUTION_H_
#define BGETERRORDISTRIBUTION_H_

/* TODO */
typedef struct {
	int length;
	int *countOne;
	int *countTwo;
} Count;

/* Structure to hold data about the error distributions
 * */
typedef struct {
	int numReads;
	int space;
	Count by[2]; /* nt errors by position and color errors by position */
	Count across[2]; /* nt errors across reads and color errors across reads */
} Errors;

void GetErrorDistribution(char*, char*);

void ErrorsPrint(Errors*, FILE**, int);
void ErrorsUpdate(Errors*, AlignEntries *a);
void ErrorsUpdateHelper(Errors*, AlignEntry *a, int, int, int);
void ErrorsInitialize(Errors*);
void ErrorsFree(Errors*);

void CountPrint(Count*, FILE*, int);
void CountUpdate(Count*, int, int, int);
void CountInitialize(Count*);
void CountFree(Count*);

#endif
