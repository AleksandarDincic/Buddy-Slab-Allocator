#include "buddy.h"
#include <stdio.h>

struct buddy_block {
	char block[BLOCK_SIZE];
};

struct buddy_metadata {
	BuddyBlock* freeChunks[MAX_BLOCK_DEG];
	BuddyBlock* startingAddress;
};

BuddyMetadata* buddy_init(void* space, int num_blocks) {
	
	BuddyMetadata* metadata = space;
	int blocksNeeded = sizeof(BuddyMetadata) / BLOCK_SIZE + (sizeof(BuddyMetadata)%BLOCK_SIZE == 0 ? 0 : 1);
	//printf("Blokova potrebno za metadata: %d\n", blocksNeeded);
	BuddyBlock* currentChunk = ((char*) space) + blocksNeeded * BLOCK_SIZE;
	num_blocks-= blocksNeeded;
	if (num_blocks < 0) return NULL; //nije dato dovoljno mesta

	for (int i = 0; i < MAX_BLOCK_DEG; metadata->freeChunks[i++] = 0);
	metadata->startingAddress = currentChunk;
	//printf("pocetna adresa: %d\n", currentChunk);
	int degNum = 0, mask = 1, tempNum = num_blocks;

	while (tempNum > 1) {
		++degNum;
		mask <<= 1;
		tempNum >>= 1;
	}

	for (int i = degNum, j = mask; i >= 0; --i, j>>=1){
		if (num_blocks & j){ 
			metadata->freeChunks[i] = currentChunk;
			BuddyBlock** nextChunk = currentChunk;
			*nextChunk = 0;
			currentChunk += j;
			//printf("dodat chunk velicine %d,degNum %d, adresa %d\n", j,i, metadata->freeChunks[i]);
		}
	}

	return metadata;
}


void buddy_print(BuddyMetadata* buddy) {
	printf("----BUDDY STANJE----\n");
	for (int i = 0; i < MAX_BLOCK_DEG; i++) {
		if (buddy->freeChunks[i] != 0) {
			printf("2 ^ %d : ", i);
			BuddyBlock* currBlock = buddy->freeChunks[i];
			int counter = 0;
			while (currBlock) {
				int index = currBlock - buddy->startingAddress;
				printf("%d, ", index);
				counter++;
				BuddyBlock** nextBlock = currBlock;
				currBlock = *nextBlock;
			}
			printf("= %d chunkova\n", counter);
		}
	}
	printf("--------\n");
}

void buddy_calc_chunk_size(size_t size, int* degReqAddr, int* degBlkAddr) {

	int blocksRequired = size / BLOCK_SIZE;
	if (size > blocksRequired * BLOCK_SIZE)
		++blocksRequired;
	//printf("Potrebno %d blokova, ", blocksRequired);

	int degRequired = 0, needsBigger = 0, degBlocks = 1;
	while (blocksRequired > 1) {
		degRequired++;
		degBlocks <<= 1;
		if (blocksRequired & 1)
			needsBigger++;
		blocksRequired >>= 1;
	}
	if (needsBigger > 0) { degRequired++; degBlocks <<= 1; }
	//printf("Potreban chunk stepena %d\n", degRequired);

	*degReqAddr = degRequired;
	*degBlkAddr = degBlocks;
}

void* buddy_take(BuddyMetadata* buddy, size_t size) {

	int degRequired, degBlocks;

	buddy_calc_chunk_size(size, &degRequired, &degBlocks);

	for (int i = degRequired; i < MAX_BLOCK_DEG; ++i, degBlocks <<= 1) {
		if (buddy->freeChunks[i] != 0) {
			BuddyBlock* retVal = buddy->freeChunks[i];
			BuddyBlock** nextChunk = retVal;
			buddy->freeChunks[i] = *nextChunk;
			//printf("uzeo chunk stepena %d\n", i);

			while (i > degRequired) {
				degBlocks >>= 1;
				--i;
				BuddyBlock* leftoverChunk = retVal + degBlocks;
				BuddyBlock** nextLeftover = leftoverChunk;
				*nextLeftover = buddy->freeChunks[i];
				buddy->freeChunks[i] = leftoverChunk;
				//printf("cepanje chunka na dva dela stepena %d\n", i);
			}
			//buddy_print(buddy);
			//int index = retVal - buddy->startingAddress;
			//printf("take %d, size %d\n", index, degBlocks);
			return retVal;
		}
	}
	//printf("nema vise mesta\n");
	return NULL;
}

void buddy_give(BuddyMetadata* buddy, void* block, size_t size) {
	int degRequired, degBlocks;

	buddy_calc_chunk_size(size, &degRequired, &degBlocks);

	BuddyBlock* blockPointer = block;
	int index = blockPointer - buddy->startingAddress;
	//printf("indeks pocetka chunka: %d\n", index);

	//printf("give %d, size %d\n", index, degBlocks);
	
	while(1){
		BuddyBlock* partnerPointer = (index % (degBlocks * 2) == 0) ? blockPointer + degBlocks : blockPointer - degBlocks;
		int partnerIndex = partnerPointer - buddy->startingAddress;
		//printf("indeks pocetka partnera: %d\n", partnerIndex);

		BuddyBlock* currBlock = buddy->freeChunks[degRequired], *prevBlock = 0;
		while (currBlock && currBlock != partnerPointer) {
			BuddyBlock** nextBlock = currBlock;
			prevBlock = currBlock;
			currBlock = *nextBlock;
		}

		if (currBlock) {
			BuddyBlock** nextBlock = currBlock;
			if (prevBlock) {
				BuddyBlock** prevNextBlock = prevBlock;
				*prevNextBlock = *nextBlock;
			}
			else buddy->freeChunks[degRequired] = *nextBlock;

			if (currBlock < blockPointer) {
				blockPointer = currBlock;
				index = partnerIndex;
				//printf("novi indeks pocetka chunka: %d\n", index);
			}
			//else printf("naso veceg\n");

			degRequired++;
			degBlocks <<= 1;
		}
		else {// printf("nista\n"); 
		break; }
	}

	BuddyBlock** nextBlock = blockPointer;
	*nextBlock = buddy->freeChunks[degRequired];
	buddy->freeChunks[degRequired] = blockPointer;

	//buddy_print(buddy);
	//putchar('\n');
}