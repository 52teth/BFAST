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
#include "RunPostProcess.h"

// TODO: change hte meaning of unpaired
void ReadInputFilterAndOutput(RGBinary *rg,
		char *inputFileName,
		int algorithm,
		int space,
		int unpaired,
		int reversePaired,
		int avgMismatchQuality,
		char *scoringMatrixFileName,
		int numThreads,
		int queueLength,
		int outputFormat,
		char *outputID,
		char *readGroup,
		char *unmappedFileName,
		FILE *fpOut)
{
	char *FnName="ReadInputFilterAndOutput";
	gzFile fp=NULL;
	int32_t i;
	int32_t numUnmapped=0, numReported=0;
	gzFile fpReportedGZ=NULL;
	FILE *fpReported=NULL;
	gzFile fpUnmapped=NULL;
	PEDBins bins;
	int32_t *mappedEndCounts=NULL;
	int32_t mappedEndCountsNumEnds=-1;
	char *readGroupString=NULL;
	pthread_mutex_t inputFP_mutex, outputFP_mutex;
	pthread_t *threads=NULL;
	int errCode;
	void *status=NULL;
	PostProcessThreadData *data=NULL;
	ScoringMatrix sm;
	int32_t mismatchScore;

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

	/* Get the PEDBins if necessary */
	if(0 == unpaired) {
		PEDBinsInitialize(&bins);
		unpaired = GetPEDBins(inputFileName, algorithm, queueLength, &bins);
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
	if(NULL != unmappedFileName) {
		if(!(fpUnmapped=gzopen(unmappedFileName, "wb"))) {
			PrintError(FnName, unmappedFileName, "Could not open unmappedFileName for writing", Exit, OpenFileError);
		}
	}

	AlignedReadConvertPrintHeader(fpReported, rg, outputFormat, readGroup);

	// Initialize mutex
	pthread_mutex_init(&outputFP_mutex, NULL);
	pthread_mutex_init(&inputFP_mutex, NULL);

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

	/* Initialize thread data */
	for(i=0;i<numThreads;i++) {
		data[i].rg = rg;
		data[i].bins = &bins;
		data[i].algorithm = algorithm;
		data[i].unpaired = unpaired;
		data[i].reversePaired = reversePaired;
		data[i].avgMismatchQuality = avgMismatchQuality;
		data[i].mismatchScore = mismatchScore;
		data[i].queueLength = queueLength;
		data[i].outputFormat = outputFormat;
		data[i].readGroupString = readGroupString;
		data[i].outputID = outputID;
		data[i].mappedEndCounts = &mappedEndCounts;
		data[i].mappedEndCountsNumEnds = &mappedEndCountsNumEnds;
		data[i].numReported = &numReported;
		data[i].numUnmapped = &numUnmapped;

		data[i].inputFP_mutex = &inputFP_mutex;
		data[i].outputFP_mutex = &inputFP_mutex;
		data[i].fpIn = fp;
		data[i].fpReported = fpReported;
		data[i].fpReportedGZ = fpReportedGZ;
		data[i].fpUnmapped = fpUnmapped;
		data[i].threadID = i+1;
	}

	/* Go through each read */
	if(VERBOSE >= 0) {
		fprintf(stderr, "Processing reads, currently on:\n");
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
	if(VERBOSE >= 0) {
		fprintf(stderr, "\n");
	}

	/* Close output files, if necessary */
	if(BAF == outputFormat) {
		gzclose(fpReportedGZ);
	}
	else {
		fclose(fpReported);
	}
	if(NULL != unmappedFileName) {
		gzclose(fpUnmapped);
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

	/* Free */
	if(0 == unpaired) {
		PEDBinsFree(&bins);
	}
	free(mappedEndCounts);
	free(readGroupString);
	free(threads);
	free(data);
}

void *ReadInputFilterAndOutputThread(void *arg)
{
	char *FnName="ReadInputFilterAndOutputThread";
	PostProcessThreadData *data = (PostProcessThreadData*)arg;
	PEDBins *bins = data->bins;
	RGBinary *rg = data->rg;
	int algorithm = data->algorithm;
	int unpaired = data->unpaired;
	int reversePaired = data->reversePaired;
	int avgMismatchQuality = data->avgMismatchQuality;
	int mismatchScore = data->mismatchScore;
	int queueLength = data->queueLength;
	int outputFormat = data->outputFormat;
	char *outputID = data->outputID;
	char *readGroupString = data->readGroupString;
	int32_t **mappedEndCounts = data->mappedEndCounts;
	int32_t *mappedEndCountsNumEnds = data->mappedEndCountsNumEnds;
	int32_t *numReported = data->numReported;
	int32_t *numUnmapped = data->numUnmapped;
	pthread_mutex_t *inputFP_mutex = data->inputFP_mutex;
	gzFile fpIn = data->fpIn;
	FILE *fpReported = data->fpReported;
	gzFile fpReportedGZ = data->fpReportedGZ;
	gzFile fpUnmapped = data->fpUnmapped;
	pthread_mutex_t *outputFP_mutex = data->outputFP_mutex;
	int32_t threadID = data->threadID;

	int32_t i, j;
	int32_t numEnds=0;
	int64_t counter, foundType;
	AlignedRead *aBuffer=NULL;
	int32_t aBufferLength=0;
	int32_t numRead, aBufferIndex;
	int32_t **numEntries=NULL;
	int32_t *numEntriesN=NULL;

	aBufferLength=queueLength;
	aBuffer=malloc(sizeof(AlignedRead)*aBufferLength);
	if(NULL == aBuffer) {
		PrintError(FnName, "aBuffer", "Could not allocate memory", Exit, MallocMemory);
	}
	numEntries=malloc(sizeof(int32_t*)*aBufferLength);
	if(NULL == numEntries) {
		PrintError(FnName, "numEntries", "Could not allocate memory", Exit, MallocMemory);
	}
	numEntriesN=malloc(sizeof(int32_t)*aBufferLength);
	if(NULL == numEntriesN) {
		PrintError(FnName, "numEntriesN", "Could not allocate memory", Exit, MallocMemory);
	}
	for(i=0;i<aBufferLength;i++) {
		numEntries[i] = NULL;
		numEntriesN[i] = 0;
	}

	counter = numRead = 0;

	while(0 != (numRead = GetAlignedReads(fpIn, aBuffer, aBufferLength, inputFP_mutex))) {

		for(aBufferIndex=0;aBufferIndex<numRead;aBufferIndex++) {

			if(VERBOSE >= 0 && counter%ALIGNENTRIES_READ_ROTATE_NUM==0) {
				fprintf(stderr, "\rthreadID:%d\t[%lld]",
						threadID,
						(long long int)counter);
			}

			// Store the original # of entries for SAM output
			if(numEntriesN[aBufferIndex] < aBuffer[aBufferIndex].numEnds) {
				numEntriesN[aBufferIndex] = aBuffer[aBufferIndex].numEnds;
				numEntries[aBufferIndex]=realloc(numEntries[aBufferIndex], sizeof(int32_t)*numEntriesN[aBufferIndex]);
				if(NULL == numEntries[aBufferIndex]) {
					PrintError(FnName, "numEntries[aBufferIndex]", "Could not reallocate memory", Exit, ReallocMemory);
				}
			}
			for(i=0;i<aBuffer[aBufferIndex].numEnds;i++) {
				numEntries[aBufferIndex][i] = aBuffer[aBufferIndex].ends[i].numEntries;
			}

			/* Filter */
			foundType=FilterAlignedRead(&aBuffer[aBufferIndex],
					algorithm,
					unpaired,
					reversePaired,
					avgMismatchQuality,
					mismatchScore,
					bins);

			numEnds=0;
			if(NoneFound == foundType) {
				/* Print to Not Reported file */
				if(NULL != fpUnmapped) {
					AlignedReadPrint(&aBuffer[aBufferIndex], fpUnmapped);
				}

				/* Free the alignments for output */
				for(i=0;i<aBuffer[aBufferIndex].numEnds;i++) {
					for(j=0;j<aBuffer[aBufferIndex].ends[i].numEntries;j++) {
						AlignedEntryFree(&aBuffer[aBufferIndex].ends[i].entries[j]);
					}
					aBuffer[aBufferIndex].ends[i].numEntries=0;
				}
			}
			else {
				(*numReported)++;
			}

			/* Increment counter */
			counter++;
		}

		/* Print to Output file */
		pthread_mutex_lock(outputFP_mutex);
		for(aBufferIndex=0;aBufferIndex<numRead;aBufferIndex++) {
			// Get the # of ends
			numEnds = 0;
			for(i=0;i<aBuffer[aBufferIndex].numEnds;i++) {
				if(0 < aBuffer[aBufferIndex].ends[i].numEntries) {
					numEnds++;
				}
			}
			if(0 == numEnds) {
				(*numUnmapped)++;
			}
			// Clean up
			if((*mappedEndCountsNumEnds) < numEnds) {
				// Reallocate
				(*mappedEndCounts) = realloc((*mappedEndCounts), sizeof(int32_t)*(1+numEnds));
				if(NULL == (*mappedEndCounts)) {
					PrintError(FnName, "mappedEndCounts", "Could not reallocate memory", Exit, ReallocMemory);
				}
				// Initialize
				for(i=1+(*mappedEndCountsNumEnds);i<=numEnds;i++) {
					(*mappedEndCounts)[i] = 0;
				}
				(*mappedEndCountsNumEnds) = numEnds;
			}
			(*mappedEndCounts)[numEnds]++;

			AlignedReadConvertPrintOutputFormat(&aBuffer[aBufferIndex], rg, fpReported, fpReportedGZ, (NULL == outputID) ? "" : outputID, readGroupString, algorithm, numEntries[aBufferIndex], outputFormat, BinaryOutput);

			/* Free memory */
			AlignedReadFree(&aBuffer[aBufferIndex]);
		}
		pthread_mutex_unlock(outputFP_mutex);
		if(VERBOSE >= 0 && counter%ALIGNENTRIES_READ_ROTATE_NUM==0) {
			fprintf(stderr, "\rthreadID:%d\t[%lld]",
					threadID,
					(long long int)counter);
		}
	}
	if(VERBOSE >= 0) {
		fprintf(stderr, "\rthreadID:%d\t[%lld]",
				threadID,
				(long long int)counter);
	}

	free(aBuffer);
	for(i=0;i<aBufferLength;i++) {
		free(numEntries[i]);
	}
	free(numEntries);
	free(numEntriesN);
	return arg;
}

int32_t GetAlignedReads(gzFile fp, AlignedRead *aBuffer, int32_t maxToRead, pthread_mutex_t *inputFP_mutex)
{
	if(NULL != inputFP_mutex) pthread_mutex_lock(inputFP_mutex);
	int32_t numRead=0;
	while(numRead < maxToRead) {
		AlignedReadInitialize(&aBuffer[numRead]);
		if(EOF == AlignedReadRead(&aBuffer[numRead], fp)) {
			break;
		}
		numRead++;
	}
	if(NULL != inputFP_mutex) pthread_mutex_unlock(inputFP_mutex);
	return numRead;
}

int FilterAlignedRead(AlignedRead *a,
		int algorithm,
		int unpaired,
		int reversePaired,
		int avgMismatchQuality,
		int mismatchScore,
		PEDBins *b)
{
	char *FnName="FilterAlignedRead";
	int foundType;
	int32_t *foundTypes=NULL;
	AlignedRead tmpA;
	int32_t i, j, ctr;
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

	/* If we are going to use the paired end to infer the other end, then
	 * use the BestScoreAll algorithm and try to infer. */
	// Only for di-end reads and best scoring, for now
	if(0 == unpaired && 2 == tmpA.numEnds && BestScore == algorithm &&
			0 < tmpA.ends[0].numEntries && 0 < tmpA.ends[1].numEntries) {
		int32_t bestScoreSE[2] = {INT_MIN, INT_MIN};
		int32_t bestScoreSENum[2] = {0, 0};
		int32_t bestScoreSEIndex[2] = {-1, -1};
		int32_t bestIndex[2] = {-1, -1};
		int32_t bestScore = INT_MIN;
		int32_t bestNum = 0;
		int32_t penultimateIndex[2] = {-1, -1};
		int32_t penultimateScore = INT_MIN;
		int32_t penultimateNum = 0;

		int32_t max_STD = MAX_STD; // TODO: user input

		// Get "best score"
		for(i=0;i<2;i++) {
			for(j=0;j<tmpA.ends[i].numEntries;j++) {
				if(0 == bestScoreSENum[i] || bestScoreSE[i] < tmpA.ends[i].entries[j].score) {
					bestScoreSE[i] = tmpA.ends[i].entries[j].score;
					bestScoreSEIndex[i] = j;
					bestScoreSENum[i]++;
				}
			}
		}

		// all pairs
		// This could be more efficient:
		// 	- only search within a co-ordinate window etc. based on penalty/score
		for(i=0;i<tmpA.ends[0].numEntries;i++) {
			for(j=0;j<tmpA.ends[1].numEntries;j++) {
				int32_t s = (int)(tmpA.ends[0].entries[i].score + tmpA.ends[1].entries[j].score);

				if(0 == reversePaired && tmpA.ends[0].entries[i].strand != tmpA.ends[1].entries[j].strand) { // inversion penalty
					// log10(P) * mismatchScore
					if(0 < b->inversionCount && 0 < b->numDistances) {
						s -= (int)(mismatchScore * -1.0 * log10(b->inversionCount / ((double)b->numDistances)));
					}
					else {
						s -= (int)(mismatchScore * -1.0 * MAX_INVERSION_LOG10_RATIO);
					}
				}
				else if(1 == reversePaired && tmpA.ends[0].entries[i].strand == tmpA.ends[1].entries[j].strand) { // inversion penalty
					// log10(P) * mismatchScore
					if(0 < b->inversionCount && 0 < b->numDistances) {
						s -= (int)(mismatchScore * -1.0 * log10(b->inversionCount / ((double)b->numDistances)));
					}
					else {
						s -= (int)(mismatchScore * -1.0 * MAX_INVERSION_LOG10_RATIO);
					}
				}

				// add penalty for insert size -- assumes normality
				if(tmpA.ends[0].entries[i].contig != tmpA.ends[1].entries[j].contig) { // chimera/inter-chr-translocation
					// TODO: make this a constant calculation
					s -= (int)(mismatchScore * -1.0 * log10( erfc(M_SQRT1_2 * max_STD)) + 0.499);
				}
				else { // same chr
					int32_t l = tmpA.ends[1].entries[j].position - tmpA.ends[0].entries[i].position;
					// penalty times probability of observing etc.
					if(fabs(l - b->avg) / b->std <= max_STD) { 
						s -= (int)(mismatchScore * -1.0 * log10( erfc(M_SQRT1_2 * fabs(l - b->avg) / b->std)) + 0.499);
					}
					else {
						// TODO: make this a constant calculation
						s -= (int)(mismatchScore * -1.0 * log10( erfc(M_SQRT1_2 * max_STD)) + 0.499);
					}
				}

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
		assert(-1 != bestIndex[0]);
		assert(1 <= bestNum);

		// set new mapping quality (?)
		// TODO: set single end mapping quality
		if(1 < bestNum) { // more than one found
			// Revert to best alignment score for each end if they are 
			// unique.  
			for(i=0;i<2;i++) {
				if(bestScoreSENum[1] == 1) { // keep best score
					AlignedEntryCopy(&tmpA.ends[i].entries[0], &tmpA.ends[i].entries[bestScoreSEIndex[i]]);
					AlignedEndReallocate(&tmpA.ends[i], 1);
					foundTypes[0] = Found;
				}
				else { // clear
					foundTypes[i] = NoneFound;
					AlignedEndReallocate(&tmpA.ends[i], 0);
				}
			}
		}
		else {
			// copy to the front and reallocate
			AlignedEntryCopy(&tmpA.ends[0].entries[0], &tmpA.ends[0].entries[bestIndex[0]]);
			AlignedEndReallocate(&tmpA.ends[0], 1);
			foundTypes[0] = Found;
			AlignedEntryCopy(&tmpA.ends[1].entries[0], &tmpA.ends[1].entries[bestIndex[1]]);
			AlignedEndReallocate(&tmpA.ends[1], 1);
			foundTypes[1] = Found;

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
			if(tmpA.ends[0].entries[0].score < bestScoreSE[0] || // changed alignment
					tmpA.ends[0].entries[0].mappingQuality < mapq) { 
				// update mapping quality
				tmpA.ends[0].entries[0].mappingQuality = mapq;
			}
			if(tmpA.ends[1].entries[0].score < bestScoreSE[0] || // changed alignment
					tmpA.ends[1].entries[0].mappingQuality < mapq) { 
				// update mapping quality
				tmpA.ends[1].entries[0].mappingQuality = mapq;
			}
			assert(0 < tmpA.ends[0].entries[0].mappingQuality);
			assert(0 < tmpA.ends[1].entries[0].mappingQuality);
			// TODO: set single end mapq flag
		}
	}
	else {
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
					if(BestScore == algorithm &&
							1 == numBest) {
						foundTypes[i] = Found;
						/* Copy to front */
						AlignedEntryCopy(&tmpA.ends[i].entries[0], 
								&tmpA.ends[i].entries[bestIndex]);
						AlignedEndReallocate(&tmpA.ends[i], 1);
					}
					else if(BestScoreAll == algorithm &&
							1 <= numBest) {
						foundTypes[i] = Found;
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

int32_t GetPEDBins(char *inputFileName, 
		int algorithm,
		int queueLength,
		PEDBins *b)
{
	char *FnName="GetPEDBins";
	gzFile fp=NULL;
	int64_t counter, foundType;
	AlignedRead *aBuffer;
	int32_t aBufferLength=0;
	int32_t numRead, aBufferIndex;

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

	aBufferLength=queueLength;
	aBuffer=malloc(sizeof(AlignedRead)*aBufferLength);
	if(NULL == aBuffer) {
		PrintError(FnName, "aBuffer", "Could not allocate memory", Exit, MallocMemory);
	}

	/* Go through each read */
	if(VERBOSE >= 0) {
		fprintf(stderr, "Estimating paired end distance, currently on:\n0");
	}
	counter = numRead = 0;

	// TODO: make this multi-threaded
	while(0 != (numRead = GetAlignedReads(fp, aBuffer, aBufferLength, NULL))) {

		for(aBufferIndex=0;aBufferIndex<numRead;aBufferIndex++) {

			if(2 == aBuffer[aBufferIndex].numEnds) { // Only paired end data

				if(VERBOSE >= 0 && counter%1000==0) {
					fprintf(stderr, "\r%lld",
							(long long int)counter);
				}

				// Filter base ond best scoring
				foundType=FilterAlignedRead(&aBuffer[aBufferIndex],
						BestScore,
						1,
						-1,
						INT_MIN,
						INT_MIN,
						NULL);

				if(Found == foundType) {
					assert(2 == aBuffer[aBufferIndex].numEnds);
					/* Must only have one alignment per end and on the same contig.
					 * There is a potential this will be inferred incorrectly under
					 * many scenarios.  Be careful! */
					if(1 == aBuffer[aBufferIndex].ends[0].numEntries &&
							1 == aBuffer[aBufferIndex].ends[1].numEntries &&
							aBuffer[aBufferIndex].ends[0].entries[0].contig == aBuffer[aBufferIndex].ends[1].entries[0].contig) {
						PEDBinsInsert(b, 
								aBuffer[aBufferIndex].ends[0].entries[0].strand,
								aBuffer[aBufferIndex].ends[1].entries[0].strand,
								fabs(aBuffer[aBufferIndex].ends[1].entries[0].position - aBuffer[aBufferIndex].ends[0].entries[0].position));
					}
				}
			}

			// break if we have found enough
			if(MAX_PEDBINS_DISTANCES <= b->numDistances) {
				break;
			}

			/* Increment counter */
			counter++;
		}

		if(VERBOSE >= 0) {
			fprintf(stderr, "\r%lld",
					(long long int)counter);
		}

		/* Free buffer */
		for(aBufferIndex=0;aBufferIndex<numRead;aBufferIndex++) {
			/* Free memory */
			AlignedReadFree(&aBuffer[aBufferIndex]);
		}

		// break if we have found enough
		if(MAX_PEDBINS_DISTANCES <= b->numDistances) {
			break;
		}

	}
	if(VERBOSE>=0) {
		fprintf(stderr, "\r%lld\n",
				(long long int)counter);
	}

	/* Close the input file */
	gzclose(fp);
	free(aBuffer);

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

	if(0<=VERBOSE) {
		fprintf(stderr, "Used %d paired end distances to infer the insert size distribution.\n",
				b->numDistances);
		fprintf(stderr, "The paired end distance range was from %d to %d.\n",
				b->minDistance, b->maxDistance);
		fprintf(stderr, "The paired end distance mean and standard deviation was %.2lf and %.2lf.\n",
				b->avg, b->std);
		fprintf(stderr, "The inversion ratio was %lf.\n",
				b->inversionCount * 1.0 / ((double)b->numDistances));
		fprintf(stderr, "%s", BREAK_LINE);
	}
}
