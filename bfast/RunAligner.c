#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "BLibDefinitions.h"
#include "BError.h"
#include "BLib.h"
#include "RGBinary.h"
#include "RGMatch.h" 
#include "RGMatches.h" 
#include "AlignedRead.h"
#include "AlignedEnd.h" 
#include "AlignedEntry.h" 
#include "ScoringMatrix.h"
#include "Align.h"
#include "RunAligner.h"

/* TODO */
void RunAligner(char *fastaFileName,
		char *matchFileName,
		char *scoringMatrixFileName,
		int32_t ungapped,
		int32_t unconstrained,
		int32_t bestOnly,
		int32_t space,
		int32_t startReadNum,
		int32_t endReadNum,
		int32_t offsetLength,
		int32_t maxNumMatches,
		int32_t avgMismatchQuality,
		int32_t numThreads,
		int32_t queueLength,
		int32_t usePairedEndLength,
		int32_t pairedEndLength,
		int32_t mirroringType,
		int32_t forceMirroring,
		char *tmpDir,
		int32_t *totalReferenceGenomeTime,
		int32_t *totalAlignedTime,
		int32_t *totalFileHandlingTime)
{
	char *FnName = "RunAligner";
	gzFile outputFP=NULL;
	gzFile matchFP=NULL;
	int32_t startTime, endTime;
	RGBinary rg;

	startTime = time(NULL);
	RGBinaryReadBinary(&rg,
			NTSpace, // always NT space
			fastaFileName);
	endTime = time(NULL);
	/* Unpack */
	/*
	   RGBinaryUnPack(&rg);
	   */
	(*totalReferenceGenomeTime) = endTime - startTime;

	/* Check rg to make sure it is in NT Space */
	if(rg.space != NTSpace) {
		PrintError(FnName, "rg->space", "The reference genome must be in NT space", Exit, OutOfRange);
	}

	/* Open output file */
	if((outputFP=gzdopen(fileno(stdout), "wb"))==0) {
		PrintError(FnName, "stdout", "Could not open stdout file for writing", Exit, OpenFileError);
	}

	if(VERBOSE >= 0) {
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Reading match file from %s.\n",
				(NULL == matchFileName) ? "stdin" : matchFileName);
	}

	/* Open current match file */
	if(NULL == matchFileName) {
		if((matchFP=gzdopen(fileno(stdin), "rb"))==0) {
			PrintError(FnName, "stdin", "Could not open stdin for reading", Exit, OpenFileError);
		}
	}
	else {
		if((matchFP=gzopen(matchFileName, "rb"))==0) {
			PrintError(FnName, matchFileName, "Could not open file for reading", Exit, OpenFileError);
		}
	}

	RunDynamicProgramming(matchFP,
			&rg,
			scoringMatrixFileName,
			ungapped,
			unconstrained,
			bestOnly,
			space,
			startReadNum,
			endReadNum,
			offsetLength,
			maxNumMatches,
			avgMismatchQuality,
			numThreads,
			queueLength,
			usePairedEndLength,
			pairedEndLength,
			mirroringType,
			forceMirroring,
			tmpDir,
			outputFP,
			totalAlignedTime,
			totalFileHandlingTime);

	if(VERBOSE >= 0) {
		fprintf(stderr, "%s", BREAK_LINE);
	}

	/* Close the match file */
	gzclose(matchFP);

	/* Close output file */
	gzclose(outputFP);

	/* Free the Reference Genome */
	RGBinaryDelete(&rg);
}

/* TODO */
void RunDynamicProgramming(gzFile matchFP,
		RGBinary *rg,
		char *scoringMatrixFileName,
		int32_t ungapped,
		int32_t unconstrained,
		int32_t bestOnly,
		int32_t space,
		int32_t startReadNum,
		int32_t endReadNum,
		int32_t offsetLength,
		int32_t maxNumMatches,
		int32_t avgMismatchQuality,
		int32_t numThreads,
		int32_t queueLength,
		int32_t usePairedEndLength,
		int32_t pairedEndLength,
		int32_t mirroringType,
		int32_t forceMirroring,
		char *tmpDir,
		gzFile outputFP,
		int32_t *totalAlignedTime,
		int32_t *totalFileHandlingTime)
{
	char *FnName="RunDynamicProgramming";
	/* local variables */
	ScoringMatrix sm;
	double mismatchScore;
	RGMatches m;
	int32_t i, ctr;

	int32_t continueReading=0;
	int32_t numMatchesInFile=0;
	int32_t numMatchesFound=0;
	int32_t numAligned=0;
	int32_t numNotAligned=0;
	int32_t startTime, endTime;
	int64_t numLocalAlignments=0;
	AlignedRead aEntries;
	/* Thread specific data */
	ThreadData *data;
	pthread_t *threads=NULL;
	int32_t errCode;
	void *status;

	/* Initialize */
	RGMatchesInitialize(&m);
	ScoringMatrixInitialize(&sm);

	/* Allocate memory for thread arguments */
	data = malloc(sizeof(ThreadData)*numThreads);
	if(NULL==data) {
		PrintError(FnName, "data", "Could not allocate memory", Exit, MallocMemory);
	}
	/* Allocate memory for thread ids */
	threads = malloc(sizeof(pthread_t)*numThreads);
	if(NULL==threads) {
		PrintError(FnName, "threads", "Could not allocate memory", Exit, MallocMemory);
	}

	/* Start file handling timer */
	startTime = time(NULL);

	/* Read in scoring matrix */
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

	/**/
	/* Split the input file into equal temp files for each thread */
	/* Could put this in a separate function */
	/**/

	/* Open temp files for the threads */
	for(i=0;i<numThreads;i++) {
		data[i].inputFP = OpenTmpGZFile(tmpDir, &data[i].inputFileName);
		data[i].outputFP = OpenTmpGZFile(tmpDir, &data[i].outputFileName);
	}

	/* Go through each read in the match file and partition them for the threads */
	if(VERBOSE >= 0) {
		fprintf(stderr, "Filtering and partitioning matches for threads...\n0");
	}
	i=numMatchesInFile=numMatchesFound=0;
	while(EOF!=RGMatchesRead(matchFP, 
				&m)) {

		if(VERBOSE >= 0 && numMatchesFound%PARTITION_MATCHES_ROTATE_NUM==0) {
			fprintf(stderr, "\r[%d]", numMatchesFound);
		}
		numMatchesInFile++;

		/* Ignore reads based on startReadNum and endReadNum */
		if(startReadNum <= numMatchesInFile && numMatchesInFile <= endReadNum) {

			/* increment the number of matches to be aligned */
			numMatchesFound++;

			/* Filter the matches that are not within the bounds */
			/* Filter one if it has too many entries */
			RGMatchesFilterOutOfRange(&m,
					maxNumMatches);

			/* Print32_t match to temp file */
			RGMatchesPrint(data[i].inputFP,
					&m);
			/* Increment */
			i = (i+1)%numThreads;
		}

		/* Free memory */
		RGMatchesFree(&m);
	}
	if(VERBOSE >= 0) {
		fprintf(stderr, "\r[%d]\n", numMatchesFound);
		fprintf(stderr, "Found %d reads.\n",
				numMatchesFound);
	}

	/* End file handling timer */
	endTime = time(NULL);
	(*totalFileHandlingTime) += endTime - startTime;

	/* Create thread arguments */
	for(i=0;i<numThreads;i++) {
		/* Reinitialize file pointer */
		CloseTmpGZFile(&data[i].inputFP, 
				&data[i].outputFileName,
				0);
		if(!(data[i].inputFP=gzopen(data[i].inputFileName, "rb"))) {
			PrintError(FnName, data[i].inputFileName, "Could not re-open file for reading", Exit, OpenFileError);
		}
		data[i].rg=rg;
		data[i].space=space;
		data[i].offsetLength=offsetLength;
		data[i].usePairedEndLength = usePairedEndLength;
		data[i].pairedEndLength = pairedEndLength;
		data[i].mirroringType = mirroringType;
		data[i].forceMirroring = forceMirroring;
		data[i].sm = &sm;
		data[i].ungapped = ungapped;
		data[i].unconstrained = unconstrained;
		data[i].bestOnly = bestOnly;
		data[i].numLocalAlignments = 0;
		data[i].avgMismatchQuality = avgMismatchQuality;
		data[i].mismatchScore = mismatchScore;
		data[i].queueLength = queueLength;
		data[i].threadID = i;
		data[i].numAligned = 0;
		data[i].numNotAligned = 0;
	}

	if(VERBOSE >= 0) {
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Processing %d reads.\n",
				numMatchesFound - numNotAligned
			   );
		fprintf(stderr, "%s", BREAK_LINE);
		fprintf(stderr, "Performing alignment...\n");
		fprintf(stderr, "Currently on:\n0");
	}

	/* Start align timer */
	startTime = time(NULL);

	/* Create threads */
	for(i=0;i<numThreads;i++) {
		/* Start thread */
		errCode = pthread_create(&threads[i], /* thread struct */
				NULL, /* default thread attributes */
				RunDynamicProgrammingThread, /* start routine */
				&data[i]); /* data to routine */
		if(0!=errCode) {
			PrintError(FnName, "pthread_create: errCode", "Could not start thread", Exit, ThreadError);
		}
		if(VERBOSE >= DEBUG) {
			fprintf(stderr, "Created threadID:%d\n",
					i);
		}
	}

	/* Wait for the threads to finish */
	for(i=0;i<numThreads;i++) {
		/* Wait for the given thread to return */
		errCode = pthread_join(threads[i],
				&status);
		if(VERBOSE >= DEBUG) {
			fprintf(stderr, "Thread returned with errCode:%d\n",
					errCode);
		}
		/* Check the return code of the thread */
		if(0!=errCode) {
			PrintError(FnName, "pthread_join: errCode", "Thread returned an error", Exit, ThreadError);
		}
		/* Reinitialize file pointer */
		CloseTmpGZFile(&data[i].outputFP, 
				&data[i].outputFileName,
				0);
		if(!(data[i].outputFP=gzopen(data[i].outputFileName, "rb"))) {
			PrintError(FnName, data[i].outputFileName, "Could not re-open file for reading", Exit, OpenFileError);
		}
	}
	if(VERBOSE >= 0) {
		fprintf(stderr, "\n");
		fprintf(stderr, "Alignment complete.\n");
	}

	/* End align timer */
	endTime = time(NULL);
	(*totalAlignedTime) += endTime - startTime;

	/* Start file handling timer */
	startTime = time(NULL);

	/* Close tmp input files */
	for(i=0;i<numThreads;i++) {
		CloseTmpGZFile(&data[i].inputFP, &data[i].inputFileName, 1);
		numLocalAlignments += data[i].numLocalAlignments;
	}

	/* Merge all the aligned reads from the threads */
	if(VERBOSE >=0) {
		fprintf(stderr, "Merging and outputting aligned reads from threads...\n[0]");
	}
	AlignedReadInitialize(&aEntries);
	continueReading=1;
	ctr=0;
	while(continueReading==1) {
		/* Get an align from a thread */
		continueReading=0;
		for(i=0;i<numThreads;i++) {
			/* Read in the align entries */
			if(EOF != AlignedReadRead(&aEntries, data[i].outputFP)) {
				if(VERBOSE >=0 && ctr%ALIGN_ROTATE_NUM == 0) {
					fprintf(stderr, "\r[%d]", ctr);
				}
				ctr++;
				continueReading=1;
				/* Print32_t it out */
				AlignedReadPrint(&aEntries,
						outputFP);
			}
			AlignedReadFree(&aEntries);
		}
	}
	if(VERBOSE >=0) {
		fprintf(stderr, "\r[%d]\n", ctr);
	}
	numAligned = numNotAligned = 0;
	for(i=0;i<numThreads;i++) {
		numAligned += data[i].numAligned;
		numNotAligned += data[i].numNotAligned;
	}

	/* Close tmp output files */
	for(i=0;i<numThreads;i++) {
		CloseTmpGZFile(&data[i].outputFP, &data[i].outputFileName, 1);
	}

	/* Start file handling timer */
	endTime = time(NULL);
	(*totalFileHandlingTime) += endTime - startTime;

	if(VERBOSE >=0) {
		fprintf(stderr, "Performed %lld local alignments.\n", (long long int)numLocalAlignments);
		fprintf(stderr, "Outputted alignments for %d reads.\n", numAligned);
		fprintf(stderr, "Outputted %d reads for which there were no alignments.\n", numNotAligned); 
		fprintf(stderr, "Outputting complete.\n");
	}
	assert(numAligned + numNotAligned == numMatchesFound);

	/* Free memory */
	free(data);
	free(threads);
	AlignedReadFree(&aEntries);
}

/* TODO */
void *RunDynamicProgrammingThread(void *arg)
{
	/* Recover arguments */
	ThreadData *data = (ThreadData *)(arg);
	gzFile inputFP=data->inputFP;
	gzFile outputFP=data->outputFP;
	RGBinary *rg=data->rg;
	int32_t space=data->space;
	int32_t offsetLength=data->offsetLength;
	int32_t usePairedEndLength=data->usePairedEndLength;
	int32_t pairedEndLength=data->pairedEndLength;
	int32_t mirroringType=data->mirroringType;
	int32_t forceMirroring=data->forceMirroring;
	ScoringMatrix *sm = data->sm;
	int32_t ungapped=data->ungapped;
	int32_t unconstrained=data->unconstrained;
	int32_t bestOnly=data->bestOnly;
	int32_t threadID=data->threadID;
	int32_t avgMismatchQuality=data->avgMismatchQuality;
	double mismatchScore=data->mismatchScore;
	int32_t queueLength=data->queueLength;
	/* Local variables */
	char *FnName = "RunDynamicProgrammingThread";
	int32_t i, j, wasAligned;
	int32_t numAlignedRead=0;
	int32_t numMatches=0;
	int32_t ctrOne=0;
	int32_t ctrTwo=0;

	AlignMatrixNT **matrixNT=NULL;
	AlignMatrixCS **matrixCS=NULL;
	int32_t maxReadLength=0, maxReferenceLength=0;
	AlignedRead *alignedQueue=NULL;
	RGMatches *matchQueue=NULL;
	int32_t matchQueueLength=queueLength;
	int32_t numMatchesRead=0;

	/* Allocate match queue */
	matchQueue = malloc(sizeof(RGMatches)*matchQueueLength);
	if(NULL == matchQueue) {
		PrintError(FnName, "matchQueue", "Could not allocate memory", Exit, MallocMemory);
	}
	alignedQueue = malloc(sizeof(AlignedRead)*matchQueueLength);
	if(NULL == alignedQueue) {
		PrintError(FnName, "alignedQueue", "Could not allocate memory", Exit, MallocMemory);
	}

	/* Initialize */

	/* Go through each read in the match file */
	while(0 != (numMatchesRead = GetMatches(inputFP, matchQueue, matchQueueLength))) {

		for(i=0;i<numMatchesRead;i++) {
			AlignedReadInitialize(&alignedQueue[i]);

			wasAligned=0;
			if(1 == IsValidMatch(&matchQueue[i])) {
				numMatches++;
				numAlignedRead = 0;
				ctrOne=ctrTwo=0;

				if(VERBOSE >= 0 && numMatches%ALIGN_ROTATE_NUM==0) {
					fprintf(stderr, "\rthread:%d\t[%d]", threadID, numMatches);
				}

				/* Update the number of local alignments performed */
				data->numLocalAlignments += AlignRGMatches(&matchQueue[i],
						rg,
						&alignedQueue[i],
						space,
						offsetLength,
						sm,
						ungapped,
						unconstrained,
						bestOnly,
						usePairedEndLength,
						pairedEndLength,
						mirroringType,
						forceMirroring,
						&matrixNT,
						&matrixCS,
						&maxReadLength,
						&maxReferenceLength
						);

				/* Output alignment */
				for(j=wasAligned=0;0==wasAligned && j<alignedQueue[i].numEnds;j++) {
					if(0 < alignedQueue[i].ends[j].numEntries) {
						wasAligned = 1;
					}
				}

				if(1 == wasAligned) {
					/* Remove duplicates */
					AlignedReadRemoveDuplicates(&alignedQueue[i],
							AlignedEntrySortByAll);
					/* Updating mapping quality */
					AlignedReadUpdateMappingQuality(&alignedQueue[i], 
							mismatchScore, 
							avgMismatchQuality);
				}
			}
			else {
				/* Copy over to alignedQueue[i] */
				AlignedReadAllocate(&alignedQueue[i],
						matchQueue[i].readName,
						matchQueue[i].numEnds,
						space);
				for(j=0;j<matchQueue[i].numEnds;j++) {
					AlignedEndAllocate(&alignedQueue[i].ends[j],
							matchQueue[i].ends[j].read,
							matchQueue[i].ends[j].qual,
							0);
				}
			}

			if(0 == wasAligned) {
				data->numNotAligned++;
			}
			else {
				data->numAligned++;
			}

			/* Free memory */
			RGMatchesFree(&matchQueue[i]);
		}

		if(VERBOSE >= 0) {
			fprintf(stderr, "\rthread:%d\t[%d]", threadID, numMatches);
		}

		/* Print32_t */
		for(i=0;i<numMatchesRead;i++) {
			AlignedReadPrint(&alignedQueue[i],
					outputFP);

			/* Free memory */
			AlignedReadFree(&alignedQueue[i]);
		}
	}
	/* Free the matrix, free your mind */
	if(NTSpace == space) {
		for(i=0;i<maxReadLength+1;i++) {
			free(matrixNT[i]);
			matrixNT[i]=NULL;
		}
		free(matrixNT);
		matrixNT=NULL;
	}
	else {
		for(i=0;i<maxReadLength+1;i++) {
			free(matrixCS[i]);
			matrixCS[i]=NULL;
		}
		free(matrixCS);
		matrixCS=NULL;
	}

	if(VERBOSE >= 0) {
		fprintf(stderr, "\rthread:%d\t[%d]", threadID, numMatches);
	}

	free(matchQueue);

	return arg;
}

int32_t GetMatches(gzFile fp, RGMatches *m, int32_t maxToRead)
{
	int32_t numRead = 0;

	while(numRead < maxToRead) {
		RGMatchesInitialize(&(m[numRead]));
		if(EOF == RGMatchesRead(fp, &(m[numRead]))) {
			return numRead;
		}
		numRead++;
	}
	return numRead;
}
