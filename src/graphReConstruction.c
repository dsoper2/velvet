/*
Copyright 2007, 2008 Daniel Zerbino (zerbino@ebi.ac.uk)

    This file is part of Velvet.

    Velvet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Velvet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Velvet; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#ifdef OPENMP
#include <omp.h>
#endif

#include "globals.h"
#include "graph.h"
#include "passageMarker.h"
#include "readSet.h"
#include "tightString.h"
#include "recycleBin.h"
#include "utility.h"
#include "kmer.h"
#include "kmerOccurenceTable.h"
#include "roadMap.h"

#define ADENINE 0
#define CYTOSINE 1
#define GUANINE 2
#define THYMINE 3

//////////////////////////////////////////////////////////
// Node Lists
//////////////////////////////////////////////////////////
typedef struct smallNodeList_st SmallNodeList;

struct smallNodeList_st {
	Node *node;
	SmallNodeList *next;
} ATTRIBUTE_PACKED;

static RecycleBin *smallNodeListMemory = NULL;

#define BLOCKSIZE 1000

static SmallNodeList *allocateSmallNodeList()
{
	if (smallNodeListMemory == NULL)
		smallNodeListMemory =
		    newRecycleBin(sizeof(SmallNodeList), BLOCKSIZE);

	return allocatePointer(smallNodeListMemory);
}

static void deallocateSmallNodeList(SmallNodeList * smallNodeList)
{
	deallocatePointer(smallNodeListMemory, smallNodeList);
}

static void memorizeNode(Node * node, SmallNodeList ** nodePile)
{
	SmallNodeList *list = allocateSmallNodeList();
	list->node = node;
	list->next = *nodePile;
	*nodePile = list;
}

static boolean nodeMemorized(Node * node, SmallNodeList * nodePile) {
	SmallNodeList * list;

	for (list = nodePile; list; list = list->next)
		if (list->node == node)
			return true;

	return false;
}

static void unlockMemorizedNodes(SmallNodeList ** nodePile)
{
	SmallNodeList * list;

	while (*nodePile) {
		list = *nodePile;
		*nodePile = list->next;
		deallocateSmallNodeList(list);
	}
}

///////////////////////////////////////////////////////////
// Reference Mappings
///////////////////////////////////////////////////////////
typedef struct referenceMapping_st ReferenceMapping;

struct referenceMapping_st {
	IDnum referenceStart;
	IDnum nodeStart;
	IDnum length;
	IDnum referenceID;
	IDnum nodeID;
} ATTRIBUTE_PACKED;

static IDnum countMappings(char * preGraphFilename) {
	FILE *file = fopen(preGraphFilename, "r");
	const int maxline = MAXLINE;
	char line[MAXLINE];
	IDnum count = 0;

	// Go past NODE blocks
	while(fgets(line, maxline, file))
		if (line[0] == 'S')
			break;

	// Count relevant lines
	while(fgets(line, maxline, file))
		if (line[0] != 'S')
			count++;

	fclose(file);
	return count;
}

static ReferenceMapping * recordReferenceMappings(char * preGraphFilename, IDnum arrayLength) {
	ReferenceMapping * mappings = callocOrExit(arrayLength, ReferenceMapping);
	FILE *file = fopen(preGraphFilename, "r");
	const int maxline = MAXLINE;
	char line[MAXLINE];
	ReferenceMapping * current = mappings;
	IDnum referenceID;
	long long_var;
	long long coord1, coord2, coord3; 
	
	// Go past NODE blocks
	while(fgets(line, maxline, file))
		if (line[0] == 'S')
			break;

	sscanf(line, "SEQ\t%li\n", &long_var);
	referenceID = long_var;

	// Go relevant lines
	while(fgets(line, maxline, file)) {
		if (line[0] != 'S') {
			sscanf(line, "%li\t%lli\t%lli\t%lli\n", &long_var, &coord1, &coord2, &coord3);
			current->referenceID = referenceID;
			current->nodeID = long_var;
			current->nodeStart = coord1;
			current->referenceStart = coord2;
			current->length = coord3;
			current++;
		} else {
			sscanf(line, "SEQ\t%li\n", &long_var);
			referenceID = long_var;
		} 
	}

	fclose(file);
	return mappings;
}

static int compareRefMaps(const void * ptrA, const void * ptrB) {
	ReferenceMapping * A = (ReferenceMapping *) ptrA;
	ReferenceMapping * B = (ReferenceMapping *) ptrB;

	if (A->referenceID > B->referenceID) 
		return 1;
	else if (A->referenceID < B->referenceID)
		return -1;
	else {
		if (A->referenceStart >= B->referenceStart + B->length)
			return 1;
		else if (A->referenceStart + A->length <= B->referenceStart)
			return -1;
		else 
			return 0;
	}
}

static ReferenceMapping * computeReferenceMappings(char * preGraphFilename, ReadSet * reads, Coordinate * referenceMappingLength, IDnum * referenceCount) {
	IDnum index;
	ReferenceMapping * referenceMappings;

	for(index = 0; index < reads->readCount && reads->categories[index] == 2 * CATEGORIES + 2; index++)
		(*referenceCount)++;

	if (*referenceCount == 0) {
		*referenceMappingLength = 0;
		return NULL;
	}

	*referenceMappingLength = countMappings(preGraphFilename);
	
	if (*referenceMappingLength == 0)
		return NULL;

	referenceMappings = recordReferenceMappings(preGraphFilename, *referenceMappingLength); 
	qsort(referenceMappings, *referenceMappingLength, sizeof(ReferenceMapping), compareRefMaps);

	return referenceMappings;
}

static ReferenceMapping * findReferenceMapping(IDnum seqID, Coordinate refCoord, ReferenceMapping * referenceMappings, Coordinate referenceMappingCount) {
	IDnum positive_seqID;
	Coordinate leftIndex = 0;
	Coordinate rightIndex = referenceMappingCount - 1;
	Coordinate middleIndex;
	ReferenceMapping refMap;
	int comparison;

	if (seqID > 0)
		positive_seqID = seqID;
	else
		positive_seqID = -seqID;

	refMap.referenceID = positive_seqID;
	refMap.referenceStart = refCoord;
	refMap.length = 1;
	refMap.nodeStart = 0;
	refMap.nodeID = 0;

	if (compareRefMaps(&(referenceMappings[leftIndex]), &refMap) == 0)
		return &(referenceMappings[leftIndex]);
	if (compareRefMaps(&(referenceMappings[rightIndex]), &refMap) == 0)
		return &(referenceMappings[rightIndex]);

	while (true) {
		middleIndex = (rightIndex + leftIndex) / 2;
		comparison = compareRefMaps(&(referenceMappings[middleIndex]), &refMap);

		if (leftIndex >= rightIndex)
			return NULL;
		else if (comparison == 0)
			return &(referenceMappings[middleIndex]);
		else if (leftIndex == middleIndex)
			return NULL;
		else if (comparison > 0)
			rightIndex = middleIndex;
		else
			leftIndex = middleIndex;
	}
}
 
///////////////////////////////////////////////////////////
// Node Mask
///////////////////////////////////////////////////////////

typedef struct nodeMask_st NodeMask;

struct nodeMask_st {
	IDnum nodeID;
	IDnum start;
	IDnum finish;
} ATTRIBUTE_PACKED;

static int compareNodeMasks(const void * ptrA, const void * ptrB) {
	NodeMask * A = (NodeMask *) ptrA;
	NodeMask * B = (NodeMask *) ptrB;
	
	if (A->nodeID < B->nodeID)
		return -1;
	else if (A->nodeID > B->nodeID)
		return 1;
	else {
		if (A->start < B->start)
			return -1;
		else if (A->start > B->start)
			return 1;
		else 
			return 0;
	}
}

static NodeMask * computeNodeMasks(ReferenceMapping * referenceMappings, Coordinate arrayLength, Graph * graph) {
	NodeMask * nodeMasks;
	NodeMask * currentMask;
	ReferenceMapping * currentMapping = referenceMappings;
	Coordinate index;

	if (referenceMappings == NULL)
		return NULL;

	nodeMasks = callocOrExit(arrayLength, NodeMask);
	currentMask = nodeMasks;

	for (index = 0; index < arrayLength; index++) {
		if (currentMapping->nodeID > 0) {
			currentMask->nodeID = currentMapping->nodeID;
		} else {
			currentMask->nodeID = -currentMapping->nodeID;
		}
		currentMask->start = currentMapping->nodeStart;
		currentMask->finish = currentMapping->nodeStart + currentMapping->length;
		currentMask++;
		currentMapping++;
	}

	qsort(nodeMasks, arrayLength, sizeof(NodeMask), compareNodeMasks);

	return nodeMasks;
}

///////////////////////////////////////////////////////////
// Process
///////////////////////////////////////////////////////////

static KmerOccurenceTable *referenceGraphKmers(char *preGraphFilename,
					       short int accelerationBits, Graph * graph, boolean double_strand, NodeMask * nodeMasks, Coordinate nodeMaskCount)
{
	FILE *file = fopen(preGraphFilename, "r");
	const int maxline = MAXLINE;
	char line[MAXLINE];
	char c;
	int wordLength;
	Coordinate lineLength, kmerCount;
	Kmer word;
	Kmer antiWord;
	KmerOccurenceTable *kmerTable;
	IDnum index;
	IDnum nodeID = 0;
	Nucleotide nucleotide;
	NodeMask * nodeMask = nodeMasks; 
	Coordinate nodeMaskIndex = 0;

	if (file == NULL)
		exitErrorf(EXIT_FAILURE, true, "Could not open %s", preGraphFilename);

	// Count kmers
	velvetLog("Scanning pre-graph file %s for k-mers\n",
		  preGraphFilename);

	// First  line
	if (!fgets(line, maxline, file))
		exitErrorf(EXIT_FAILURE, true, "PreGraph file incomplete");
	sscanf(line, "%*i\t%*i\t%i\n", &wordLength);

	kmerTable = newKmerOccurenceTable(accelerationBits, wordLength);

	// Read nodes
	if (!fgets(line, maxline, file))
		exitErrorf(EXIT_FAILURE, true, "PreGraph file incomplete");
	kmerCount = 0;
	while (line[0] == 'N') {
		lineLength = 0;
		while ((c = getc(file)) != EOF && c != '\n')
			lineLength++;
		kmerCount += lineLength - wordLength + 1;
		if (fgets(line, maxline, file) == NULL)
			break;
	}

	velvetLog("%li kmers found\n", (long) kmerCount);

	for(nodeMaskIndex = 0; nodeMaskIndex < nodeMaskCount; nodeMaskIndex++) {
		kmerCount -= nodeMasks[nodeMaskIndex].finish -
nodeMasks[nodeMaskIndex].start;
	}

	nodeMaskIndex = 0;

	fclose(file);

	// Create table
	allocateKmerOccurences(kmerCount, kmerTable);

	// Fill table
	file = fopen(preGraphFilename, "r");
	if (file == NULL)
		exitErrorf(EXIT_FAILURE, true, "Could not open %s", preGraphFilename);

	if (!fgets(line, maxline, file))
		exitErrorf(EXIT_FAILURE, true, "PreGraph file incomplete");

	// Read nodes
	if (!fgets(line, maxline, file))
		exitErrorf(EXIT_FAILURE, true, "PreGraph file incomplete");
	while (line[0] == 'N') {
		nodeID++;

		// Fill in the initial word : 
		clearKmer(&word);
		clearKmer(&antiWord);

		for (index = 0; index < wordLength - 1; index++) {
			c = getc(file);
			if (c == 'A')
				nucleotide = ADENINE;
			else if (c == 'C')
				nucleotide = CYTOSINE;
			else if (c == 'G')
				nucleotide = GUANINE;
			else if (c == 'T')
				nucleotide = THYMINE;
			else if (c == '\n')
				exitErrorf(EXIT_FAILURE, true, "PreGraph file incomplete");
			else
				nucleotide = ADENINE;
				

			pushNucleotide(&word, nucleotide);
			if (double_strand) {
#ifdef COLOR
				reversePushNucleotide(&antiWord, nucleotide);
#else
				reversePushNucleotide(&antiWord, 3 - nucleotide);
#endif
			}
		}

		// Scan through node
		index = 0;
		while((c = getc(file)) != '\n' && c != EOF) {
			if (c == 'A')
				nucleotide = ADENINE;
			else if (c == 'C')
				nucleotide = CYTOSINE;
			else if (c == 'G')
				nucleotide = GUANINE;
			else if (c == 'T')
				nucleotide = THYMINE;
			else
				nucleotide = ADENINE;

			pushNucleotide(&word, nucleotide);
			if (double_strand) {
#ifdef COLOR
				reversePushNucleotide(&antiWord, nucleotide);
#else
				reversePushNucleotide(&antiWord, 3 - nucleotide);
#endif
			}

			// Update mask if necessary 
			if (nodeMask) { 
				if (nodeMask->nodeID < nodeID || (nodeMask->nodeID == nodeID && index >= nodeMask->finish)) {
					if (++nodeMaskIndex == nodeMaskCount) 
						nodeMask = NULL;
					else 
						nodeMask++;
				}
			}

			// Check if not masked!
			if (nodeMask) { 
				if (nodeMask->nodeID == nodeID && index >= nodeMask->start && index < nodeMask->finish) {
					index++;
					continue;
				} 			
			}

			if (!double_strand || compareKmers(&word, &antiWord) <= 0)
				recordKmerOccurence(&word, nodeID, index, kmerTable);
			else
				recordKmerOccurence(&antiWord, -nodeID, getNodeLength(getNodeInGraph(graph, nodeID)) - 1 - index, kmerTable);

			index++;
		}

		if (fgets(line, maxline, file) == NULL)
			break;
	}

	fclose(file);

	// Sort table
	sortKmerOccurenceTable(kmerTable);

	return kmerTable;
}

static void ghostThreadSequenceThroughGraph(TightString * tString,
					    KmerOccurenceTable *
					    kmerTable, Graph * graph,
					    IDnum seqID, Category category,
					    boolean readTracking,
					    boolean double_strand,
					    ReferenceMapping * referenceMappings,
					    Coordinate referenceMappingCount,
					    IDnum refCount,
					    Annotation * annotations,
					    IDnum annotationCount,
					    boolean second_in_pair)
{
	Kmer word;
	Kmer antiWord;
	Coordinate readNucleotideIndex;
	KmerOccurence *kmerOccurence;
	int wordLength = getWordLength(graph);
	Nucleotide nucleotide;
	IDnum refID;
	Coordinate refCoord;
	ReferenceMapping * refMap = NULL;
	Coordinate uniqueIndex = 0;
	Coordinate annotIndex = 0;
	IDnum annotCount = 0;
	boolean reversed;
	SmallNodeList * nodePile = NULL;
	Annotation * annotation = annotations;

	Node *node;
	Node *previousNode = NULL;

	clearKmer(&word);
	clearKmer(&antiWord);

	// Neglect any read which will not be short paired
	if ((!readTracking && category % 2 == 0)
	    || category / 2 >= CATEGORIES)
		return;

	// Neglect any string shorter than WORDLENGTH :
	if (getLength(tString) < wordLength)
		return;

	// Verify that all short reads are reasonnably short
	if (getLength(tString) > USHRT_MAX) {
		velvetLog("Short read of length %lli, longer than limit %i\n",
			  (long long) getLength(tString), SHRT_MAX);
		velvetLog("You should better declare this sequence as long, because it genuinely is!\n");
		exit(1);
	}
	// Allocate memory for the read pairs
	if (!readStartsAreActivated(graph))
		activateReadStarts(graph);

	// Fill in the initial word : 
	for (readNucleotideIndex = 0;
	     readNucleotideIndex < wordLength - 1; readNucleotideIndex++) {
		nucleotide = getNucleotide(readNucleotideIndex, tString);
		pushNucleotide(&word, nucleotide);
		if (double_strand || second_in_pair) {
#ifdef COLOR
			reversePushNucleotide(&antiWord, nucleotide);
#else
			reversePushNucleotide(&antiWord, 3 - nucleotide);
#endif
		}
	}

	// Go through sequence
	while (readNucleotideIndex < getLength(tString)) {
		// Shift word:
		nucleotide = getNucleotide(readNucleotideIndex++, tString);
		pushNucleotide(&word, nucleotide);
		if (double_strand || second_in_pair) {
#ifdef COLOR
			reversePushNucleotide(&antiWord, nucleotide);
#else
			reversePushNucleotide(&antiWord, 3 - nucleotide);
#endif
		}

		// Update annotation if necessary
		if (annotCount < annotationCount && annotIndex == getAnnotationLength(annotation)) {
			annotation = getNextAnnotation(annotation);
			annotCount++;
		}

		// Search for reference mapping
		if (annotCount < annotationCount && uniqueIndex >= getPosition(annotation) && getAnnotSequenceID(annotation) <= refCount && getAnnotSequenceID(annotation) >= -refCount) {
			refID = getAnnotSequenceID(annotation);
			if (refID > 0)
				refCoord = getStart(annotation) + annotIndex;
			else
				refCoord = getStart(annotation) - annotIndex;
			
			refMap = findReferenceMapping(refID, refCoord, referenceMappings, referenceMappingCount);
			// If success
			if (refMap) {
				if (refID > 0) 
					node = getNodeInGraph(graph, refMap->nodeID);
				else
					node = getNodeInGraph(graph, -refMap->nodeID);
			} else  {
				node = NULL;
				if (previousNode)
					break;
			}
		}		
		// if not.. look in table
		else {
			reversed = false;
			if (double_strand) {
				if (compareKmers(&word, &antiWord) <= 0) {
					kmerOccurence =
					findKmerInKmerOccurenceTable(&word,
								       kmerTable);
				} else { 
					kmerOccurence =
					       findKmerInKmerOccurenceTable(&antiWord,
						kmerTable);
					reversed = true;
				}
			} else {
				if (!second_in_pair) {
					kmerOccurence =
					findKmerInKmerOccurenceTable(&word,
								       kmerTable);
				} else { 
					kmerOccurence =
					       findKmerInKmerOccurenceTable(&antiWord,
						kmerTable);
					reversed = true;
				}
			}
			
			if (kmerOccurence) {
				if (!reversed) 
					node = getNodeInGraph(graph, getKmerOccurenceNodeID(kmerOccurence));
				else
					node = getNodeInGraph(graph, -getKmerOccurenceNodeID(kmerOccurence));
			} else {
				node = NULL;
				if (previousNode) 
					break;
			}

		}

		if (annotCount < annotationCount && uniqueIndex >= getPosition(annotation))
			annotIndex++;
		else
			uniqueIndex++;

		previousNode = node;

		// Fill in graph
#ifdef OPENMP
		#pragma omp critical 
#endif
		if (node && !nodeMemorized(node, nodePile)) {
			incrementReadStartCount(node, graph);
			memorizeNode(node, &nodePile);
		}
	}

	unlockMemorizedNodes(&nodePile);
}

static void threadSequenceThroughGraph(TightString * tString,
				       KmerOccurenceTable * kmerTable,
				       Graph * graph,
				       IDnum seqID, Category category,
				       boolean readTracking,
				       boolean double_strand,
				       ReferenceMapping * referenceMappings,
				       Coordinate referenceMappingCount,
				       IDnum refCount,
				       Annotation * annotations,
				       IDnum annotationCount,
				       boolean second_in_pair)
{
	Kmer word;
	Kmer antiWord;
	Coordinate readNucleotideIndex;
	Coordinate kmerIndex;
	KmerOccurence *kmerOccurence;
	int wordLength = getWordLength(graph);

	PassageMarkerI marker = NULL_IDX;
	PassageMarkerI previousMarker = NULL_IDX;
	Node *node;
	Node *previousNode = NULL;
	Coordinate coord = 0;
	Coordinate previousCoord = 0;
	Nucleotide nucleotide;
	boolean reversed;

	IDnum refID;
	Coordinate refCoord = 0;
	ReferenceMapping * refMap;
	Annotation * annotation = annotations;
	Coordinate index = 0;
	Coordinate uniqueIndex = 0;
	Coordinate annotIndex = 0;
	IDnum annotCount = 0;
	SmallNodeList * nodePile = NULL;

	clearKmer(&word);
	clearKmer(&antiWord);

	// Neglect any string shorter than WORDLENGTH :
	if (getLength(tString) < wordLength)
		return;

	// Fill in the initial word : 
	for (readNucleotideIndex = 0;
	     readNucleotideIndex < wordLength - 1; readNucleotideIndex++) {
		nucleotide = getNucleotide(readNucleotideIndex, tString);
		pushNucleotide(&word, nucleotide);
		if (double_strand || second_in_pair) {
#ifdef COLOR
			reversePushNucleotide(&antiWord, nucleotide);
#else
			reversePushNucleotide(&antiWord, 3 - nucleotide);
#endif
		}
	}

	// Go through sequence
	while (readNucleotideIndex < getLength(tString)) {
		nucleotide = getNucleotide(readNucleotideIndex++, tString);
		pushNucleotide(&word, nucleotide);
		if (double_strand || second_in_pair) {
#ifdef COLOR
			reversePushNucleotide(&antiWord, nucleotide);
#else
			reversePushNucleotide(&antiWord, 3 - nucleotide);
#endif
		}

		// Update annotation if necessary
		if (annotCount < annotationCount && annotIndex == getAnnotationLength(annotation)) {
			annotation = getNextAnnotation(annotation);
			annotCount++;
		}

		// Search for reference mapping
		if (category == REFERENCE) {
			if (referenceMappings) 
				refMap = findReferenceMapping(seqID, index, referenceMappings, referenceMappingCount);
			else 
				refMap = NULL;

			if (refMap) {
				node = getNodeInGraph(graph, refMap->nodeID);
				if (refMap->nodeID > 0) {
					coord = refMap->nodeStart + (index - refMap->referenceStart);
				} else {
					coord = getNodeLength(node) - refMap->nodeStart - refMap->length + (index - refMap->referenceStart);
				}
			} else  {
				node = NULL;
				if (previousNode)
					break;
			}
		}
		// Search for reference-based mapping
		else if (annotCount < annotationCount && uniqueIndex >= getPosition(annotation) && getAnnotSequenceID(annotation) <= refCount && getAnnotSequenceID(annotation) >= -refCount) {
			refID = getAnnotSequenceID(annotation);
			if (refID > 0)
				refCoord = getStart(annotation) + annotIndex; 
			else
				refCoord = getStart(annotation) - annotIndex; 
			
			refMap = findReferenceMapping(refID, refCoord, referenceMappings, referenceMappingCount);
			// If success
			if (refMap) {
				if (refID > 0) {
					node = getNodeInGraph(graph, refMap->nodeID);
					if (refMap->nodeID > 0) {
						coord = refMap->nodeStart + (refCoord - refMap->referenceStart);
					} else {
						coord = getNodeLength(node) - refMap->nodeStart - refMap->length + (refCoord - refMap->referenceStart);
					}
				} else {
					node = getNodeInGraph(graph, -refMap->nodeID);
					if (refMap->nodeID > 0) {
						coord =  getNodeLength(node) - refMap->nodeStart - (refCoord - refMap->referenceStart) - 1;
					} else {
						coord = refMap->nodeStart + refMap->length - (refCoord - refMap->referenceStart) - 1;
					}
				}
			} else  {
				node = NULL;
				if (previousNode)
					break;
			}
		}		
		// Search in table
		else {
			reversed = false;
			if (double_strand) {
				if (compareKmers(&word, &antiWord) <= 0) {
					kmerOccurence =
					findKmerInKmerOccurenceTable(&word,
								       kmerTable);
				} else { 
					kmerOccurence =
					       findKmerInKmerOccurenceTable(&antiWord,
						kmerTable);
					reversed = true;
				}
			} else {
				if (!second_in_pair) {
					kmerOccurence =
					findKmerInKmerOccurenceTable(&word,
								       kmerTable);
				} else { 
					kmerOccurence =
					       findKmerInKmerOccurenceTable(&antiWord,
						kmerTable);
					reversed = true;
				}
			}
			
			if (kmerOccurence) {
				if (!reversed) {
					node = getNodeInGraph(graph, getKmerOccurenceNodeID(kmerOccurence));
					coord = getKmerOccurencePosition(kmerOccurence);
				} else {
					node = getNodeInGraph(graph, -getKmerOccurenceNodeID(kmerOccurence));
					coord = getNodeLength(node) - getKmerOccurencePosition(kmerOccurence) - 1;
				}
			} else {
				node = NULL;
				if (previousNode) 
					break;
			}
		}

		// Increment positions
		if (annotCount < annotationCount && uniqueIndex >= getPosition(annotation)) 
			annotIndex++;
		else
			uniqueIndex++;

		// Fill in graph
#ifdef OPENMP
		#pragma omp critical
#endif 
		if (node) {
			kmerIndex = readNucleotideIndex - wordLength;

			if (previousNode == node
			    && previousCoord == coord - 1) {
				if (category / 2 >= CATEGORIES) {
					setPassageMarkerFinish(marker,
							       kmerIndex +
							       1);
					setFinishOffset(marker,
							getNodeLength(node)
							- coord - 1);
				} else {
					incrementVirtualCoverage(node,
								 category /
								 2, 1);
					incrementOriginalVirtualCoverage
					    (node, category / 2, 1);
				}

			} else {
				if (category / 2 >= CATEGORIES) {
					marker =
					    newPassageMarker(seqID,
							     kmerIndex,
							     kmerIndex + 1,
							     coord,
							     getNodeLength
							     (node) -
							     coord - 1);
					transposePassageMarker(marker,
							       node);
					connectPassageMarkers
					    (previousMarker, marker,
					     graph);
					previousMarker = marker;
				} else {
					if (readTracking) {
						if (!nodeMemorized(node, nodePile)) {
							addReadStart(node,
								     seqID,
								     coord,
								     graph,
								     kmerIndex);
							memorizeNode(node, &nodePile);
						} else {
							blurLastShortReadMarker
							    (node, graph);
						}
					}

					incrementVirtualCoverage(node,
								 category /
								 2, 1);
					incrementOriginalVirtualCoverage
					    (node, category / 2, 1);
				}

				createArc(previousNode, node, graph);
			}

			previousNode = node;
			previousCoord = coord;
		}

		index++;
	}

	unlockMemorizedNodes(&nodePile);
}

static void fillUpGraph(ReadSet * reads,
			KmerOccurenceTable * kmerTable, Graph * graph,
			boolean readTracking, boolean double_strand,
			ReferenceMapping * referenceMappings, Coordinate referenceMappingCount,
			IDnum refCount,
			char * roadmapFilename)
{
	IDnum readIndex;
	Category category;
	FILE * file = NULL;
	char line[MAXLINE];
	boolean second_in_pair = false;
	Annotation * annotations; 
	IDnum annotationCount = 0;
	
	if (referenceMappings) {
		file = fopen(roadmapFilename, "r");
		for (readIndex = 0; readIndex < refCount + 1; readIndex++)
			while(fgets(line, MAXLINE, file))
				if (line[0] == 'R')
					break;
	}

	resetNodeStatus(graph);

#ifdef OPENMP
	#pragma omp parallel for
#endif
	for (readIndex = refCount; readIndex < reads->readCount; readIndex++) {

#ifdef OPENMP
		#pragma omp critical 
#endif
		if (referenceMappings) 
			annotations = importAnnotations(file, &readIndex, &annotationCount);

		category = reads->categories[readIndex];
		second_in_pair = reads->categories[readIndex] % 2 && isSecondInPair(reads, readIndex);
	
		ghostThreadSequenceThroughGraph(getTightStringInArray(reads->tSequences, readIndex),
						kmerTable,
						graph, readIndex + 1,
						category, 
						readTracking, double_strand,
						referenceMappings, referenceMappingCount,
					  	refCount, annotations, annotationCount,
						second_in_pair);

		if (referenceMappings)
			free(annotations);
	}
	if (file) 
		fclose(file);

	createNodeReadStartArrays(graph);

	if (referenceMappings) {
		file = fopen(roadmapFilename, "r");
		while(fgets(line, MAXLINE, file))
			if (line[0] == 'R')
				break;
	}

#ifdef OPENMP
	#pragma omp parallel for
#endif
	for (readIndex = 0; readIndex < reads->readCount; readIndex++) {
		if (readIndex % 100000 == 0)
			velvetLog("Threading through reads %d / %d\n",
				  readIndex, reads->readCount);

#ifdef OPENMP
		#pragma omp critical 
#endif
		if (referenceMappings) 
			annotations = importAnnotations(file, &readIndex, &annotationCount);

		category = reads->categories[readIndex];
		second_in_pair = reads->categories[readIndex] % 2 && isSecondInPair(reads, readIndex);

		threadSequenceThroughGraph(getTightStringInArray(reads->tSequences, readIndex),
					   kmerTable,
					   graph, readIndex + 1, category,
					   readTracking, double_strand,
					   referenceMappings, referenceMappingCount,
					   refCount, annotations, annotationCount, second_in_pair);

		if (referenceMappings)
			free(annotations);
	}

	orderNodeReadStartArrays(graph);

	if (smallNodeListMemory != NULL)
		destroyRecycleBin(smallNodeListMemory);

	if (file) 
		fclose(file);
	destroyKmerOccurenceTable(kmerTable);
}

Graph *importPreGraph(char *preGraphFilename, ReadSet * reads, char * roadmapFilename, 
		      boolean readTracking, short int accelerationBits)
{
	boolean double_strand = false;
	Graph *graph = readPreGraphFile(preGraphFilename, &double_strand);
	Coordinate referenceMappingCount = 0;
	IDnum referenceCount = 0;

	if (nodeCount(graph) == 0)
		return graph;

	// If necessary compile reference -> node
	ReferenceMapping * referenceMappings = computeReferenceMappings(preGraphFilename, reads, &referenceMappingCount, &referenceCount); 
	// Node -> reference maps
	NodeMask * nodeMasks = computeNodeMasks(referenceMappings, referenceMappingCount, graph);

	// Map k-mers to nodes
	KmerOccurenceTable *kmerTable =
	    referenceGraphKmers(preGraphFilename, accelerationBits, graph, double_strand, nodeMasks, referenceMappingCount);

	free(nodeMasks);

	// Map sequences -> kmers -> nodes
	fillUpGraph(reads, kmerTable, graph, readTracking, double_strand, referenceMappings, referenceMappingCount, referenceCount, roadmapFilename);

	free(referenceMappings);

	return graph;
}
