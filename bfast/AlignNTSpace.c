#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "BLibDefinitions.h"
#include "BLib.h"
#include "BError.h"
#include "RGMatches.h"
#include "AlignedEntry.h"
#include "ScoringMatrix.h"
#include "Align.h"
#include "AlignNTSpace.h"

/* TODO */
void AlignNTSpaceMismatchesOnly(char *read,
		int readLength,
		char *reference,
		int referenceLength,
		ScoringMatrix *sm,
		AlignedEntry *a,
		char strand,
		int32_t position)
{
	char *FnName = "AlignNTSpaceMismatchesOnly";
	/* Read goes on the second row, reference on the first */
	int i, j;
	int32_t maxScore = NEGATIVE_INFINITY;
	int offset=-1;
	int32_t curScore = 0.0;
	
	assert(readLength <= referenceLength);

	for(i=0;i<referenceLength-readLength+1;i++) { /* Starting position */
		curScore = 0.0;
		for(j=0;j<readLength;j++) { /* Position in the alignment */
			curScore += ScoringMatrixGetNTScore(read[j], reference[i+j], sm);
		}
		if(maxScore < curScore) {
			maxScore = curScore;
			offset = i;
		}
	}

	/* Copy over */
	a->referenceLength = readLength;
	a->length = readLength;
	a->score = maxScore;
	/* Allocate memory */
	assert(NULL==a->read);
	a->read = malloc(sizeof(char)*(a->length+1));
	if(NULL==a->read) {
		PrintError(FnName, "a->read", "Could not allocate memory", Exit, MallocMemory);
	}
	assert(NULL==a->reference);
	a->reference = malloc(sizeof(char)*(a->length+1));
	if(NULL==a->reference) {
		PrintError(FnName, "a->reference", "Could not allocate memory", Exit, MallocMemory);
	}
	assert(NULL==a->colorError);
	/*
	   a->colorError = malloc(sizeof(char)*SEQUENCE_LENGTH);
	   if(NULL==a->colorError) {
	   PrintError(FnName, "a->colorError", "Could not allocate memory", Exit, MallocMemory);
	   }
	   */
	/* Copy over */
	for(i=0;i<a->length;i++) {
		a->read[i] = read[i];
		a->reference[i] = reference[i+offset];
	}
	a->read[a->length] = '\0';
	a->reference[a->length] = '\0';
	a->position = (FORWARD==strand)?(position + referenceLength - readLength - offset):(position + offset);
}

/* TODO */
void AlignNTSpaceFull(char *read,
		int readLength,
		char *reference,
		int referenceLength,
		ScoringMatrix *sm,
		AlignedEntry *a,
		char strand,
		int32_t position)
{
	char *FnName = "AlignNTSpaceFull";
	/* read goes on the rows, reference on the columns */
	AlignMatrixNT **matrix=NULL;
	int offset = 0;
	int i, j;

	/* Allocate memory for the matrix */
	matrix = malloc(sizeof(AlignMatrixNT*)*(readLength+1));
	if(NULL==matrix) {
		PrintError(FnName, "matrix", "Could not allocate memory", Exit, MallocMemory);
	}
	for(i=0;i<readLength+1;i++) {
		matrix[i] = malloc(sizeof(AlignMatrixNT)*(referenceLength+1));
		if(NULL==matrix[i]) {
			PrintError(FnName, "matrix[i]", "Could not allocate memory", Exit, MallocMemory);
		}
	}

	/* Initialize "the matrix" */
	/* Row i (i>0) column 0 should be negative infinity since we want to
	 * align the full read */
	for(i=1;i<readLength+1;i++) {
		matrix[i][0].h.score = matrix[i][0].s.score = matrix[i][0].v.score = NEGATIVE_INFINITY;
		matrix[i][0].h.from = matrix[i][0].s.from = matrix[i][0].v.from = StartNT;
		matrix[i][0].h.length = matrix[i][0].s.length = matrix[i][0].v.length = 0;
	}
	/* Row 0 column j should be zero since we want to find the best
	 * local alignment within the reference */
	for(j=0;j<referenceLength+1;j++) {
		matrix[0][j].s.score = 0;
		matrix[0][j].h.score = matrix[0][j].v.score = NEGATIVE_INFINITY;
		matrix[0][j].h.from = matrix[0][j].s.from = matrix[0][j].v.from = StartNT;
		matrix[0][j].h.length = matrix[0][j].s.length = matrix[0][j].v.length = 0;
	}

	/* Fill in the matrix according to the recursive rules */
	for(i=0;i<readLength;i++) { /* read/rows */
		for(j=0;j<referenceLength;j++) { /* reference/columns */
			/* Deletion relative to reference across a column */
			/* Insertion relative to reference is down a row */
			/* Match/Mismatch is a diagonal */

			/* Update deletion */
			/* Deletion extension */
			matrix[i+1][j+1].h.score = matrix[i+1][j].h.score + sm->gapExtensionPenalty; 
			matrix[i+1][j+1].h.length = matrix[i+1][j].h.length + 1;
			matrix[i+1][j+1].h.from = DeletionExtension;
			/* Check if starting a new deletion is better */
			if(matrix[i+1][j+1].h.score < matrix[i+1][j].s.score + sm->gapOpenPenalty) {
				matrix[i+1][j+1].h.score = matrix[i+1][j].s.score + sm->gapOpenPenalty;
				matrix[i+1][j+1].h.length = matrix[i+1][j].s.length + 1;
				matrix[i+1][j+1].h.from = DeletionStart;
			}

			/* Update insertion */
			/* Insertion extension */
			matrix[i+1][j+1].v.score = matrix[i][j+1].v.score + sm->gapExtensionPenalty; 
			matrix[i+1][j+1].v.length = matrix[i][j+1].v.length + 1;
			matrix[i+1][j+1].v.from = InsertionExtension;
			/* Check if starting a new insertion is better */
			if(matrix[i+1][j+1].v.score < matrix[i][j+1].s.score + sm->gapOpenPenalty) {
				matrix[i+1][j+1].v.score = matrix[i][j+1].s.score + sm->gapOpenPenalty;
				matrix[i+1][j+1].v.length = matrix[i][j+1].s.length + 1;
				matrix[i+1][j+1].v.from = InsertionStart;
			}

			/* Update diagonal */
			/* Get mismatch score */
			matrix[i+1][j+1].s.score = matrix[i][j].s.score + ScoringMatrixGetNTScore(read[i], reference[j], sm);
			matrix[i+1][j+1].s.length = matrix[i][j].s.length + 1;
			matrix[i+1][j+1].s.from = Match;
			/* Get the maximum score of the three cases: horizontal, vertical and diagonal */
			if(matrix[i+1][j+1].s.score < matrix[i+1][j+1].h.score) {
				matrix[i+1][j+1].s.score = matrix[i+1][j+1].h.score;
				matrix[i+1][j+1].s.length = matrix[i+1][j+1].h.length;
				matrix[i+1][j+1].s.from = matrix[i+1][j+1].h.from;
			}
			if(matrix[i+1][j+1].s.score < matrix[i+1][j+1].v.score) {
				matrix[i+1][j+1].s.score = matrix[i+1][j+1].v.score;
				matrix[i+1][j+1].s.length = matrix[i+1][j+1].v.length;
				matrix[i+1][j+1].s.from = matrix[i+1][j+1].v.from;
			}
		}
	}

	offset = FillAlignedEntryFromMatrixNTSpace(a,
			matrix,
			read,
			readLength,
			reference,
			referenceLength,
			0,
			0);

	/* Free the matrix, free your mind */
	for(i=0;i<readLength+1;i++) {
		free(matrix[i]);
		matrix[i]=NULL;
	}
	free(matrix);
	matrix=NULL;

	a->position = (FORWARD==strand)?(position + offset):(position + referenceLength - a->referenceLength - offset);
}

/* TODO */
void AlignNTSpaceFullWithBound(char *read,
		int readLength,
		char *reference,
		int referenceLength,
		ScoringMatrix *sm,
		AlignedEntry *a,
		char strand,
		int32_t position,
		int32_t maxH,
		int32_t maxV)
{
	char *FnName = "AlignNTSpaceFullWithBound";
	/* read goes on the rows, reference on the columns */
	AlignMatrixNT **matrix=NULL;
	int offset = 0;
	int i, j;

	assert(maxV >= 0 && maxH >= 0);

	/* Allocate memory for the matrix */
	matrix = malloc(sizeof(AlignMatrixNT*)*(readLength+1));
	if(NULL==matrix) {
		PrintError(FnName, "matrix", "Could not allocate memory", Exit, MallocMemory);
	}
	for(i=0;i<readLength+1;i++) {
		matrix[i] = malloc(sizeof(AlignMatrixNT)*(referenceLength+1));
		if(NULL==matrix[i]) {
			PrintError(FnName, "matrix[i]", "Could not allocate memory", Exit, MallocMemory);
		}
	}

	/* Initialize "the matrix" */
	/* Row i (i>0) column 0 should be negative infinity since we want to
	 * align the full read */
	for(i=1;i<readLength+1;i++) {
		matrix[i][0].h.score = matrix[i][0].s.score = matrix[i][0].v.score = NEGATIVE_INFINITY;
		matrix[i][0].h.from = matrix[i][0].s.from = matrix[i][0].v.from = StartNT;
		matrix[i][0].h.length = matrix[i][0].s.length = matrix[i][0].v.length = 0;
	}
	/* Row 0 column j should be zero since we want to find the best
	 * local alignment within the reference */
	for(j=0;j<referenceLength+1;j++) {
		matrix[0][j].s.score = 0;
		matrix[0][j].h.score = matrix[0][j].v.score = NEGATIVE_INFINITY;
		matrix[0][j].h.from = matrix[0][j].s.from = matrix[0][j].v.from = StartNT;
		matrix[0][j].h.length = matrix[0][j].s.length = matrix[0][j].v.length = 0;
	}

	/* Fill in the matrix according to the recursive rules */
	for(i=0;i<readLength;i++) { /* read/rows */
		for(j=GETMAX(0, i - maxV);
				j <= GETMIN(referenceLength-1, referenceLength - (readLength - maxH) + i);
				j++) { /* reference/columns */
			assert(i-maxV <= j && j <= referenceLength - (readLength - maxH) + i);

			/* Deletion relative to reference across a column */
			/* Insertion relative to reference is down a row */
			/* Match/Mismatch is a diagonal */

			/* Update deletion */
			if(maxV == i - j) {
				matrix[i+1][j+1].h.score = NEGATIVE_INFINITY;
				matrix[i+1][j+1].h.length = INT_MIN;
				matrix[i+1][j+1].h.from = NoFromNT;
			}
			else {
				/* Deletion extension */
				matrix[i+1][j+1].h.score = matrix[i+1][j].h.score + sm->gapExtensionPenalty; 
				matrix[i+1][j+1].h.length = matrix[i+1][j].h.length + 1;
				matrix[i+1][j+1].h.from = DeletionExtension;
				/* Check if starting a new deletion is better */
				if(matrix[i+1][j+1].h.score < matrix[i+1][j].s.score + sm->gapOpenPenalty) {
					matrix[i+1][j+1].h.score = matrix[i+1][j].s.score + sm->gapOpenPenalty;
					matrix[i+1][j+1].h.length = matrix[i+1][j].s.length + 1;
					matrix[i+1][j+1].h.from = DeletionStart;
				}
			}

			/* Update insertion */
			if(j == referenceLength - (readLength - maxH) + i) {
				matrix[i+1][j+1].v.score = NEGATIVE_INFINITY;
				matrix[i+1][j+1].v.length = INT_MIN;
				matrix[i+1][j+1].v.from = NoFromNT;
			}
			else {
				/* Insertion extension */
				matrix[i+1][j+1].v.score = matrix[i][j+1].v.score + sm->gapExtensionPenalty; 
				matrix[i+1][j+1].v.length = matrix[i][j+1].v.length + 1;
				matrix[i+1][j+1].v.from = InsertionExtension;
				/* Check if starting a new insertion is better */
				if(matrix[i+1][j+1].v.score < matrix[i][j+1].s.score + sm->gapOpenPenalty) {
					matrix[i+1][j+1].v.score = matrix[i][j+1].s.score + sm->gapOpenPenalty;
					matrix[i+1][j+1].v.length = matrix[i][j+1].s.length + 1;
					matrix[i+1][j+1].v.from = InsertionStart;
				}
			}

			/* Update diagonal */
			/* Get mismatch score */
			matrix[i+1][j+1].s.score = matrix[i][j].s.score + ScoringMatrixGetNTScore(read[i], reference[j], sm);
			matrix[i+1][j+1].s.length = matrix[i][j].s.length + 1;
			matrix[i+1][j+1].s.from = Match;
			/* Get the maximum score of the three cases: horizontal, vertical and diagonal */
			if(matrix[i+1][j+1].s.score < matrix[i+1][j+1].h.score) {
				matrix[i+1][j+1].s.score = matrix[i+1][j+1].h.score;
				matrix[i+1][j+1].s.length = matrix[i+1][j+1].h.length;
				matrix[i+1][j+1].s.from = matrix[i+1][j+1].h.from;
			}
			if(matrix[i+1][j+1].s.score < matrix[i+1][j+1].v.score) {
				matrix[i+1][j+1].s.score = matrix[i+1][j+1].v.score;
				matrix[i+1][j+1].s.length = matrix[i+1][j+1].v.length;
				matrix[i+1][j+1].s.from = matrix[i+1][j+1].v.from;
			}
		}
	}

	offset = FillAlignedEntryFromMatrixNTSpace(a,
			matrix,
			read,
			readLength,
			reference,
			referenceLength,
			readLength - maxV,
			0);

	/* Free the matrix, free your mind */
	for(i=0;i<readLength+1;i++) {
		free(matrix[i]);
		matrix[i]=NULL;
	}
	free(matrix);
	matrix=NULL;

	/* Debug code */
	/*
	AlignedEntry tmp;
	AlignedEntryInitialize(&tmp);
	AlignNTSpaceFull(read,
			readLength,
			reference,
			referenceLength,
			sm,
			&tmp,
			strand,
			position);
	if(a->score < tmp.score ||
			tmp.score < a->score ||
			!(a->length == tmp.length) ||
			!(a->referenceLength == tmp.referenceLength)) {
		fprintf(stderr, "\nreferenceLength=%d\n", referenceLength);
		fprintf(stderr, "\nstrand=%c\n", strand);
		AlignedEntryPrint(a,
				stderr,
				ColorSpace,
				TextOutput);
		AlignedEntryPrint(&tmp,
				stderr,
				ColorSpace,
				TextOutput);
		PrintError(FnName, NULL, "Alignments did not match", Exit, OutOfRange);
	}
	AlignedEntryFree(&tmp);
	*/

	a->position = (FORWARD==strand)?(position + offset):(position + referenceLength - a->referenceLength - offset);
}

/* TODO */
int FillAlignedEntryFromMatrixNTSpace(AlignedEntry *a,
		AlignMatrixNT **matrix,
		char *read,
		int readLength,
		char *reference,
		int referenceLength,
		int toExclude,
		int debug)
{
	char *FnName="FillAlignedEntryFromMatrixNTSpace";
	int curRow, curCol, startRow, startCol;
	char curReadBase;
	int nextRow, nextCol;
	char nextReadBase;
	int curFrom;
	double maxScore;
	int i;
	int offset;

	curReadBase = nextReadBase = 'X';
	nextRow = nextCol = -1;

	assert(0 <= toExclude);

	/* Get the best alignment.  We can find the best score in the last row and then
	 * trace back.  We choose the best score from the last row since we want to 
	 * align the read completely and only locally to the reference. */
	startRow=-1;
	startCol=-1;
	maxScore = NEGATIVE_INFINITY;
	for(i=toExclude;i<referenceLength+1;i++) {
		/* Check only the first cell */
		if(maxScore < matrix[readLength][i].s.score) {
			maxScore = matrix[readLength][i].s.score;
			startRow = readLength;
			startCol = i;
		}
	}
	assert(startRow >= 0 && startCol >= 0);

	/* Initialize variables for the loop */
	curRow=startRow;
	curCol=startCol;
	curFrom = Match;
	a->referenceLength=0;
	i=matrix[curRow][curCol].s.length-1; /* Get the length of the alignment */
	a->length=matrix[curRow][curCol].s.length; /* Copy over the length */
	a->score = maxScore; /* Copy over score */

	/* Allocate memory */
	assert(NULL==a->read);
	a->read = malloc(sizeof(char)*(a->length+1));
	if(NULL==a->read) {
		PrintError(FnName, "a->read", "Could not allocate memory", Exit, MallocMemory);
	}
	assert(NULL==a->reference);
	a->reference = malloc(sizeof(char)*(a->length+1));
	if(NULL==a->reference) { 
		PrintError(FnName, "a->reference", "Could not allocate memory", Exit, MallocMemory);
	}
	assert(NULL==a->colorError); 

	/* Now trace back the alignment using the "from" member in the matrix */
	while(curRow > 0 && curCol > 0) {

		/* Where did the current cell come from */
		switch(curFrom) {
			case DeletionStart:
				curFrom = matrix[curRow][curCol].s.from;
				assert(curFrom == Match || curFrom == InsertionExtension);
				break;
			case DeletionExtension:
				curFrom = matrix[curRow][curCol].h.from;
				assert(curFrom == DeletionStart || curFrom == DeletionExtension);
				break;
			case Match:
				curFrom = matrix[curRow][curCol].s.from;
				break;
			case InsertionStart:
				curFrom = matrix[curRow][curCol].s.from;
				assert(curFrom == Match || curFrom == DeletionExtension);
				break;
			case InsertionExtension:
				curFrom = matrix[curRow][curCol].v.from;
				if(!(curFrom == InsertionStart || curFrom == InsertionExtension)) {
					fprintf(stderr, "\ncurFrom=%d\n",
							curFrom);
					fprintf(stderr, "toExclude=%d\n",
							toExclude);
					fprintf(stderr, "readLength=%d\n",
							readLength);
					fprintf(stderr, "startRow=%d\nstartCol=%d\n",
							startRow,
							startCol);
					fprintf(stderr, "curRow=%d\ncurCol=%d\n",
							curRow,
							curCol);
				}
				assert(curFrom == InsertionStart || curFrom == InsertionExtension);
				break;
			default:
				PrintError(FnName, "curFrom", "Could not recognize curFrom", Exit, OutOfRange);
		}

		assert(i>=0);

		/* Update alignment */
		switch(curFrom) {
			case DeletionStart:
			case DeletionExtension:
				a->read[i] = GAP;
				a->reference[i] = reference[curCol-1];
				a->referenceLength++;
				nextRow = curRow;
				nextCol = curCol-1;
				break;
			case Match:
				a->read[i] = read[curRow-1];
				a->reference[i] = reference[curCol-1];
				a->referenceLength++;
				nextRow = curRow-1;
				nextCol = curCol-1;
				break;
			case InsertionStart:
			case InsertionExtension:
				a->read[i] = read[curRow-1];
				a->reference[i] = GAP;
				nextRow = curRow-1;
				nextCol = curCol;
				break;
			default:
				fprintf(stderr, "curFrom=%d\n", curFrom);
				PrintError(FnName, "curFrom", "Could not understand curFrom", Exit, OutOfRange);
		}

		assert(a->read[i] != GAP || a->read[i] != a->reference[i]);

		/* Update for next loop iteration */
		curRow = nextRow;
		curCol = nextCol;
		i--;

		assert(i!=-1 || a->read[0] != GAP);
	} /* End Loop */
	assert(-1==i);
	assert(a->length >= a->referenceLength);
	assert(a->length >= readLength);

	a->read[a->length]='\0';
	a->reference[a->length]='\0';

	offset = curCol;

	return offset;
}