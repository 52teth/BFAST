#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "BError.h"
#include "BLib.h"
#include "BLibDefinitions.h"
#include "RGBinary.h"

/* TODO */
/* Read from fasta file */
void RGBinaryRead(char *rgFileName, 
		RGBinary *rg,
		int32_t colorSpace)
{
	char *FnName="RGBinaryRead";
	FILE *fpRG=NULL;
	int8_t c;
	int8_t original;
	int64_t numPosRead=0;
	int32_t continueReadingContig;
	int32_t byteIndex;
	int32_t numCharsPerByte;
	fpos_t curFilePos;

	char header[MAX_FILENAME_LENGTH]="\0";
	char prevBase = COLOR_SPACE_START_NT; /* For color space */

	/* We assume that we can hold 2 [acgt] (nts) in each byte */
	assert(ALPHABET_SIZE==4);
	numCharsPerByte=ALPHABET_SIZE/2;

	/* Initialize the data structure for holding the rg */
	rg->id=BFAST_ID;
	rg->contigs=NULL;
	rg->numContigs=0;
	rg->colorSpace=colorSpace;

	if(VERBOSE>=0) {
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Reading from %s.\n",
				rgFileName);
	}

	/* open file */
	if(!(fpRG=fopen(rgFileName, "rb"))) {
		PrintError(FnName,
				rgFileName,
				"Could not open file for reading",
				Exit,
				OpenFileError);
	}

	/*****/
	/* Read in the sequence for each contig. */
	/*****/
	if(VERBOSE >= 0) {
		fprintf(stderr, "Reading in [contig,pos]:\n[-1,-1]");
	}
	rg->numContigs=0;
	while(EOF!=fscanf(fpRG, ">%s", header)) {
		rg->numContigs++;

		if(VERBOSE>=0) {
			fprintf(stderr, "Reading in contig %d.\n",
					rg->numContigs);
		}

		/* Reallocate memory to store one more contig. */
		rg->contigs = realloc(rg->contigs, rg->numContigs*sizeof(RGBinaryContig));
		if(NULL == rg->contigs) {
			PrintError(FnName,
					"rg->contigs",
					"Could not reallocate memory",
					Exit,
					ReallocMemory);
		}
		/* Allocate memory for contig name */
		rg->contigs[rg->numContigs-1].contigNameLength=strlen(header);
		rg->contigs[rg->numContigs-1].contigName = malloc(sizeof(char)*rg->contigs[rg->numContigs-1].contigNameLength);
		if(NULL==rg->contigs[rg->numContigs-1].contigName) {
			PrintError(FnName,
					"rg->contigs[rg->numContigs-1].contigName",
					"Could not allocate memory",
					Exit,
					MallocMemory);
		}
		/* Copy over contig name */
		strcpy(rg->contigs[rg->numContigs-1].contigName, header); 
		/* Initialize contig */
		rg->contigs[rg->numContigs-1].sequence = NULL;
		rg->contigs[rg->numContigs-1].sequenceLength = 0;
		rg->contigs[rg->numContigs-1].numBytes = 0;

		/* Save the file position */
		if(0!=fgetpos(fpRG, &curFilePos)) {
			PrintError(FnName,
					"curFilePos",
					"Could not get the current file position",
					Exit,
					OutOfRange);
		}
		/* Read in contig. */
		prevBase = COLOR_SPACE_START_NT; /* For color space */
		continueReadingContig=1;
		while(continueReadingContig==1 && fscanf(fpRG, "%c", &original) > 0) {
			/* original - will be the original sequence.  Possibilities include:
			 * Non-repeat sequence: a,c,g,t
			 * Repeat sequence: A,C,G,T
			 * Null Character: N,n
			 * */

			/* Transform IUPAC codes */
			original=TransformFromIUPAC(original);

			if(original == '\n') {
				/* ignore */
			}
			else if(original == '>') {
				/* New contig, exit the loop and reset the file position */
				continueReadingContig = 0;
				if(0!=fsetpos(fpRG, &curFilePos)) {
					PrintError(FnName,
							"curFilePos",
							"Could not set current file position",
							Exit,
							OutOfRange);
				}
			}
			else {
				/* Convert to color space */
				if(1==colorSpace) {
					/* Convert color space to A,C,G,T */
					c = ConvertBaseToColorSpace(prevBase, original);
					/* Convert to nucleotide equivalent for storage */
					/* Store 0=A, 1=C, 2=G, 3=T, else N */
					c = ConvertColorToStorage(c);
					/* Update if it is a repeat */
					/* For repeat sequence, if both the previous base and 
					 * current base are non-repeat, the color is non-repeat.
					 * Otherwise, it is repeat sequence */
					if(RGBinaryIsBaseRepeat(prevBase) == 1 || RGBinaryIsBaseRepeat(original) == 1) {
						/* Repeat */
						prevBase = original; /* Update the previous base */
						original = ToUpper(c);
					}
					else {
						/* Non-repeat */
						prevBase = original; /* Update the previous base */
						original = ToLower(c);
					}
				}

				if(VERBOSE >= 0) {
					if(0==rg->contigs[rg->numContigs-1].sequenceLength % READ_ROTATE_NUM) {
						fprintf(stderr, "\r[%d,%d]",
								rg->numContigs,
								rg->contigs[rg->numContigs-1].sequenceLength);
					}
				}

				/* Validate base pair */
				if(ValidateBasePair(original)==0) {
					fprintf(stderr, "Base:[%c]\n", original);
					PrintError(FnName,
							"original",
							"Not a valid base pair",
							Exit,
							OutOfRange);
				}
				/* Get which byte to insert */
				byteIndex = rg->contigs[rg->numContigs-1].sequenceLength%numCharsPerByte;
				/* Check if we must allocate a new byte */
				if(byteIndex==0) {
					/* Update the number of bytes */
					rg->contigs[rg->numContigs-1].numBytes++;
					/* Reallocate a new byte */
					rg->contigs[rg->numContigs-1].sequence = realloc(rg->contigs[rg->numContigs-1].sequence, sizeof(uint8_t)*(rg->contigs[rg->numContigs-1].numBytes));
					if(NULL == rg->contigs[rg->numContigs-1].sequence) {
						PrintError(FnName,
								"rg->contigs[rg->numContigs-1].sequence",
								"Could not reallocate memory",
								Exit,
								ReallocMemory);
					}
					/* Initialize the byte */
					rg->contigs[rg->numContigs-1].sequence[rg->contigs[rg->numContigs-1].numBytes-1] = 0;
				}
				/* Insert the sequence correctly (as opposed to incorrectly) */
				RGBinaryInsertBase(&rg->contigs[rg->numContigs-1].sequence[rg->contigs[rg->numContigs-1].numBytes-1], byteIndex, original);
				rg->contigs[rg->numContigs-1].sequenceLength++;
				numPosRead++;
				if(rg->contigs[rg->numContigs-1].sequenceLength >= UINT_MAX) {
					PrintError(FnName,
							"sequenceLength",
							"Maximum sequence length for a given contig was reached",
							Exit,
							OutOfRange);
				}
			}
			/* Save the file position in case the next character is the beginning of the header */
			if(0!=fgetpos(fpRG, &curFilePos)) {
				PrintError(FnName,
						"curFilePos",
						"Could not get the current file position",
						Exit,
						OutOfRange);
			}
		}
		/* Update our our output */
		if(VERBOSE >= 0) {
			fprintf(stderr, "\r[%d,%d]", 
					rg->numContigs, 
					rg->contigs[rg->numContigs-1].sequenceLength);
		}

		/* Reallocate to reduce memory (fit exactly) */
		assert(numCharsPerByte==2);
		/* we must add one since there could be an odd number of positions */
		rg->contigs[rg->numContigs-1].sequence = realloc(rg->contigs[rg->numContigs-1].sequence, sizeof(int8_t)*(rg->contigs[rg->numContigs-1].numBytes));
		if(NULL == rg->contigs[rg->numContigs-1].sequence) {
			PrintError(FnName,
					"rg->contigs[numContigs-1].sequence",
					"Could not reallocate memory",
					Exit,
					ReallocMemory);
		}
		/* End loop for the current contig */
	}

	/* Close file */
	fclose(fpRG);

	if(VERBOSE>=0) {
		fprintf(stderr, "In total read %d contigs for a total of %lld bases\n",
				rg->numContigs,
				(long long int)numPosRead);
		fprintf(stderr, "%s", BREAK_LINE);
	}
}

/* TODO */
/* Read from file */
void RGBinaryReadBinary(RGBinary *rg,
		char *rgFileName)
{
	char *FnName="RGBinaryReadBinary";
	FILE *fpRG;
	int i;
	int32_t numCharsPerByte;
	int64_t numPosRead=0;
	/* We assume that we can hold 2 [acgt] (nts) in each byte */
	assert(ALPHABET_SIZE==4);
	numCharsPerByte=ALPHABET_SIZE/2;

	if(VERBOSE>=0) {
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Reading in reference genome from %s.\n", rgFileName);
	}

	/* Open output file */
	if((fpRG=fopen(rgFileName, "rb"))==0) {
		PrintError(FnName,
				rgFileName,
				"Could not open rgFileName for writing",
				Exit,
				OpenFileError);
	}

	/* Read RGBinary information */
	if( fread(&rg->id, sizeof(int32_t), 1, fpRG)!=1 ||
			fread(&rg->numContigs, sizeof(int32_t), 1, fpRG)!=1 ||
			fread(&rg->colorSpace, sizeof(int32_t), 1, fpRG)!=1) {
		PrintError(FnName,
				NULL,
				"Could not read RGBinary information",
				Exit,
				ReadFileError);
	}

	/* Check id */
	if(BFAST_ID != rg->id) {
		PrintError(FnName,
				"rg->id",
				"The id did not match",
				Exit,
				OutOfRange);
	}

	assert(rg->numContigs > 0);
	assert(rg->colorSpace == NTSpace|| rg->colorSpace == ColorSpace);

	/* Allocate memory for the contigs */
	rg->contigs = malloc(sizeof(RGBinaryContig)*rg->numContigs);
	if(NULL==rg->contigs) {
		PrintError(FnName,
				"rg->contigs",
				"Could not allocate memory",
				Exit,
				MallocMemory);
	}

	/* Read each contig */
	for(i=0;i<rg->numContigs;i++) {
		/* Read contig name length */
		if(fread(&rg->contigs[i].contigNameLength, sizeof(int32_t), 1, fpRG)!=1) {
			PrintError(FnName,
					NULL,
					"Could not read contig name length",
					Exit,
					ReadFileError);
		}
		assert(rg->contigs[i].contigNameLength > 0);
		/* Read RGContig information */
		if(fread(&rg->contigs[i].contigName, sizeof(int8_t), rg->contigs[i].contigNameLength, fpRG) ||
				fread(&rg->contigs[i].sequenceLength, sizeof(int32_t), 1, fpRG)!=1 ||
				fread(&rg->contigs[i].numBytes, sizeof(uint32_t), 1, fpRG)!=1) {
			PrintError(FnName,
					NULL,
					"Could not read RGContig information",
					Exit,
					ReadFileError);
		}
		/* Allocate memory for the sequence */
		rg->contigs[i].sequence = malloc(sizeof(uint8_t)*rg->contigs[i].numBytes);
		if(NULL==rg->contigs[i].sequence) {
			PrintError(FnName,
					"rg->contigs[i].sequence",
					"Could not allocate memory",
					Exit,
					MallocMemory);
		}
		/* Read sequence */
		if(fread(rg->contigs[i].sequence, sizeof(uint8_t), rg->contigs[i].numBytes, fpRG)!=rg->contigs[i].numBytes) {
			PrintError(FnName,
					NULL,
					"Could not read sequence",
					Exit,
					ReadFileError);
		}
		numPosRead += rg->contigs[i].sequenceLength;
	}

	/* Close the output file */
	fclose(fpRG);

	if(VERBOSE>=0) {
		fprintf(stderr, "In total read %d contigs for a total of %lld bases\n",
				rg->numContigs,
				(long long int)numPosRead);
		fprintf(stderr, "%s", BREAK_LINE);
	}
}

void RGBinaryWriteBinary(RGBinary *rg,
		char *rgFileName) 
{
	char *FnName="RGBinaryWriteBinary";
	FILE *fpRG;
	int i;
	int32_t numCharsPerByte;
	/* We assume that we can hold 2 [acgt] (nts) in each byte */
	assert(ALPHABET_SIZE==4);
	numCharsPerByte=ALPHABET_SIZE/2;

	fprintf(stderr, "%s", BREAK_LINE);
	fprintf(stderr, "Outputting to %s\n", rgFileName);

	/* Open output file */
	if((fpRG=fopen(rgFileName, "wb"))==0) {
		PrintError(FnName,
				rgFileName,
				"Could not open rgFileName for writing",
				Exit,
				OpenFileError);
	}

	/* Output RGBinary information */
	if(fwrite(&rg->id, sizeof(int32_t), 1, fpRG)!=1 ||
			fwrite(&rg->numContigs, sizeof(int32_t), 1, fpRG)!=1 ||
			fwrite(&rg->colorSpace, sizeof(int32_t), 1, fpRG)!=1) {
		PrintError(FnName,
				NULL,
				"Could not output rg header",
				Exit,
				WriteFileError);
	}

	/* Output each contig */
	for(i=0;i<rg->numContigs;i++) {
		/* Output RGContig information */
		if(fwrite(&rg->contigs[i].contigNameLength, sizeof(int32_t), 1, fpRG) != 1 ||
				fwrite(&rg->contigs[i].contigName, sizeof(int8_t), rg->contigs[i].contigNameLength, fpRG) != 1 ||
				fwrite(&rg->contigs[i].sequenceLength, sizeof(int32_t), 1, fpRG) != 1 ||
				fwrite(&rg->contigs[i].numBytes, sizeof(uint32_t), 1, fpRG) != 1 ||
				/* Output sequence */
				fwrite(rg->contigs[i].sequence, sizeof(uint8_t), rg->contigs[i].numBytes, fpRG) != rg->contigs[i].numBytes) {
			PrintError(FnName,
					NULL,
					"Could not output rg contig",
					Exit,
					WriteFileError);
		}
	}
	fclose(fpRG);

	fprintf(stderr, "Output complete.\n");
	fprintf(stderr, "%s", BREAK_LINE);
}

/* TODO */
void RGBinaryDelete(RGBinary *rg)
{
	int32_t i;

	/* Free each contig */
	for(i=0;i<rg->numContigs;i++) {
		free(rg->contigs[i].sequence);
		rg->contigs[i].sequence=NULL;
	}
	/* Free the contigs */
	free(rg->contigs);
	rg->contigs = NULL;

	/* Initialize structure */
	rg->id = 0;
	rg->numContigs = 0;
	rg->colorSpace = 0;
}

/* TODO */
void RGBinaryInsertBase(uint8_t *dest,
		int32_t byteIndex,
		int8_t base)
{
	/*********************************
	 * In four bits we hold two no:
	 *
	 * left two bits:
	 * 		0 - No repat and [acgt]
	 * 		1 - Repeat and [acgt]
	 * 		2 - [nN]
	 * 		3 - undefined
	 *
	 * right-most:
	 * 		0 - aAnN
	 * 		1 - cC
	 * 		2 - gG
	 * 		3 - tt
	 *********************************
	 * */
	int32_t numCharsPerByte;
	/* We assume that we can hold 2 [acgt] (nts) in each byte */
	assert(ALPHABET_SIZE==4);
	numCharsPerByte=ALPHABET_SIZE/2;

	switch(byteIndex%numCharsPerByte) {
		case 0:
			(*dest)=0;
			/* left-most 2-bits will hold the repeat*/
			switch(base) {
				case 'a':
				case 'c':
				case 'g':
				case 't':
					/* zero */
					(*dest) = (*dest) | 0x00;
					break;
				case 'A':
				case 'C':
				case 'G':
				case 'T':
					/* one */
					(*dest) = (*dest) | 0x40;
					break;
				case 'N':
				case 'n':
					/* two */
					(*dest) = (*dest) | 0x80;
					break;
				default:
					PrintError("RGBinaryInsertSequenceLetterIntoByte",
							NULL,
							"Could not understand case 0 base",
							Exit,
							OutOfRange);
			}
			/* third and fourth bits from the left will hold the sequence */
			switch(base) {
				case 'N':
				case 'n': /* This does not matter if the base is an n */
				case 'A':
				case 'a':
					(*dest) = (*dest) | 0x00;
					break;
				case 'C':
				case 'c':
					(*dest) = (*dest) | 0x10;
					break;
				case 'G':
				case 'g':
					(*dest) = (*dest) | 0x20;
					break;
				case 'T':
				case 't':
					(*dest) = (*dest) | 0x30;
					break;
				default:
					PrintError("RGBinaryInsertSequenceLetterIntoByte",
							NULL,
							"Could not understand case 0 base",
							Exit,
							OutOfRange);
					break;
			}
			break;
		case 1:
			/* third and fourth bits from the right will hold the repeat*/
			switch(base) {
				case 'a':
				case 'c':
				case 'g':
				case 't':
					/* zero */
					(*dest) = (*dest) | 0x00;
					break;
				case 'A':
				case 'C':
				case 'G':
				case 'T':
					/* one */
					(*dest) = (*dest) | 0x04;
					break;
				case 'N':
				case 'n':
					/* two */
					(*dest) = (*dest) | 0x08;
					break;
				default:
					PrintError("RGBinaryInsertSequenceLetterIntoByte",
							NULL,
							"Could not understand case 1 repeat",
							Exit,
							OutOfRange);
			}
			/* right most 2-bits will hold the sequence */
			switch(base) {
				case 'N':
				case 'n': /* This does not matter if the base is an n */
				case 'A':
				case 'a':
					(*dest) = (*dest) | 0x00;
					break;
				case 'C':
				case 'c':
					(*dest) = (*dest) | 0x01;
					break;
				case 'G':
				case 'g':
					(*dest) = (*dest) | 0x02;
					break;
				case 'T':
				case 't':
					(*dest) = (*dest) | 0x03;
					break;
				default:
					PrintError("RGBinaryInsertSequenceLetterIntoByte",
							NULL,
							"Could not understand case 1 base",
							Exit,
							OutOfRange);
			}
			break;
		default:
			PrintError("RGBinaryInsertSequenceLetterIntoByte",
					NULL,
					"Could not understand byteIndex",
					Exit,
					OutOfRange);
	}
}

/* TODO */
int32_t RGBinaryGetSequence(RGBinary *rg,
		int32_t contig,
		int32_t position,
		int8_t strand,
		char **sequence,
		int32_t sequenceLength)
{
	char *FnName="RGBinaryGetSequence";
	char *reverseCompliment;
	int32_t curPos;

	/* We assume that we can hold 2 [acgt] (nts) in each byte */
	assert(ALPHABET_SIZE==4);
	assert(contig > 0 && contig <= rg->numContigs);

	/* Allocate memory for the reference */
	assert((*sequence)==NULL);
	(*sequence) = malloc(sizeof(char)*(sequenceLength+1));
	if(NULL==(*sequence)) {
		PrintError(FnName,
				"sequence",
				"Could not allocate memory",
				Exit,
				MallocMemory);
	}

	/* Copy over bases */
	for(curPos=position;curPos < position + sequenceLength;curPos++) {
		(*sequence)[curPos-position] = RGBinaryGetBase(rg, contig, curPos);
		if(0==(*sequence)[curPos-position]) {
			/* Free memory */
			free((*sequence));
			(*sequence) = NULL;
			return 0;
		}
	}
	(*sequence)[sequenceLength] = '\0';

	/* Get the reverse compliment if necessary */
	if(strand == FORWARD) {
		/* ignore */
	}
	else if(strand == REVERSE) {
		/* Get the reverse compliment */
		reverseCompliment = malloc(sizeof(char)*(sequenceLength+1));
		if(NULL == reverseCompliment) {
			PrintError(FnName,
					"reverseCompliment",
					"Could not allocate memory",
					Exit,
					MallocMemory);
		}
		GetReverseComplimentAnyCase((*sequence), reverseCompliment, sequenceLength);
		/* Copy reverse compliment to the sequence.  We could just strcpy or we 
		 * could just use the wonderful world of pointers */
		free((*sequence)); /* Free memory pointed to by sequence */
		(*sequence) = reverseCompliment; /* Point sequence to reverse compliment's memory */
		reverseCompliment=NULL; /* Destroy the pointer for reverse compliment */
	}
	else {
		PrintError(FnName,
				"strand",
				"Could not understand strand",
				Exit,
				OutOfRange);
	}
	return 1;
}

/* TODO */
void RGBinaryGetReference(RGBinary *rg,
		int32_t contig,
		int32_t position,
		int8_t strand,
		int32_t offsetLength,
		char **reference,
		int32_t readLength,
		int32_t *returnReferenceLength,
		int32_t *returnPosition)
{
	char *FnName="RGBinaryGetReference";
	int32_t startPos, endPos;
	int success;

	/* We assume that we can hold 2 [acgt] (nts) in each byte */
	assert(ALPHABET_SIZE==4);
	assert(contig > 0 && contig <= rg->numContigs);

	/* Get bounds for the sequence to return */
	startPos = position - offsetLength;
	endPos = position + readLength - 1 + offsetLength;

	/* Check contig bounds */
	if(contig < 1 || contig > rg->numContigs) {
		PrintError(FnName,
				NULL,
				"Contigomosome is out of range",
				Exit,
				OutOfRange);
	}

	/* Check position bounds */
	if(startPos < 1) {
		startPos = 1;
	}
	if(endPos > rg->contigs[contig-1].sequenceLength) {
		endPos = rg->contigs[contig-1].sequenceLength;
	}

	/* Check that enough bases remain */
	if(endPos - startPos + 1 <= 0) {
		/* Return just one base = N */
		assert((*reference)==NULL);
		(*reference) = malloc(sizeof(char)*(2));
		if(NULL==(*reference)) {
			PrintError(FnName,
					"reference",
					"Could not allocate memory",
					Exit,
					MallocMemory);
		}
		(*reference)[0] = 'N';
		(*reference)[1] = '\0';

		(*returnReferenceLength) = 1;
		(*returnPosition) = 1;
	}
	else {
		/* Get reference */
		success = RGBinaryGetSequence(rg,
				contig,
				startPos,
				strand,
				reference,
				endPos - startPos + 1);

		if(0 == success) {
			PrintError(FnName,
					NULL,
					"COuld not get reference",
					Exit,
					OutOfRange);
		}

		/* Update start pos and reference length */
		(*returnReferenceLength) = endPos - startPos + 1;
		(*returnPosition) = startPos;
	}
}

int8_t RGBinaryGetBase(RGBinary *rg,
		int32_t contig,
		int32_t position) 
{
	char *FnName = "RGBinaryGetBase";
	int32_t numCharsPerByte=ALPHABET_SIZE/2;
	uint8_t curByte, curChar;
	int32_t repeat;

	if(contig < 1 ||
			contig > rg->numContigs ||
			position < 1 ||
			position > rg->contigs[contig-1].sequenceLength) {
		return 0;
	}

	/* For DNA */
	assert(numCharsPerByte == 2);

	/* The index in the sequence for the given position */
	int32_t posIndex = position-1;
	int32_t byteIndex = posIndex%numCharsPerByte; /* Which bits in the byte */
	posIndex = (posIndex - byteIndex)/numCharsPerByte; /* Get which byte */

	/* Get the current byte */
	assert(posIndex >= 0 && posIndex < rg->contigs[contig-1].numBytes);
	curByte= rg->contigs[contig-1].sequence[posIndex];

	/* Extract base */
	repeat = 0;
	curChar = 'E';
	switch(byteIndex) {
		case 0:
			/* left-most 2-bits */
			repeat = curByte & 0xC0; /* zero out the irrelevant bits */
			switch(repeat) {
				case 0x00:
					repeat = 0;
					break;
				case 0x40:
					repeat = 1;
					break;
				case 0x80:
					repeat = 2;
					break;
				default:
					PrintError(FnName,
							NULL,
							"Could not understand case 0 repeat",
							Exit,
							OutOfRange);
					break;
			}                /* third and fourth bits from the left */
			curByte = curByte & 0x30; /* zero out the irrelevant bits */
			switch(curByte) {
				case 0x00:
					curChar = 'a';
					break;
				case 0x10:
					curChar = 'c';
					break;
				case 0x20:
					curChar = 'g';
					break;
				case 0x30:
					curChar = 't';
					break;
				default:
					PrintError(FnName,
							NULL,
							"Could not understand case 0 base",
							Exit,
							OutOfRange);
					break;
			}
			break;
		case 1:
			/* third and fourth bits from the right */
			repeat = curByte & 0x0C; /* zero out the irrelevant bits */
			switch(repeat) {
				case 0x00:
					repeat = 0;
					break;
				case 0x04:
					repeat = 1;
					break;
				case 0x08:
					repeat = 2;
					break;
				default:
					PrintError(FnName,
							NULL,
							"Could not understand case 1 repeat",
							Exit,
							OutOfRange);
					break;
			}
			/* right-most 2-bits */
			curByte = curByte & 0x03; /* zero out the irrelevant bits */
			switch(curByte) {
				case 0x00:
					curChar = 'a';
					break;
				case 0x01:
					curChar = 'c';
					break;
				case 0x02:
					curChar = 'g';
					break;
				case 0x03:
					curChar = 't';
					break;
				default:
					PrintError(FnName,
							NULL,
							"Could not understand case 1 base",
							Exit,
							OutOfRange);
					break;
			}
			break;
		default:
			PrintError(FnName,
					"byteIndex",
					"Could not understand byteIndex",
					Exit,
					OutOfRange);
	}
	/* Update based on repeat */
	switch(repeat) {
		case 0:
			/* ignore, not a repeat */
			break;
		case 1:
			/* repeat, convert int8_t to upper */
			curChar=ToUpper(curChar);
			break;
		case 2:
			/* N int8_tacter */
			curChar='N';
			break;
		default:
			PrintError(FnName,
					"repeat",
					"Could not understand repeat",
					Exit,
					OutOfRange);
			break;
	}
	/* Error check */
	switch(curChar) {
		case 'a':
		case 'c':
		case 'g':
		case 't':
		case 'A':
		case 'C':
		case 'G':
		case 'T':
		case 'N':
			break;
		default:
			PrintError(FnName,
					NULL,
					"Could not understand base",
					Exit,
					OutOfRange);
	}
	return curChar;
}

int32_t RGBinaryIsRepeat(RGBinary *rg,
		int32_t contig,
		int32_t position)
{
	int8_t curBase = RGBinaryGetBase(rg,
			contig,
			position);

	return RGBinaryIsBaseRepeat(curBase);
}

int32_t RGBinaryIsBaseRepeat(int8_t curBase)
{
	switch(curBase) {
		/* Lower case is repat */
		case 'a':
		case 'c':
		case 'g':
		case 't':
			return 1;
			break;
		case 'A':
		case 'G':
		case 'C':
		case 'T':
		default:
			return 0;
			break;
	}
}

int32_t RGBinaryIsN(RGBinary *rg,
		int32_t contig, 
		int32_t position) 
{
	int8_t curBase = RGBinaryGetBase(rg,
			contig,
			position);

	return RGBinaryIsBaseN(curBase);
}

int32_t RGBinaryIsBaseN(int8_t curBase)
{
	return ( (curBase == 'n' || curBase == 'N')?1:0);
}
