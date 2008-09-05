#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#include "../blib/BLibDefinitions.h"
#include "../blib/BError.h"
#include "../blib/RGIndex.h"
#include "bexonify.h"

#define Name "bexonify"

/* Modifies an index to only include locations specified
 * by the user.  The main purpose is to allow for 
 * alignment to exons, multiple subregions or the like.
 * */

int main(int argc, char *argv[]) 
{
	/* I have to admit, this is kind of a hack, we could 
	 * just modify the data structure of the index.  Oh well.
	 * */
	if(argc == 4) {

		FILE *fp;
		char rgFileName[MAX_FILENAME_LENGTH]="\0";
		char indexFileName[MAX_FILENAME_LENGTH]="\0";
		char exonsFileName[MAX_FILENAME_LENGTH]="\0";
		char outputFileName[MAX_FILENAME_LENGTH]="\0";

		RGBinary rg;
		RGIndex index;
		Exon *exons=NULL;
		int numExons = 0;

		strcpy(rgFileName, argv[1]);
		strcpy(indexFileName, argv[2]);
		strcpy(exonsFileName, argv[3]);

		/* Read in the rg binary file */
		RGBinaryReadBinary(&rg, rgFileName);

		/* Read the index */
		fprintf(stderr, "Reading in index from %s.\n",
				indexFileName);
		if(!(fp=fopen(indexFileName, "rb"))) {
			PrintError(Name,
					indexFileName,
					"Could not open file for reading",
					Exit,
					OpenFileError);
		}
		RGIndexRead(fp, &index, 1);
		fclose(fp);

		/* Read in the exons */
		numExons = ReadExons(exonsFileName, &exons);

		/* Filter based on the exons */
		FilterIndexBasedOnExons(&index, &exons, numExons);

		/* Free exons */
		free(exons);
		exons=NULL;
		numExons = 0;

		/* We need to update the hash */
		/* Free hash */
		free(index.starts);
		index.starts=NULL;
		free(index.ends);
		index.ends=NULL;

		/* Fix the hash by recreating it */
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Regenerating the hash.\n");
		RGIndexCreateHash(&index, &rg);

		/* Create new file name */
		sprintf(outputFileName, "%s.index.file.%s.%s",
				PROGRAM_NAME,
				Name,
				BFAST_INDEX_FILE_EXTENSION);

		/* Print the new index */ 
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Outputting to %s.\n", outputFileName);
		if(!(fp=fopen(outputFileName, "wb"))) {
			PrintError(Name,
					outputFileName,
					"Could not open file for writing",
					Exit,
					OpenFileError);
		}
		RGIndexPrint(fp, &index, 1);
		fclose(fp);

		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Cleaning up.\n");
		/* Delete the index */
		RGIndexDelete(&index);
		/* Delete the rg */
		RGBinaryDelete(&rg);
	}
	else {
		fprintf(stderr, "%s [OPTIONS]\n", Name);
		fprintf(stderr, "\t<bfast reference genome file>\n");
		fprintf(stderr, "\t<bfast index file>\n");
		fprintf(stderr, "\t<exon list file>\n");
	}

	return 0;
}

int ReadExons(char *exonsFileName,
		Exon **exons)
{
	char *FnName = "ReadExons";
	FILE *fp;
	uint8_t chr, prevChr;
	uint32_t start, end, tmpUint32_t, prevStart, prevEnd;
	int numExons = 0;

	/* Open the file */
	fprintf(stderr, "Reading in exons from %s.\n",
			exonsFileName);
	if(!(fp=fopen(exonsFileName, "rb"))) {
		PrintError(FnName,
				exonsFileName,
				"Could not open file for reading",
				Exit,
				OpenFileError);
	}

	/* Read in the exons */ 
	prevChr = 0;
	prevStart = prevEnd = 0;
	while(EOF!=fscanf(fp, "%u %u %u", &tmpUint32_t, &start, &end)) {
		chr = tmpUint32_t;

		/* Check that the exons in increasing order */
		if(chr < prevChr || 
				(chr == prevChr && start < prevStart) ||
				(chr == prevChr && start == prevStart && end <= prevEnd)) {
			PrintError(FnName,
					NULL,
					"Entries must be in increasing order with the keys=(chr, start, end)",
					Exit,
					OutOfRange);
		}

		numExons++;
		(*exons) = realloc((*exons), sizeof(Exon)*numExons);
		if(NULL == (*exons)) {
			PrintError(FnName,
					"(*exons)",
					"Could not allocate memory",
					Exit,
					MallocMemory);
		}
		assert(start <= end);
		(*exons)[numExons-1].chr = chr;
		(*exons)[numExons-1].start = start;
		(*exons)[numExons-1].end = end;
	}

	/* Close the file */
	fclose(fp);

	fprintf(stderr, "Read in %d exons.\n",
			numExons);

	return numExons;
}
		
void FilterIndexBasedOnExons(RGIndex *index, Exon **exons, int numExons) 
{
	char *FnName = "FilterIndexBasedOnExons";
	int64_t i, j;
	int64_t low, mid, high, found;

	/* Go through each entry in the index */
	for(i=index->length-1;i>=0;i--) {
		/* Check if it falls within range */
		/* Binary search */
		low = 0;
		high = numExons-1;
		found = 0;
		while(low <= high && found == 0) {
			mid = (low + high/2);
			if(index->chromosomes[i] < (*exons)[mid].chr ||
				 (index->chromosomes[i] == (*exons)[mid].chr && index->positions[i] < (*exons)[mid].start)) {
				high = mid - 1;
			}
			else if(index->chromosomes[i] > (*exons)[mid].chr ||
				 (index->chromosomes[i] == (*exons)[mid].chr && index->positions[i] > (*exons)[mid].end)) {
				low = mid + 1;
			}
			else {
				found = 1;
			}
		}
		/* If not found, remove and shift */
		if(found == 0) {
			for(j=i+1;j<index->length;j++) {
				index->chromosomes[j-1] = index->chromosomes[j];
				index->positions[j-1] = index->positions[j];
			}
			/* Decrement index length, reallocate later */
			index->length--;
		}
	}

	/* Reallocate */
	index->chromosomes = realloc(index->chromosomes, index->length*sizeof(uint8_t));
	if(NULL==index->chromosomes) {
		PrintError(FnName,
				"index->chromosomes",
				"Could not reallocate memory",
				Exit,
				ReallocMemory);
	}
	index->positions = realloc(index->positions, index->length*sizeof(uint32_t));
	if(NULL==index->positions) {
		PrintError(FnName,
				"index->positions",
				"Could not reallocate memory",
				Exit,
				ReallocMemory);
	}

	/* Update index range */
	index->startChr = (*exons)[0].chr;
	index->startPos = (*exons)[0].start;
	index->endChr = (*exons)[numExons-1].chr;
	index->endPos = (*exons)[numExons-1].end;

}
