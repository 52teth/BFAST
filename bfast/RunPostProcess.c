#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <zlib.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include "BLibDefinitions.h"
#include "BLib.h"
#include "BError.h"
#include "AlignedRead.h"
#include "AlignedEnd.h"
#include "AlignedReadConvert.h"
#include "ScoringMatrix.h"
#include "AlignMatrix.h"
#include "Align.h"
#include "RunPostProcess.h"

#define MAXIMUM_RESCUE_MAPQ 30

static pthread_mutex_t alignQueueMutex = PTHREAD_MUTEX_INITIALIZER;

static inline int isDiscordantPair(AlignedEntry *entryOne,
		AlignedEntry *entryTwo,
		PEDBins *b,
		double pairingStandardDeviation)
{
	int8_t strand = 0;
	double window = 0.0;
	int32_t insertSize = 0.0;
	int32_t discordantPair = 1; // prove otherwise

	window = b->avg;
	if(b->avg < 0) {
		window -= 2.0*pairingStandardDeviation*b->std;
	}
	else {
		window += 2.0*pairingStandardDeviation*b->std;
	}

	if(FORWARD == entryOne->strand) {
		insertSize = entryOne->position - entryTwo->position;
	}
	else {
		insertSize = entryTwo->position - entryOne->position;
	}
	if(0.5 < b->inversionCount / ((double)b->numDistances)) {
		strand = (FORWARD == entryOne->strand) ? REVERSE : FORWARD;
	}
	else {
		strand = entryOne->strand;
	}
	if(entryOne->contig == entryTwo->contig &&
			strand == entryOne->strand &&
			insertSize < window) {
		discordantPair = 0;
	}

	return discordantPair;
}

// TODO: change the meaning of unpaired
void ReadInputFilterAndOutput(RGBinary *rg,
		char *inputFileName,
		int algorithm,
		int space,
		int unpaired,
		int reversePaired,
		int avgMismatchQuality,
		char *scoringMatrixFileName,
		int randomBest,
		int minimumMappingQuality,
		int minimumNormalizedScore,
		double pairingStandardDeviation,
		int gappedPairingRescue,
		int numThreads,
		int queueLength,
		int outputFormat,
		char *outputID,
		char *readGroup,
		FILE *fpOut)
{
	char *FnName="ReadInputFilterAndOutput";
	gzFile fp=NULL;
	int32_t i, j;
	int32_t numUnmapped=0, numReported=0;
	gzFile fpReportedGZ=NULL;
	FILE *fpReported=NULL;
	int32_t *mappedEndCounts=NULL;
	int32_t mappedEndCountsNumEnds=-1;
	int8_t *foundTypes=NULL;
	char *readGroupString=NULL;
	pthread_t *threads=NULL;
	int errCode;
	void *status=NULL;
	PostProcessThreadData *data=NULL;
	ScoringMatrix sm;
	int32_t mismatchScore, numRead, queueIndex;
	int32_t numReadsProcessed = 0;
	AlignedRead *alignQueue=NULL;
	int32_t alignQueueLength = 0;
	int32_t *alignQueueThreadIDs = NULL;
	int32_t **numEntries=NULL;
	int32_t *numEntriesN=NULL;
	PEDBins bins;

	srand48(1); // to get the same behavior

	/* Read in scoring matrix */
	ScoringMatrixInitialize(&sm);
	if(NULL != scoringMatrixFileName) {
		ScoringMatrixRead(scoringMatrixFileName, &sm, space);
	}
	/* Calculate mismatch score */
	/* Assumes all match scores are the same and all substitution scores are the same */
	if(space == NTSpace) {
		mismatchScore = sm.ntMatch - sm.ntMismatch;
	}
	else {
		mismatchScore = sm.colorMatch - sm.colorMismatch;
	}

	if(NULL != readGroup) {
		readGroupString=ParseReadGroup(readGroup);
	}

	if(NULL == inputFileName && 0 == unpaired) {
		PrintError(FnName, "unpaired", "Pairing from stdin currently not supported", Exit, OutOfRange);
	}

	/* Open the input file */
	if(NULL == inputFileName) {
		if(!(fp=gzdopen(fileno(stdin), "rb"))) {
			PrintError(FnName, "stdin", "Could not open stdin for reading", Exit, OpenFileError);
		}
	}
	else {
		if(!(fp=gzopen(inputFileName, "rb"))) {
			PrintError(FnName, inputFileName, "Could not open inputFileName for reading", Exit, OpenFileError);
		}
	}

	/* Open output files, if necessary */
	if(BAF == outputFormat) {
		if(!(fpReportedGZ=gzdopen(fileno(fpOut), "wb"))) {
			PrintError(FnName, "stdout", "Could not open stdout for writing", Exit, OpenFileError);
		}
	}
	else {
		if(!(fpReported=fdopen(fileno(fpOut), "wb"))) {
			PrintError(FnName, "stdout", "Could not open stdout for writing", Exit, OpenFileError);
		}
	}

	AlignedReadConvertPrintHeader(fpReported, rg, outputFormat, readGroup);

	/* Allocate memory for threads */
	threads=malloc(sizeof(pthread_t)*numThreads);
	if(NULL==threads) {
		PrintError(FnName, "threads", "Could not allocate memory", Exit, MallocMemory);
	}
	/* Allocate memory to pass data to threads */
	data=malloc(sizeof(PostProcessThreadData)*numThreads);
	if(NULL==data) {
		PrintError(FnName, "data", "Could not allocate memory", Exit, MallocMemory);
	}

	alignQueueLength=queueLength;
	alignQueue=malloc(sizeof(AlignedRead)*alignQueueLength);
	if(NULL == alignQueue) {
		PrintError(FnName, "alignQueue", "Could not allocate memory", Exit, MallocMemory);
	}
	alignQueueThreadIDs=malloc(sizeof(int32_t)*alignQueueLength);
	if(NULL == alignQueueThreadIDs) {
		PrintError(FnName, "alignQueue", "Could not allocate memory", Exit, MallocMemory);
	}
	numEntries=malloc(sizeof(int32_t*)*alignQueueLength);
	if(NULL == numEntries) {
		PrintError(FnName, "numEntries", "Could not allocate memory", Exit, MallocMemory);
	}
	numEntriesN=malloc(sizeof(int32_t)*alignQueueLength);
	if(NULL == numEntriesN) {
		PrintError(FnName, "numEntriesN", "Could not allocate memory", Exit, MallocMemory);
	}
	foundTypes=malloc(sizeof(int8_t)*alignQueueLength);
	if(NULL == foundTypes) {
		PrintError(FnName, "foundTypes", "Could not allocate memory", Exit, MallocMemory);
	}

	// Initialize
	for(i=0;i<alignQueueLength;i++) {
		numEntries[i] = NULL;
		numEntriesN[i] = 0;
	}

	/* Go through each read */
	if(VERBOSE >= 0) {
		fprintf(stderr, "Postprocessing...\n");
	}
	numRead = 0;
	while(0 != (numRead = GetAlignedReads(fp, alignQueue, alignQueueLength))) {

		/* Get the PEDBins if necessary */
		if(0 == unpaired) {
			PEDBinsInitialize(&bins);
			unpaired = GetPEDBins(alignQueue, alignQueueLength, &bins);
		}

		// Store the original # of entries for SAM output
		for(i=0;i<numRead;i++) {
			numEntries[i] = NULL;
			numEntriesN[i] = 0;
			foundTypes[i] = NoneFound;
			alignQueueThreadIDs[i] = -1;
		}

		/* Initialize thread data */
		for(i=0;i<numThreads;i++) {
			data[i].bins = &bins;
			data[i].rg = rg;
			data[i].sm = &sm;
			data[i].algorithm = algorithm;
			data[i].unpaired = unpaired;
			data[i].reversePaired = reversePaired;
			data[i].avgMismatchQuality = avgMismatchQuality;
			data[i].randomBest = randomBest;
			data[i].mismatchScore = mismatchScore;
			data[i].minimumMappingQuality = minimumMappingQuality; 
			data[i].minimumNormalizedScore = minimumNormalizedScore; 
			data[i].alignQueue =  alignQueue;
			data[i].alignQueueThreadIDs =  alignQueueThreadIDs;
			data[i].pairingStandardDeviation = pairingStandardDeviation;
			data[i].gappedPairingRescue = gappedPairingRescue;
			data[i].queueLength = numRead;
			data[i].foundTypes = foundTypes;
			data[i].numEntriesN = numEntriesN;
			data[i].numEntries = numEntries;
			data[i].threadID = i+1;
			data[i].numThreads = numThreads;
		}

		/* Open threads */
		for(i=0;i<numThreads;i++) {
			/* Start thread */
			errCode = pthread_create(&threads[i], /* thread struct */
					NULL, /* default thread attributes */
					ReadInputFilterAndOutputThread, /* start routine */
					&data[i]); /* data to routine */
			if(0!=errCode) {
				PrintError(FnName, "pthread_create: errCode", "Could not start thread", Exit, ThreadError);
			}
		}
		/* Wait for threads to return */
		for(i=0;i<numThreads;i++) {
			/* Wait for the given thread to return */
			errCode = pthread_join(threads[i],
					&status);
			/* Check the return code of the thread */
			if(0!=errCode) {
				PrintError(FnName, "pthread_join: errCode", "Thread returned an error", Exit, ThreadError);
			}
		}

		/* Print to Output file */
		for(queueIndex=0;queueIndex<numRead;queueIndex++) {
			int32_t numEnds=0;
			if(NoneFound == foundTypes[queueIndex]) {
				/* Free the alignments for output */
				for(i=0;i<alignQueue[queueIndex].numEnds;i++) {
					for(j=0;j<alignQueue[queueIndex].ends[i].numEntries;j++) {
						AlignedEntryFree(&alignQueue[queueIndex].ends[i].entries[j]);
					}
					alignQueue[queueIndex].ends[i].numEntries=0;
				}
			}
			else {
				numReported++;
			}

			// Get the # of ends
			numEnds = 0;
			for(i=0;i<alignQueue[queueIndex].numEnds;i++) {
				if(0 < alignQueue[queueIndex].ends[i].numEntries) {
					numEnds++;
				}
			}
			if(0 == numEnds) {
				numUnmapped++;
			}
			// Clean up
			if(mappedEndCountsNumEnds < numEnds) {
				// Reallocate
				mappedEndCounts = realloc(mappedEndCounts, sizeof(int32_t)*(1+numEnds));
				if(NULL == mappedEndCounts) {
					PrintError(FnName, "mappedEndCounts", "Could not reallocate memory", Exit, ReallocMemory);
				}
				// Initialize
				for(i=1+mappedEndCountsNumEnds;i<=numEnds;i++) {
					mappedEndCounts[i] = 0;
				}
				mappedEndCountsNumEnds = numEnds;
			}
			mappedEndCounts[numEnds]++;

			// Proper pair ? 
			int properPair = 0;
			if(2 == alignQueue[queueIndex].numEnds && 1 == alignQueue[queueIndex].ends[0].numEntries && 1 == alignQueue[queueIndex].ends[1].numEntries) {
				if(0 == isDiscordantPair(&alignQueue[queueIndex].ends[0].entries[0],
							&alignQueue[queueIndex].ends[1].entries[0],
							&bins,
							pairingStandardDeviation)) {
					properPair = 1;
				}
			}

			AlignedReadConvertPrintOutputFormat(&alignQueue[queueIndex], rg, fpReported, fpReportedGZ, (NULL == outputID) ? "" : outputID, readGroupString, algorithm, numEntries[queueIndex], outputFormat, properPair, BinaryOutput);

			/* Free memory */
			AlignedReadFree(&alignQueue[queueIndex]);
			/* Free */
			if(0 == unpaired) {
				PEDBinsFree(&bins);
			}
		}

		// Free
		for(i=0;i<numRead;i++) {
			free(numEntries[i]);
			numEntries[i] = NULL;
			numEntriesN[i] = 0;
		}

		numReadsProcessed += numRead;
		if(VERBOSE >= 0) {
			fprintf(stderr, "Reads processed: %d\n%s", numReadsProcessed, BREAK_LINE);
		}
	}
	if(0 <= VERBOSE) {
		fprintf(stderr, "Reads processed: %d\n", numReadsProcessed);
		fprintf(stderr, "Alignment complete.\n");
	}


	/* Close output files, if necessary */
	if(BAF == outputFormat) {
		gzclose(fpReportedGZ);
	}
	else {
		fclose(fpReported);
	}
	/* Close the input file */
	gzclose(fp);

	if(VERBOSE>=0) {
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Found %10lld reads with no ends mapped.\n", 
				(long long int)numUnmapped);
		if(!(mappedEndCountsNumEnds < 1 || numUnmapped == mappedEndCounts[0])) {
			fprintf(stderr, "%d < 1 || %d == %d\n",
					mappedEndCountsNumEnds,
					(int)numUnmapped,
					mappedEndCounts[0]);
		}
		assert(mappedEndCountsNumEnds < 1 || numUnmapped == mappedEndCounts[0]);
		for(i=1;i<=mappedEndCountsNumEnds;i++) {
			if(1 == i) fprintf(stderr, "Found %10d reads with %2d end mapped.\n", mappedEndCounts[i], i);
			else fprintf(stderr, "Found %10d reads with %2d ends mapped.\n", mappedEndCounts[i], i);
		}
		fprintf(stderr, "Found %10lld reads with at least one end mapping.\n",
				(long long int)numReported);
		fprintf(stderr, "%s", BREAK_LINE);
	}
	free(mappedEndCounts);
	free(readGroupString);
	free(threads);
	free(data);
	free(alignQueue);
	free(alignQueueThreadIDs);
	free(foundTypes);
	free(numEntries);
	free(numEntriesN);
}

void *ReadInputFilterAndOutputThread(void *arg)
{
	char *FnName="ReadInputFilterAndOutputThread";
	PostProcessThreadData *data = (PostProcessThreadData*)arg;
	PEDBins *bins = data->bins;
	RGBinary *rg = data->rg;
	ScoringMatrix *sm = data->sm;
	int algorithm = data->algorithm;
	int unpaired = data->unpaired;
	int reversePaired = data->reversePaired;
	int avgMismatchQuality = data->avgMismatchQuality;
	int randomBest = data->randomBest;
	int mismatchScore = data->mismatchScore;
	int minimumMappingQuality = data->minimumMappingQuality;
	int minimumNormalizedScore = data->minimumNormalizedScore;
	double pairingStandardDeviation = data->pairingStandardDeviation;
	int gappedPairingRescue = data->gappedPairingRescue;
	AlignedRead *alignQueue = data->alignQueue;
	int32_t *alignQueueThreadIDs = data->alignQueueThreadIDs;
	int queueLength = data->queueLength;
	int8_t *foundTypes = data->foundTypes;
	int32_t threadID = data->threadID;
	int32_t numThreads = data->numThreads;
	int32_t **numEntries = data->numEntries;
	int32_t *numEntriesN = data->numEntriesN;
	int32_t i, j;
	int32_t queueIndex=0;
	AlignMatrix matrix;
	AlignMatrixInitialize(&matrix); 

	while(queueIndex<queueLength) {
		if(1 < numThreads) {
			pthread_mutex_lock(&alignQueueMutex);
			if(alignQueueThreadIDs[queueIndex] < 0) {
				// mark this block
				for(i=queueIndex;i<queueLength && i<queueIndex+BFAST_POSTPROCESS_THREAD_BLOCK_SIZE;i++) {
					alignQueueThreadIDs[i] = threadID;
				}
			}
			else if(alignQueueThreadIDs[queueIndex] != threadID) {
				pthread_mutex_unlock(&alignQueueMutex);
				queueIndex+=BFAST_POSTPROCESS_THREAD_BLOCK_SIZE;
				// skip this block
				continue;
			}
			pthread_mutex_unlock(&alignQueueMutex);
		}

		for(i=0;i<BFAST_POSTPROCESS_THREAD_BLOCK_SIZE && queueIndex<queueLength;i++,queueIndex++) {
			assert(numThreads <= 1 || alignQueueThreadIDs[queueIndex] == threadID);

			if(numEntriesN[queueIndex] < alignQueue[queueIndex].numEnds) {
				numEntriesN[queueIndex] = alignQueue[queueIndex].numEnds;
				numEntries[queueIndex]=realloc(numEntries[queueIndex], sizeof(int32_t)*numEntriesN[queueIndex]);
				if(NULL == numEntries[queueIndex]) {
					PrintError(FnName, "numEntries[queueIndex]", "Could not reallocate memory", Exit, ReallocMemory);
				}
			}
			for(j=0;j<alignQueue[queueIndex].numEnds;j++) {
				numEntries[queueIndex][j] = alignQueue[queueIndex].ends[j].numEntries;
			}

			/* Filter */
			foundTypes[queueIndex] = FilterAlignedRead(&alignQueue[queueIndex],
					rg,
					&matrix,
					sm,
					algorithm,
					unpaired,
					reversePaired,
					avgMismatchQuality,
					randomBest,
					mismatchScore,
					minimumMappingQuality,
					minimumNormalizedScore,
					pairingStandardDeviation,
					gappedPairingRescue,
					bins);
		}
	}

	// Free
	AlignMatrixFree(&matrix);

	return arg;
}

int32_t GetAlignedReads(gzFile fp, AlignedRead *alignQueue, int32_t maxToRead) 
{
	int32_t numRead=0;
	while(numRead < maxToRead) {
		AlignedReadInitialize(&alignQueue[numRead]);
		if(EOF == AlignedReadRead(&alignQueue[numRead], fp)) {
			break;
		}
		numRead++;
	}
	return numRead;
}

static int32_t getPairedScore(AlignedEnd *endOne,
		int32_t endOneEntriesIndex,
		AlignedEnd *endTwo,
		int32_t endTwoEntriesIndex,
		int reversePaired,
		int avgMismatchQuality,
		int mismatchScore,
		PEDBins *b,
		double pairingStandardDeviation)
{
	int32_t s = (int)(endOne->entries[endOneEntriesIndex].score + endTwo->entries[endTwoEntriesIndex].score);

	if((0 == reversePaired && endOne->entries[endOneEntriesIndex].strand != endTwo->entries[endTwoEntriesIndex].strand) ||
			(1 == reversePaired && endOne->entries[endOneEntriesIndex].strand == endTwo->entries[endTwoEntriesIndex].strand)) { // inversion penalty
		// log10(P) * mismatchScore
		if(0 < b->inversionCount && 0 < b->numDistances) {
			if(MAX_INVERSION_LOG10_RATIO < b->invRatio) {
				s -= (int)(mismatchScore * MAX_INVERSION_LOG10_RATIO);
			}
			else {
				s -= (int)(mismatchScore * b->invRatio);
			}
		}
		else {
			s -= (int)(mismatchScore * MAX_INVERSION_LOG10_RATIO);
		}
	}

	// add penalty for insert size -- assumes normality
	if(endOne->entries[endOneEntriesIndex].contig != endTwo->entries[endTwoEntriesIndex].contig) { // chimera/inter-chr-translocation
		// TODO: make this a constant calculation
		s -= (int)(mismatchScore * -1.0 * log10( erfc(M_SQRT1_2 * pairingStandardDeviation)) + 0.499);
	}
	else { // same chr
		int32_t l = endTwo->entries[endTwoEntriesIndex].position - endOne->entries[endOneEntriesIndex].position;
		// penalty times probability of observing etc.
		if(fabs(l - b->avg) / b->std <= pairingStandardDeviation) { 
			// no penalty
			//s -= (int)(mismatchScore * -1.0 * log10( erfc(M_SQRT1_2 * fabs(l - b->avg) / b->std)) + 0.499);
		}
		else {
			// TODO: make this a constant calculation
			s -= (int)(mismatchScore * -1.0 * log10( erfc(M_SQRT1_2 * pairingStandardDeviation)) + 0.499);
		}
	}

	return s;
}

// Return 1 if read-rescue is performed, 0 otherwise
static int ReadPairingAndRescue(AlignedRead *a,
		RGBinary *rg,
		AlignMatrix *matrix,
		ScoringMatrix *sm,
		int algorithm,
		int unpaired,
		int reversePaired,
		int avgMismatchQuality,
		int randomBest,
		int mismatchScore,
		int minimumMappingQuality,
		int minimumNormalizedScore,
		double pairingStandardDeviation,
		int gappedPairingRescue,
		PEDBins *b,
		int32_t *foundTypes)
{
	int32_t i, j, k;
	int32_t rescuedEnd[2]={0,0};

	if(2 == a->numEnds && 
			unpaired == 0 && 
			(BestScore == algorithm || BestScoreAll == algorithm) &&
			(0 < a->ends[0].numEntries || 0 < a->ends[1].numEntries)) {

		int32_t bestScoreSE[2] = {INT_MIN, INT_MIN};
		int32_t bestScoreSENum[2] = {0, 0};
		int32_t bestScoreSEIndex[2] = {-1, -1};
		int32_t bestIndex[2] = {-1, -1};
		int32_t bestScore = INT_MIN;
		int32_t bestNum = 0;
		int32_t penultimateIndex[2] = {-1, -1};
		int32_t penultimateScore = INT_MIN;
		int32_t penultimateNum = 0;
		int32_t discordantPair = 0;
		AlignedEnd ends[2];
		int32_t space = a->space;

		// Get "best score"
		for(i=0;i<2;i++) {
			for(j=0;j<a->ends[i].numEntries;j++) {
				if(0 == bestScoreSENum[i] || bestScoreSE[i] < a->ends[i].entries[j].score) {
					bestScoreSE[i] = a->ends[i].entries[j].score;
					bestScoreSEIndex[i] = j;
					bestScoreSENum[i] = 1;
				}
				else if(bestScoreSE[i] == a->ends[i].entries[j].score) {
					bestScoreSENum[i]++;
				}
			}
		}

		// Check discordance
		if(1 == bestScoreSENum[0] && 1 == bestScoreSENum[1]) {
			discordantPair = isDiscordantPair(&a->ends[0].entries[bestScoreSEIndex[0]],
					&a->ends[1].entries[bestScoreSEIndex[1]],
					b,
					pairingStandardDeviation);
		}

		// HERE
		//fprintf(stderr, "HERE 1 %d:%d discordant=%d\n", a->ends[0].numEntries, a->ends[1].numEntries, discordantPair);

		// Rescue unpaired reads and discordant pairs
		if(0 == a->ends[0].numEntries || 0 == a->ends[1].numEntries || 1 == discordantPair) { 
			// Read-rescue
			for(i=0;i<2;i++) {
				double meanInsert = 0.0;

				AlignedEndInitialize(&ends[1-i]);

				if(0 == i) {
					meanInsert += b->avg;
				}
				else {
					meanInsert -= b->avg;
				}
				for(j=0;j<a->ends[i].numEntries;j++) {
					if(bestScoreSE[i] == a->ends[i].entries[j].score) {
						// Read rescue
						int32_t contig;
						int32_t referenceLength = 0;
						int32_t referencePosition = 0;
						int32_t readLength;
						char *reference=NULL;
						int32_t startPos;
						char read[SEQUENCE_LENGTH]="\0";
						char colors[SEQUENCE_LENGTH]="\0";
						int8_t strand = 0;

						contig = a->ends[i].entries[j].contig;
						startPos = a->ends[i].entries[j].position - pairingStandardDeviation*b->std;
						referenceLength = (int)((2.0*pairingStandardDeviation*b->std) + 0.5);
						if(0.5 < b->inversionCount / ((double)b->numDistances)) {
							// Opposite strands
							strand = (FORWARD == a->ends[i].entries[j].strand) ? REVERSE : FORWARD;
						}
						else {
							// Same strand
							strand = a->ends[i].entries[j].strand;
						}
						if(FORWARD == a->ends[i].entries[j].strand) {
							if(0 == i) { // first end
								startPos = a->ends[i].entries[j].position + b->avg - pairingStandardDeviation*b->std;
							}
							else {
								startPos = a->ends[i].entries[j].position - b->avg - pairingStandardDeviation*b->std;
							}
						}
						else {
							if(0 == i) { // first end
								startPos = a->ends[i].entries[j].position - b->avg - pairingStandardDeviation*b->std;
							}
							else {
								startPos = a->ends[i].entries[j].position + b->avg - pairingStandardDeviation*b->std;
							}
						}
						if(1 == reversePaired) {
							startPos -= a->ends[1-i].readLength; // Not counted otherwise
							referenceLength += a->ends[1-i].readLength; 
						}
						referenceLength += a->ends[1-i].readLength;

						// Initialize read
						strcpy(read, a->ends[1-i].read);
						if(NTSpace == space) {
							readLength = a->ends[1-i].readLength;
						}
						else {
							strcpy(colors, a->ends[1-i].read); 
							readLength = a->ends[1-i].readLength;
							NormalizeColorSpaceRead(colors, readLength, COLOR_SPACE_START_NT);
							readLength = ConvertReadFromColorSpace(read, readLength);
							// Remove the adaptor from the colors
							for(k=0;k<readLength;k++) {
								// remember to convert to '4's
								switch(colors[k+1]) {
									case '0':
									case '1':
									case '2':
									case '3':
										colors[k] = colors[k+1]; break;
									default:
										colors[k] = '4';
								}
							}
							colors[k]='\0';
						}
						// Get the reference sequence
						RGBinaryGetReference(rg,
								contig,
								startPos,
								strand,
								0,
								&reference,
								referenceLength,
								&referenceLength,
								&referencePosition);
						// HERE
						/*
						   fprintf(stderr, "%s\t%c:%d-%d\t%c:%d-%d\t%c:%d-%d\n",
						   a->readName, a->ends[i].entries[j].strand, a->ends[i].entries[j].position, a->ends[i].entries[j].position + a->ends[i].readLength-1, 
						   strand, startPos, startPos + referenceLength -1,
						   strand, referencePosition, referencePosition + referenceLength-1);
						   */
						if(readLength <= referenceLength) {
							AlignedEntry *aEntry=NULL;

							AlignedEndReallocate(&ends[1-i], ends[1-i].numEntries+1);

							aEntry = &ends[1-i].entries[ends[1-i].numEntries-1];

							if(matrix->nrow < readLength+1 || matrix->ncol < referenceLength+1) {
								AlignMatrixReallocate(matrix, GETMAX(matrix->nrow, readLength+1), GETMAX(matrix->ncol, referenceLength+1));
							}
							aEntry->contig = contig;
							aEntry->strand = strand;
							aEntry->position = -1;
							aEntry->score = NEGATIVE_INFINITY;
							aEntry->alnRead = NULL;
							aEntry->alnReadLength = 0;

							if(1 == gappedPairingRescue) {
								if(0 == AlignGapped(read,
											colors,
											NULL,
											readLength,
											reference,
											referenceLength,
											Unconstrained, // ALWAYS
											sm,
											aEntry,
											matrix,
											space,
											referenceLength-readLength+1,
											0,
											0,
											referencePosition,
											strand,
											NEGATIVE_INFINITY) ||
										aEntry->score <= 0) {
									// HERE
									//fprintf(stderr, "IGNORED aEntry->score=%d\n", aEntry->score);
									AlignedEndReallocate(&ends[1-i], ends[1-i].numEntries-1);
								}
								else { 
									if(0 == discordantPair) {
										if(MAXIMUM_RESCUE_MAPQ < a->ends[i].entries[j].mappingQuality) {
											ends[1-i].entries[ends[1-i].numEntries-1].mappingQuality = MAXIMUM_RESCUE_MAPQ;
										}
										else {
											ends[1-i].entries[ends[1-i].numEntries-1].mappingQuality = a->ends[i].entries[j].mappingQuality;
										}
									}
									// HERE
									/*
									   fprintf(stderr, "RESCUED %c:%d\t%d\t%c:%d-%d\t%d:%d:%d\n", aEntry->strand, aEntry->position, aEntry->score,
									   strand, referencePosition, referencePosition + referenceLength-1,
									   a->ends[0].numEntries,
									   a->ends[1].numEntries,
									   discordantPair);
									   */
								}
							}
							else {
								if(0 == AlignUngapped(read,
											colors,
											NULL,
											readLength,
											reference,
											referenceLength,
											Unconstrained, // ALWAYS
											sm,
											aEntry,
											space,
											0,
											referencePosition,
											strand) ||
										aEntry->score <= 0) {
									AlignedEndReallocate(&ends[1-i], ends[1-i].numEntries-1);
								}
								else if(0 == i) { // HERE
									// HERE
									//fprintf(stderr, "RESCUED %c:%d\t%d\n", aEntry->strand, aEntry->position, aEntry->score);
									if(0 == discordantPair) {
										if(MAXIMUM_RESCUE_MAPQ < a->ends[i].entries[j].mappingQuality) {
											ends[1-i].entries[ends[1-i].numEntries-1].mappingQuality = MAXIMUM_RESCUE_MAPQ;
										}
										else {
											ends[1-i].entries[ends[1-i].numEntries-1].mappingQuality = a->ends[i].entries[j].mappingQuality;
										}
									}
								}
							}
						}
						free(reference);
						reference=NULL;
					}
				}
			}

			// Copy over
			for(i=0;i<2;i++) {
				if(0 < ends[i].numEntries) {
					k = a->ends[i].numEntries;
					AlignedEndReallocate(&a->ends[i], a->ends[i].numEntries + ends[i].numEntries);
					for(j=0;j<ends[i].numEntries;j++,k++) {
						ends[i].entries[j].mappingQuality = 0;
						AlignedEntryCopy(&a->ends[i].entries[k], &ends[i].entries[j]);
						if(bestScoreSE[i] < ends[i].entries[j].score) {
							bestScoreSE[i] = ends[i].entries[j].score;
							bestScoreSEIndex[i] = k;
							bestScoreSENum[i] = 1;
							rescuedEnd[i] = 1;
						}
						else if(bestScoreSE[i] == ends[i].entries[j].score) {
							bestScoreSENum[i]++;
							rescuedEnd[i] = 1;
						}
					}
				}
			}
			if(0 < rescuedEnd[0] + rescuedEnd[1]) {
				AlignedReadRemoveDuplicates(a, AlignedEntrySortByAll);

				int32_t mismatchScore;
				if(space == NTSpace) {
					mismatchScore = sm->ntMatch - sm->ntMismatch;
				}
				else {
					mismatchScore = sm->colorMatch - sm->colorMismatch;
				}

				if(1 == discordantPair) {
					AlignedReadUpdateMappingQuality(a,
							mismatchScore,
							avgMismatchQuality);
				}
			}
			// Free
			for(i=0;i<2;i++) {
				AlignedEndFree(&ends[i]);
			}
		}

		// HERE
		/*
		   fprintf(stderr, "HERE 2 %d:%d\n", a->ends[0].numEntries, a->ends[1].numEntries);
		   for(i=0;i<2;i++) {
		   for(j=0;j<a->ends[i].numEntries;j++) {
		   AlignedEntryPrintText(&a->ends[i].entries[j], stderr);
		   }
		   }
		   */

		// Check if we have hits for both ends
		if(a->ends[0].numEntries <= 0 || a->ends[1].numEntries <= 0) {
			return 0;
		}

		// HERE skip pairing scoring
		//return 0;

		// one end must be the anchor, and thus the best score.
		// This could be more efficient
		for(i=0;i<a->ends[0].numEntries;i++) {
			int anchored = 0;
			if(bestScoreSE[0] <= a->ends[0].entries[i].score) { // anchor
				anchored = 1;
			}
			for(j=0;j<a->ends[1].numEntries;j++) {
				if(1 == anchored || bestScoreSE[1] <= a->ends[1].entries[j].score) { // itself an anchor, or other end anchored

					int32_t s = getPairedScore(&a->ends[0], i, &a->ends[1], j, reversePaired, avgMismatchQuality, mismatchScore, b, pairingStandardDeviation);

					if(-1 == bestIndex[0] || bestScore < s) { // current is the best
						if(-1 != bestIndex[0]) { // there was a previous
							penultimateIndex[0] = bestIndex[0];
							penultimateIndex[1] = bestIndex[1];
							penultimateScore = bestScore;
							penultimateNum = bestNum;
						}
						// reset
						bestIndex[0] = i; bestIndex[1] = j;
						bestScore = s;
						bestNum = 1;
					}
					else if(bestScore == s) { // equal to the best
						bestNum++;
					}
					else if(-1 == penultimateIndex[0] || penultimateScore < s) { // current is next best
						penultimateIndex[0] = i; penultimateIndex[1] = j;
						penultimateScore = s;
						penultimateNum = 1;
					}
					else { // current is equal to the next best
						penultimateNum++;
					}
				}
			}
		}
		assert(-1 != bestIndex[0]);
		assert(1 <= bestNum);

		int randomized = 0;
		if(1 < bestNum && BestScore == algorithm && 1 == randomBest) {
			// Choose random pair, give zero mapping quality
			int32_t keep = (int)(drand48() * bestNum);
			int32_t k = 0;
			for(i=0;i<a->ends[0].numEntries && k <= keep;i++) {
				int anchored = 0;
				if(bestScoreSE[0] <= a->ends[0].entries[i].score) { // anchor
					anchored = 1;
				}
				for(j=0;j<a->ends[1].numEntries;j++) {
					if(1 == anchored || bestScoreSE[1] <= a->ends[1].entries[j].score) { // itself an anchor, or other end anchored

						int32_t s = getPairedScore(&a->ends[0], i, &a->ends[1], j, reversePaired, avgMismatchQuality, mismatchScore, b, pairingStandardDeviation);
						if(bestScore == s) {
							if(keep == k) {
								AlignedEntryCopy(&a->ends[0].entries[0], &a->ends[0].entries[i]);
								AlignedEndReallocate(&a->ends[0], 1);
								foundTypes[0] = Found;
								AlignedEntryCopy(&a->ends[1].entries[0], &a->ends[1].entries[j]);
								AlignedEndReallocate(&a->ends[1], 1);
								foundTypes[1] = Found;
								randomized = 1;
								break;
							}
							k++;
						}
					}
				}
			}
			bestNum = 1;
		}

		// set new mapping quality (?)
		// TODO: set single end mapping quality
		if(1 < bestNum) { // more than one found
			// Revert to best alignment score(s) for each end.
			for(i=0;i<2;i++) {
				int k = 0;
				for(j=0;j<a->ends[i].numEntries;j++) {
					if(a->ends[i].entries[j].score == bestScoreSE[i]) { // keep best score
						AlignedEntryCopy(&a->ends[i].entries[k], &a->ends[i].entries[j]);
						k++;
					}
				}
				AlignedEndReallocate(&a->ends[i], k);
			}

			if(BestScore == algorithm) {
				for(i=0;i<2;i++) {
					if(1 == a->ends[i].numEntries) {
						foundTypes[i] = Found;
						assert(0 == randomized);
					}
					else { // clear
						foundTypes[i] = NoneFound;
						AlignedEndReallocate(&a->ends[i], 0);
					}
				}
			}
			else {
				assert(BestScoreAll == algorithm);
				for(i=0;i<2;i++) {
					if(0 < a->ends[i].numEntries) {
						foundTypes[i] = Found;
					}
					else { // clear
						foundTypes[i] = NoneFound;
						AlignedEndReallocate(&a->ends[i], 0);
					}
				}
			}
		}
		else {
			// copy to the front and reallocate
			if(0 == randomized) {
				AlignedEntryCopy(&a->ends[0].entries[0], &a->ends[0].entries[bestIndex[0]]);
				AlignedEndReallocate(&a->ends[0], 1);
				AlignedEntryCopy(&a->ends[1].entries[0], &a->ends[1].entries[bestIndex[1]]);
				AlignedEndReallocate(&a->ends[1], 1);
			}
			foundTypes[0] = Found;
			foundTypes[1] = Found;

			if(1 == randomized) {
				for(i=0;i<2;i++) {
					if(1 == bestScoreSENum[i] && bestScoreSE[i] == a->ends[i].entries[0].score) {
						// keep the mapping quality
					}
					else {
						a->ends[i].entries[0].mappingQuality = 0;
					}
				}
			}
			else {
				int32_t mapq = MAXIMUM_MAPPING_QUALITY;
				if(-1 != penultimateIndex[0]) { // next best pairing found
					mapq = (bestScore - penultimateScore) * avgMismatchQuality / mismatchScore;
					if(0 == mapq) {
						mapq = 1;
					}
					else if(MAXIMUM_MAPPING_QUALITY < mapq) {
						mapq = MAXIMUM_MAPPING_QUALITY;
					}
				}
				assert(0 < mapq);
				if(a->ends[0].entries[0].score < bestScoreSE[0] || // changed alignment
						a->ends[0].entries[0].mappingQuality < mapq) { 
					// update mapping quality
					a->ends[0].entries[0].mappingQuality = mapq;
				}
				if(a->ends[1].entries[0].score < bestScoreSE[1] || // changed alignment
						a->ends[1].entries[0].mappingQuality < mapq) { 
					// update mapping quality
					a->ends[1].entries[0].mappingQuality = mapq;
				}
				assert(0 < a->ends[0].entries[0].mappingQuality);
				assert(0 < a->ends[1].entries[0].mappingQuality);
			}
			// TODO: set single end mapq flag
		}
		return 1; // Performed
	}
	return 0; // Not performed
}

int FilterAlignedRead(AlignedRead *a,
		RGBinary *rg,
		AlignMatrix *matrix,
		ScoringMatrix *sm,
		int algorithm,
		int unpaired,
		int reversePaired,
		int avgMismatchQuality,
		int randomBest,
		int mismatchScore,
		int minimumMappingQuality,
		int minimumNormalizedScore,
		double pairingStandardDeviation,
		int gappedPairingRescue,
		PEDBins *b)
{
	char *FnName="FilterAlignedRead";
	int foundType;
	int32_t *foundTypes=NULL;
	AlignedRead tmpA;
	int32_t i, j, k, ctr;
	int32_t best, bestIndex, numBest;

	AlignedReadInitialize(&tmpA);

	/* We should only modify "a" if it is going to be reported */ 
	/* Copy in case we do not find anything to report */
	AlignedReadCopy(&tmpA, a);

	foundType=NoneFound;
	foundTypes=malloc(sizeof(int32_t)*tmpA.numEnds);
	if(NULL == foundTypes) {
		PrintError(FnName, "foundTypes", "Could not allocate memory", Exit, MallocMemory);
	}
	for(i=0;i<tmpA.numEnds;i++) {
		foundTypes[i]=NoneFound;
	}

	if(0 == ReadPairingAndRescue(&tmpA, 
				rg,
				matrix,
				sm,
				algorithm,
				unpaired,
				reversePaired,
				avgMismatchQuality,
				randomBest,
				mismatchScore,
				minimumMappingQuality,
				minimumNormalizedScore,
				pairingStandardDeviation,
				gappedPairingRescue,
				b,
				foundTypes)) {
		// can we use Smith-Waterman to recover one end from the other?

		/* Pick alignment for each end individually (is this a good idea?) */
		for(i=0;i<tmpA.numEnds;i++) {
			/* Choose each end */
			switch(algorithm) {
				case NoFiltering:
				case AllNotFiltered:
					foundTypes[i] = (0<tmpA.ends[i].numEntries)?Found:NoneFound;
					break;
				case Unique:
					foundTypes[i]=(1==tmpA.ends[i].numEntries)?Found:NoneFound;
					break;
				case BestScore:
				case BestScoreAll:
					best = INT_MIN;
					bestIndex = -1;
					numBest = 0;
					for(j=0;j<tmpA.ends[i].numEntries;j++) {
						if(best < tmpA.ends[i].entries[j].score) {
							best = tmpA.ends[i].entries[j].score;
							bestIndex = j;
							numBest = 1;
						}
						else if(best == tmpA.ends[i].entries[j].score) {
							numBest++;
						}
					}
					// Copy all to the front
					ctr=0;
					for(j=0;j<tmpA.ends[i].numEntries;j++) {
						if(tmpA.ends[i].entries[j].score == best) {
							if(ctr != j) {
								AlignedEntryCopy(&tmpA.ends[i].entries[ctr], 
										&tmpA.ends[i].entries[j]);
							}
							ctr++;
						}
					}
					assert(ctr == numBest);
					AlignedEndReallocate(&tmpA.ends[i], numBest);
					// Random
					if(BestScore == algorithm) {
						if(1 < numBest && 1 == randomBest) {
							int32_t keep = (int)(drand48() * numBest);
							AlignedEntryCopy(&tmpA.ends[i].entries[0], &tmpA.ends[i].entries[keep]);
							AlignedEndReallocate(&tmpA.ends[i], 1);
							tmpA.ends[i].entries[0].mappingQuality = 0; // ambiguous
							numBest = 1;
						}
						if(1 == numBest) {
							foundTypes[i] = Found;
						}
						else {
							foundTypes[i] = NoneFound;
						}
					}
					else if(BestScoreAll == algorithm) {
						if( 1 <= numBest) {
							foundTypes[i] = Found;
						}
						else {
							foundTypes[i] = NoneFound;
						}
					}
					break;
				default:
					PrintError(FnName, "algorithm", "Could not understand algorithm", Exit, OutOfRange);
					break;
			}
			/* Free if not found */
			if(NoneFound == foundTypes[i]) {
				AlignedEndReallocate(&tmpA.ends[i],
						0);
			}
		}
	}

	if(INT_MIN < minimumMappingQuality ||
			INT_MIN < minimumNormalizedScore) {
		for(i=0;i<tmpA.numEnds;i++) {
			for(j=k=0;j<tmpA.ends[i].numEntries;j++) {
				if(minimumMappingQuality <= tmpA.ends[i].entries[j].mappingQuality &&
						minimumNormalizedScore <= tmpA.ends[i].entries[j].score/AlignedEntryGetReadLength(&tmpA.ends[i].entries[j])) {
					// Copy to the front
					if(j != k) {
						AlignedEntryCopy(&tmpA.ends[i].entries[k], &tmpA.ends[i].entries[j]);
					}
					k++;
				}
			}
			// Reallocate
			if(k < tmpA.ends[i].numEntries) {
				AlignedEndReallocate(&tmpA.ends[i], k);
			}
		}
	}

	// Found if one end is found
	foundType=NoneFound;
	for(i=0;NoneFound==foundType && i<tmpA.numEnds;i++) {
		if(Found == foundTypes[i]) {
			foundType=Found;
			break;
		}
	}

	/* copy back */
	if(NoneFound != foundType) {
		AlignedReadFree(a);
		AlignedReadCopy(a, &tmpA);
	}
	AlignedReadFree(&tmpA);
	free(foundTypes);


	return foundType;
}

int32_t GetPEDBins(AlignedRead *alignQueue,
		int queueLength,
		PEDBins *b)
{
	char *FnName="GetPEDBins";
	int64_t counter, foundType;
	int32_t alignQueueLength;
	int32_t numRead, queueIndex;

	alignQueueLength=queueLength;

	/* Go through each read */
	if(VERBOSE >= 0) {
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Estimating paired end distance...\n");
	}
	counter = numRead = 0;

	// TODO: make this multi-threaded
	for(queueIndex=0;queueIndex<queueLength;queueIndex++) {
		if(2 == alignQueue[queueIndex].numEnds) { // Only paired end data
			AlignedRead tmpA;

			// Filter base ond best scoring
			AlignedReadInitialize(&tmpA);
			AlignedReadCopy(&tmpA, &alignQueue[queueIndex]);

			foundType=FilterAlignedRead(&tmpA,
					NULL,
					NULL,
					NULL,
					BestScore,
					1,
					-1,
					INT_MIN,
					0,
					INT_MIN,
					INT_MIN,
					INT_MIN,
					0.0,
					0,
					NULL);

			if(Found == foundType) {
				assert(2 == tmpA.numEnds);
				/* Must only have one alignment per end and on the same contig.
				 * There is a potential this will be inferred incorrectly under
				 * many scenarios.  Be careful! */
				if(1 == tmpA.ends[0].numEntries &&
						1 == tmpA.ends[1].numEntries &&
						tmpA.ends[0].entries[0].contig == tmpA.ends[1].entries[0].contig) {
					if(FORWARD == tmpA.ends[0].entries[0].strand) {
						PEDBinsInsert(b, 
								tmpA.ends[0].entries[0].strand,
								tmpA.ends[1].entries[0].strand,
								tmpA.ends[1].entries[0].position - tmpA.ends[0].entries[0].position);
					}
					else {
						PEDBinsInsert(b, 
								tmpA.ends[0].entries[0].strand,
								tmpA.ends[1].entries[0].strand,
								tmpA.ends[0].entries[0].position - tmpA.ends[1].entries[0].position);
					}
				}
			}
			AlignedReadFree(&tmpA);
		}

		/* Increment counter */
		counter++;
	}

	if(b->numDistances < MIN_PEDBINS_SIZE) {
		fprintf(stderr, "Found only %d distances to infer the insert size distribution\n", b->numDistances);
		PrintError(FnName, "b->numDistances", "Not enough distances to infer insert size distribution", Warn, OutOfRange);
		PEDBinsFree(b);
		return 1;
	}

	if(VERBOSE>=0) {
		// Print Statistics
		PEDBinsPrintStatistics(b, stderr);
	}

	return 0;
}

void PEDBinsInitialize(PEDBins *b)
{
	int32_t i;
	b->minDistance = INT_MAX;
	b->maxDistance = INT_MIN;
	for(i=0;i<MAX_PEDBINS_DISTANCE - MIN_PEDBINS_DISTANCE+1;i++) {
		b->bins[i] = 0;
	}
	b->numDistances = 0;
	b->inversionCount = 0;
	b->avg = 0;
	b->std = 0;
}

void PEDBinsFree(PEDBins *b) {
	PEDBinsInitialize(b);
}

void PEDBinsInsert(PEDBins *b,
		char strandOne,
		char strandTwo,
		int32_t distance)
{
	if(distance < MIN_PEDBINS_DISTANCE ||
			MAX_PEDBINS_DISTANCE < distance) {
		return;
	}

	/// Update inversion count
	if(strandOne != strandTwo) {
		b->inversionCount++;
	}

	if(distance < b->minDistance) {
		b->minDistance = distance;
		if(0 == b->numDistances) { // First one!
			b->minDistance = b->maxDistance = distance;
		}
	}
	else if(b->maxDistance < distance) {
		b->maxDistance = distance;
		if(0 == b->numDistances) { // First one!
			b->minDistance = distance;
		}
	}

	// Add to bin
	b->bins[distance - b->minDistance]++;
	b->numDistances++;
}

void PEDBinsPrintStatistics(PEDBins *b, FILE *fp)
{
	// Mean, Range, and SD
	int32_t i;

	// Mean
	b->avg = 0.0;
	for(i=0;i<b->maxDistance-b->minDistance+1;i++) {
		b->avg += (b->minDistance + i)*b->bins[i];
	}
	b->avg /= b->numDistances;

	// SD
	b->std = 0.0;
	for(i=0;i<b->maxDistance-b->minDistance+1;i++) {
		b->std += b->bins[i]*((b->minDistance + i) - b->avg)*((b->minDistance + i) - b->avg);
	}
	b->std /= b->numDistances-1;
	b->std = sqrt(b->std);

	b->invRatio = -1.0 * log10(b->inversionCount / ((double)b->numDistances));

	if(0<=VERBOSE) {
		fprintf(stderr, "Used %d paired end distances to infer the insert size distribution.\n",
				b->numDistances);
		fprintf(stderr, "The paired end distance range was from %d to %d.\n",
				b->minDistance, b->maxDistance);
		fprintf(stderr, "The paired end distance mean and standard deviation was %.2lf and %.2lf.\n",
				b->avg, b->std);
		fprintf(stderr, "The inversion ratio was %lf (%d / %d).\n",
				b->inversionCount * 1.0 / ((double)b->numDistances), b->inversionCount, b->numDistances);
	}
}
