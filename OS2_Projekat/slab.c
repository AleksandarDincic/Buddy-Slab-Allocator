#include "slab_structs.h"
#include "buddy.h"
#include "slab.h"
#include <stdio.h>
#include <windows.h>

typedef struct slab_alloc_metadata {
	kmem_cache_t* cacheList;
	kmem_cache_t* smallBufferCaches[MAX_DEG_SMALL - MIN_DEG_SMALL + 1];
	HANDLE mutex;
	BuddyMetadata* buddy;
};

struct slab_metadata {
	int freeObjectsLeft;
	struct slab_metadata* nextSlab, *prevSlab;
	char* occupyBits, * startingAddress, * freeList;
};

struct kmem_cache_s {
	char name[MAX_NAME_LENGTH];
	kmem_cache_t* nextCache, * prevCache;
	SlabMetadata* fullSlabs, * partialSlabs, * emptySlabs;
	size_t objectSize, actualSize;
	int slabSizeInBlocks, occupyBytes, numberOfSlabs, objectsPerSlab, lastErrorCode, canShrink, deallocCount;
	int remainingSpace, cacheShifting, nextOffset;
	void(*ctor)(void*);
	void(*dtor)(void*);
	HANDLE mutex;
};

SlabAllocMetadata* slabAllocator = NULL;

void kmem_init(void* space, int block_num)
{
	if (space == NULL || block_num <= 0) return; //neispravni argumenti
	BuddyMetadata* buddy = buddy_init(space, block_num);
	if (buddy == NULL) return NULL; //nije dato dovoljno mesta

	SlabAllocMetadata* tempPointer = buddy_take(buddy, sizeof(SlabAllocMetadata));
	tempPointer->buddy = buddy;
	tempPointer->cacheList = NULL;
	for (int i = 0; i < MAX_DEG_SMALL - MIN_DEG_SMALL + 1; tempPointer->smallBufferCaches[i++] = NULL);
	tempPointer->mutex = CreateMutex(NULL, FALSE, NULL);
	slabAllocator = tempPointer;
}

void setCacheFields(kmem_cache_t* cache, size_t size, const char* name, void(*ctor)(void*), void(*dtor)(void*)) {
	strcpy_s(cache->name, MAX_NAME_LENGTH, name);
	cache->emptySlabs = cache->partialSlabs = cache->fullSlabs = NULL;
	cache->objectSize = size;
	cache->lastErrorCode = 0;
	cache->canShrink = 1;
	cache->ctor = ctor;
	cache->dtor = dtor;
	cache->numberOfSlabs = 0;
	cache->deallocCount = 0;
	cache->objectsPerSlab = MIN_OBJECTS_PER_SLAB;

	int blocksNeeded = 1;
	size_t actualSize = size >= sizeof(void*) ? size : sizeof(void*);
	cache->actualSize = actualSize;
	int occupyBytesNeeded = MIN_OBJECTS_PER_SLAB / 8 + (MIN_OBJECTS_PER_SLAB % 8 == 0 ? 0 : 1);

	int spaceNeeded = sizeof(SlabMetadata) + MIN_OBJECTS_PER_SLAB * actualSize + occupyBytesNeeded;
	//printf("minimalna velicina slaba: %d\n", spaceNeeded);
	while (blocksNeeded * BLOCK_SIZE < spaceNeeded)
		blocksNeeded <<= 1;
	cache->slabSizeInBlocks = blocksNeeded;
	//printf("potrebno blokova: %d\n", blocksNeeded);
	int remainingSpace = blocksNeeded * BLOCK_SIZE - spaceNeeded;

	cache->objectsPerSlab += remainingSpace / (actualSize * 8 + 1) * 8;
	occupyBytesNeeded += remainingSpace / (actualSize * 8 + 1);
	remainingSpace %= (actualSize * 8 + 1);

	while (remainingSpace >= (actualSize + (cache->objectsPerSlab % 8 == 0 ? 1 : 0))) {
		if (cache->objectsPerSlab % 8 == 0) {
			++occupyBytesNeeded, --remainingSpace;
		}
		remainingSpace -= actualSize;
		++(cache->objectsPerSlab);
	}

	//printf("broj objekata: %d\n", cache->objectsPerSlab);
	//printf("broj bajtova potrebnih za evidenciju slobode objekata: %d\n", occupyBytesNeeded);
	//printf("neiskoriscenog mesta: %d\n", remainingSpace);

	//printf("Velicina objekta: %d, velicina slaba u blokovima : %d\n", size, cache->slabSizeInBlocks);

	cache->remainingSpace = remainingSpace;
	cache->occupyBytes = occupyBytesNeeded;
	cache->cacheShifting = remainingSpace >= CACHE_L1_LINE_SIZE ? 1 : 0;
	cache->nextOffset = 0;
	cache->mutex = CreateMutex(NULL, FALSE, NULL);
}

kmem_cache_t* kmem_cache_create(const char* name, size_t size, void(*ctor)(void*), void(*dtor)(void*))
{

	if (slabAllocator == NULL || size == 0) return NULL; //neispravna velicina ili alokator nije inicijalizovan
	
	WaitForSingleObject(slabAllocator->mutex, INFINITE);
	//printf("kmem_cache_create (%s , %d)\n",name,size);
	kmem_cache_t* cache = buddy_take(slabAllocator->buddy, sizeof(kmem_cache_t));
	if (cache == NULL) {
		ReleaseMutex(slabAllocator->mutex);
		return NULL; //nema prostora
	}
	cache->prevCache = NULL;
	if (slabAllocator->cacheList != NULL)
		slabAllocator->cacheList->prevCache = cache;
	cache->nextCache = slabAllocator->cacheList;
	slabAllocator->cacheList = cache;
	
	ReleaseMutex(slabAllocator->mutex);

	setCacheFields(cache, size, name, ctor, dtor);
	
	//kmem_cache_info(cache);
	return cache;
}

int kmem_cache_shrink_trusted(kmem_cache_t* cachep) {
	if (WaitForSingleObject(cachep->mutex, INFINITE) != WAIT_OBJECT_0) return 0; //kes je obrisan u medjuvremenu
	if (!cachep->canShrink) {
		//printf("cant shrink \n");
		cachep->canShrink = 1;
		ReleaseMutex(cachep->mutex);
		return 0;
	}

	int blocksFreed = 0;
	SlabMetadata* currSlab = cachep->emptySlabs;
	while (currSlab != NULL) {
		WaitForSingleObject(slabAllocator->mutex, INFINITE);
		//printf("kmem_cache_shrink (%s)\n", cachep->name);
		buddy_give(slabAllocator->buddy, currSlab, cachep->slabSizeInBlocks * BLOCK_SIZE);
		ReleaseMutex(slabAllocator->mutex);

		blocksFreed += cachep->slabSizeInBlocks;
		--(cachep->numberOfSlabs);
		currSlab = currSlab->nextSlab;
	}
	cachep->emptySlabs = NULL;
	cachep->lastErrorCode = 0;

	//printf("can shrink, blokova %d\n", blocksFreed);
	ReleaseMutex(cachep->mutex);
}

int kmem_cache_shrink(kmem_cache_t* cachep)
{
	if (slabAllocator == NULL || cachep == NULL) return NULL; //neispravan argument ili alokator nije inicijalizovan

	if (!cacheExists(cachep)) return 0; //nevalidna adresa kesa

	return kmem_cache_shrink_trusted(cachep);
}

void setOccupyBit(kmem_cache_t* cachep, SlabMetadata* slab, char* obj, char bit) {
	unsigned distance = obj - slab->startingAddress;
	distance /= cachep->actualSize;
	int selectedByte = distance / 8;
	int selectedBit = distance % 8;
	unsigned char bitMask = 1;
	bitMask <<= selectedBit;
	if (bit)
		slab->occupyBits[selectedByte] |= bitMask;
	else
		slab->occupyBits[selectedByte] &= ~bitMask;
	//printf("setujem bit bajta %d bit %d vrednost %d\n",selectedByte,selectedBit, bit);
}

char getOccupyBit(kmem_cache_t* cachep, SlabMetadata* slab, char* obj) {
	unsigned distance = obj - slab->startingAddress;
	distance /= cachep->actualSize;
	int selectedByte = distance / 8;
	int selectedBit = distance % 8;
	unsigned char bitMask = 1;
	bitMask <<= selectedBit;
	//printf("getujem bit bajta %d bit %d vrednost %d\n", selectedByte, selectedBit, (slab->occupyBits[selectedByte] & bitMask) ? 1 : 0);
	return (slab->occupyBits[selectedByte] & bitMask) ? 1 : 0;
}

void* getFreeObject(kmem_cache_t* cachep, SlabMetadata* slab) {
	void* retVal = slab->freeList;
	char** nextObject = slab->freeList;
	slab->freeList = *nextObject;
	--(slab->freeObjectsLeft);
	setOccupyBit(cachep, slab, retVal, 1);
	return retVal;
}

SlabMetadata* createNewSlab(kmem_cache_t* cachep) {
	WaitForSingleObject(slabAllocator->mutex, INFINITE);

	//printf("createNewSlab (%s)\n",cachep->name);
	SlabMetadata* newSlab = buddy_take(slabAllocator->buddy, cachep->slabSizeInBlocks * BLOCK_SIZE);
	
	ReleaseMutex(slabAllocator->mutex);
	
	if(newSlab == NULL){
		return NULL;
	}
	
	newSlab->prevSlab = NULL;
	newSlab->freeObjectsLeft = cachep->objectsPerSlab;
	
	newSlab->occupyBits = newSlab;
	newSlab->occupyBits += sizeof(SlabMetadata);
	for (int i = 0; i < cachep->occupyBytes; newSlab->occupyBits[i++] = 0);
	
	newSlab->startingAddress = newSlab->occupyBits + cachep->occupyBytes;
	if (cachep->cacheShifting) {
		newSlab->startingAddress += cachep->nextOffset;
		cachep->nextOffset += CACHE_L1_LINE_SIZE;
		if (cachep->nextOffset > cachep->remainingSpace)
			cachep->nextOffset = 0;
	}
	
	newSlab->freeList = newSlab->startingAddress;
	char* currObj = newSlab->startingAddress;
	for (int i = 0; i < cachep->objectsPerSlab - 1; i++) {
		char** nextObj = currObj;
		*nextObj = currObj + cachep->actualSize;
		currObj += cachep->actualSize;
	}
	char** nextObj = currObj;
	*nextObj = NULL;

	return newSlab;
}

int cacheExists(kmem_cache_t* cachep) {
	WaitForSingleObject(slabAllocator->mutex, INFINITE);
	kmem_cache_t* currCache = slabAllocator->cacheList;
	while (currCache!= NULL) {
		if (currCache == cachep) {
			ReleaseMutex(slabAllocator->mutex);
			return 1;
		} 
		currCache = currCache->nextCache;
	}
	ReleaseMutex(slabAllocator->mutex);
	return 0;
}

void* kmem_cache_alloc_trusted(kmem_cache_t* cachep) {
	if (WaitForSingleObject(cachep->mutex, INFINITE) != WAIT_OBJECT_0) return NULL; //kes je obrisan u medjuvremenu
	SlabMetadata* selectedSlab = NULL;
	void* returnedObject = NULL;

	if (cachep->partialSlabs != NULL) {
		//printf("izabran parcijalan slab\n");
		selectedSlab = cachep->partialSlabs;
		returnedObject = getFreeObject(cachep, selectedSlab);
		if (!(selectedSlab->freeObjectsLeft)) {
			cachep->partialSlabs = cachep->partialSlabs->nextSlab;
			if (cachep->partialSlabs != NULL)
				cachep->partialSlabs->prevSlab = NULL;
			selectedSlab->nextSlab = cachep->fullSlabs;
			if (cachep->fullSlabs != NULL)
				cachep->fullSlabs->prevSlab = selectedSlab;
			cachep->fullSlabs = selectedSlab;
		}
	}
	else {
		if (cachep->emptySlabs != NULL) {
			//printf("izabran prazan slab\n");
			selectedSlab = cachep->emptySlabs;
			returnedObject = getFreeObject(cachep, selectedSlab);
			cachep->emptySlabs = cachep->emptySlabs->nextSlab;
			if (cachep->emptySlabs != NULL)
				cachep->emptySlabs->prevSlab = NULL;
		}
		else {
			//printf("izabran nov slab\n");
			selectedSlab = createNewSlab(cachep);
			if (selectedSlab == NULL) {
				cachep->lastErrorCode = ERRCODE_NO_SPACE;
				ReleaseMutex(cachep->mutex); //nema mesta za novi slab
				return NULL;
			}
			cachep->canShrink = 0;
			++(cachep->numberOfSlabs);
			returnedObject = getFreeObject(cachep, selectedSlab);

		}

		if (!(selectedSlab->freeObjectsLeft)) {
			selectedSlab->nextSlab = cachep->fullSlabs;
			if (cachep->fullSlabs != NULL)
				cachep->fullSlabs->prevSlab = selectedSlab;
			cachep->fullSlabs = selectedSlab;
		}

		else {
			selectedSlab->nextSlab = cachep->partialSlabs;
			if (cachep->partialSlabs != NULL)
				cachep->partialSlabs->prevSlab = selectedSlab;
			cachep->partialSlabs = selectedSlab;
		}
	}

	cachep->lastErrorCode = 0;

	ReleaseMutex(cachep->mutex);

	if (cachep->ctor != NULL)
		cachep->ctor(returnedObject);

	return returnedObject;
}

void* kmem_cache_alloc(kmem_cache_t* cachep)
{
	if (slabAllocator == NULL || cachep == NULL) return NULL; //neispravan argument ili alokator nije inicijalizovan
	
	if (!cacheExists(cachep)) return NULL; //nevalidna adresa kesa

	return kmem_cache_alloc_trusted(cachep);
}

int objectBelongsToSlab(kmem_cache_t* cachep, char* objp, SlabMetadata* slab) {
	char* endAddress = slab->startingAddress + cachep->actualSize * cachep->objectsPerSlab;
	if (objp < slab->startingAddress || objp >= endAddress) return 0;
	unsigned distance = objp - slab->startingAddress;
	
	return distance % cachep->actualSize == 0;
}

SlabMetadata* getSlabWithObject(kmem_cache_t* cachep, char* objp) {
	SlabMetadata* currSlab = cachep->partialSlabs;
	while (currSlab != NULL) {
		if (objectBelongsToSlab(cachep, objp, currSlab))
			return currSlab;
		currSlab = currSlab->nextSlab;
	}
	currSlab = cachep->fullSlabs;
	while (currSlab != NULL) {
		if (objectBelongsToSlab(cachep, objp, currSlab))
			return currSlab;
		currSlab = currSlab->nextSlab;
	}
	return NULL;
}

void freeOcupiedObject(kmem_cache_t* cachep, SlabMetadata* slab, char* objp) {
	char** nextObject = objp;
	*nextObject = slab->freeList;
	slab->freeList = objp;
	++(slab->freeObjectsLeft);
	setOccupyBit(cachep, slab, objp, 0);
}

int kmem_cache_free_trusted(kmem_cache_t* cachep, void* objp) {
	if (WaitForSingleObject(cachep->mutex, INFINITE) != WAIT_OBJECT_0) return -1; //kes je obrisan u medjuvremenu

	SlabMetadata* slabWithObject = getSlabWithObject(cachep, objp);
	if (slabWithObject == NULL || !getOccupyBit(cachep, slabWithObject, objp)) {
		//printf("ovaj objekat ne postoji\n");
		cachep->lastErrorCode = ERRCODE_INVALID_OBJECT;
		ReleaseMutex(cachep->mutex); //pokazivac ne pokazuje na zauzet objekat koji pripada kesu
		return -1;
	}

	if (cachep->dtor != NULL)
		cachep->dtor(objp);

	freeOcupiedObject(cachep, slabWithObject, objp);

	//printf("preostalo objekata: %d\n", slabWithObject->freeObjectsLeft);

	if (slabWithObject->freeObjectsLeft == cachep->objectsPerSlab || slabWithObject->freeObjectsLeft == cachep->objectsPerSlab - 1) {
		//printf("izacuje se slab iz svoje liste\n");
		if (slabWithObject->nextSlab != NULL)
			slabWithObject->nextSlab->prevSlab = slabWithObject->prevSlab;
		if (slabWithObject->prevSlab != NULL)
			slabWithObject->prevSlab->nextSlab = slabWithObject->nextSlab;
		else if (cachep->fullSlabs == slabWithObject)
			cachep->fullSlabs = slabWithObject->nextSlab;
		else
			cachep->partialSlabs = slabWithObject->nextSlab;

		slabWithObject->prevSlab = NULL;

		if (slabWithObject->freeObjectsLeft == cachep->objectsPerSlab) {
			//printf("slab presao u slobodne\n");
			slabWithObject->nextSlab = cachep->emptySlabs;
			if (cachep->emptySlabs != NULL)
				cachep->emptySlabs->prevSlab = slabWithObject;
			cachep->emptySlabs = slabWithObject;
		}
		else {
			//printf("slab presao u parcijalne\n");
			slabWithObject->nextSlab = cachep->partialSlabs;
			if (cachep->partialSlabs != NULL)
				cachep->partialSlabs->prevSlab = slabWithObject;
			cachep->partialSlabs = slabWithObject;
		}
	}

	cachep->lastErrorCode = 0;
	ReleaseMutex(cachep->mutex);
	return 0;
}

void kmem_cache_free(kmem_cache_t* cachep, void* objp)
{
	if (slabAllocator == NULL || cachep == NULL || objp == NULL) return; //neispravan argument ili alokator nije inicijalizovan

	if (!cacheExists(cachep)) return; //nevalidna adresa kesa

	kmem_cache_free_trusted(cachep, objp);
}

void* kmalloc(size_t size)
{
	if (slabAllocator == NULL || size == 0) return NULL; //neispravan argument ili alokator nije inicijalizovan

	//printf("kmalloc size %d\n", size);

	int index = 0, needsBigger = 0;
	while (size > 1) {
		if (size & 1)
			++needsBigger; //broj nije stepen dvojke
		++index;
		size >>= 1;
	}
	if (needsBigger > 0) ++index;
	size_t actualSize = 1 << index;

	//printf("kmalloc actual size %d\n", actualSize);

	//printf("indeks je %d\n", index);
	if (index < MIN_DEG_SMALL || index > MAX_DEG_SMALL) { 
		printf("oce to\n");
		return NULL; } //nije u dozvoljenom opsegu velicina

	index -= MIN_DEG_SMALL;
	//printf("tj indeks je %d\n", index);

	WaitForSingleObject(slabAllocator->mutex, INFINITE);
	if (slabAllocator->smallBufferCaches[index] == NULL) {
		//printf("kmalloc (%d)\n", index);
		kmem_cache_t* cache = buddy_take(slabAllocator->buddy, sizeof(kmem_cache_t));
		if (cache == NULL) {
			ReleaseMutex(slabAllocator->mutex);
			return NULL; //nema prostora
		}
		cache->prevCache = cache->nextCache = NULL;
		char name[MAX_NAME_LENGTH] = "size-", numBuf[MAX_NAME_LENGTH];
		_itoa_s(actualSize,numBuf,MAX_NAME_LENGTH,10);
		strcat_s(name, MAX_NAME_LENGTH,numBuf);
		setCacheFields(cache, actualSize, name, NULL, NULL);
		//printf("ime malog buffera: %s\n", name);
		//printf("VELICINA SLABA JE %d\n", cache->slabSizeInBlocks);
		slabAllocator->smallBufferCaches[index] = cache;
	}
	ReleaseMutex(slabAllocator->mutex);

	return kmem_cache_alloc_trusted(slabAllocator->smallBufferCaches[index]);

}

void kfree(const void* objp)
{
	if (slabAllocator == NULL || objp == NULL) return NULL; //neispravan argument ili alokator nije inicijalizovan

	WaitForSingleObject(slabAllocator->mutex, INFINITE);
	for (int i = 0; i < MAX_DEG_SMALL - MIN_DEG_SMALL + 1; i++) {
		if (slabAllocator->smallBufferCaches[i] != NULL) {
			ReleaseMutex(slabAllocator->mutex);
			if (!kmem_cache_free_trusted(slabAllocator->smallBufferCaches[i], objp)) {
				WaitForSingleObject(slabAllocator->smallBufferCaches[i]->mutex,INFINITE);
				if ((++(slabAllocator->smallBufferCaches[i]->deallocCount)) == slabAllocator->smallBufferCaches[i]->objectsPerSlab) {
					//printf("vreme je za brisanje\n");
					kmem_cache_shrink_trusted(slabAllocator->smallBufferCaches[i]);
					slabAllocator->smallBufferCaches[i]->deallocCount = 0;
				}
				ReleaseMutex(slabAllocator->smallBufferCaches[i]->mutex);
				return;
			}
			else WaitForSingleObject(slabAllocator->mutex, INFINITE);
		}
	}
	ReleaseMutex(slabAllocator->mutex);
}

void kmem_cache_destroy(kmem_cache_t* cachep)
{
	if (slabAllocator == NULL || cachep == NULL) return; //neispravan argument ili neinicijalizovan alokator
	
	if (!cacheExists(cachep)) return;	//nevalidna adresa kesa
	
	if (WaitForSingleObject(cachep->mutex, INFINITE) != WAIT_OBJECT_0) return; //kes je obrisan u medjuvremenu

	if (cachep->partialSlabs != NULL || cachep->fullSlabs != NULL) {
		//printf("Kes nije prazan\n");
		cachep->lastErrorCode = ERRCODE_CACHE_NOT_EMPTY;
		ReleaseMutex(cachep->mutex); //kes sadrzi objekte
		return;
	}

	WaitForSingleObject(slabAllocator->mutex, INFINITE);
	if (cachep->nextCache != NULL)
		cachep->nextCache->prevCache = cachep->prevCache;
	if (cachep->prevCache != NULL)
		cachep->prevCache->nextCache = cachep->nextCache;
	else
		slabAllocator->cacheList = cachep->nextCache;
	
	CloseHandle(cachep->mutex);

	SlabMetadata* currSlab = cachep->emptySlabs;
	while (currSlab != NULL) {
		//printf("kmem_cache_destroy (slab) (%s)\n", cachep->name);
		buddy_give(slabAllocator->buddy, currSlab, cachep->slabSizeInBlocks * BLOCK_SIZE);
		currSlab = currSlab->nextSlab;
	}
	//printf("kmem_cache_destroy (cache) (%s)\n", cachep->name);
	buddy_give(slabAllocator -> buddy, cachep, sizeof(kmem_cache_t));
	ReleaseMutex(slabAllocator->mutex);

}

void kmem_cache_info(kmem_cache_t* cachep)
{
	if (slabAllocator == NULL || cachep == NULL) { 
		printf("Neispravna adresa kesa, ili nije inicijalizovan alokator\n");
		return; 
	} //neispravan argument ili neinicijalizovan alokator
	
	if (!cacheExists(cachep)) {
		printf("Trazeni kes ne postoji!\n");
		return;
	}

	if (WaitForSingleObject(cachep->mutex, INFINITE) != WAIT_OBJECT_0) {
		printf("Trazeni kes ne postoji!\n");
		return;
	} //kes je obrisan u medjuvremenu

	int totalSlots = 0, usedSlots = 0;

	SlabMetadata* currSlab = cachep->emptySlabs;

	while (currSlab != NULL) {
		totalSlots += cachep->objectsPerSlab;
		currSlab = currSlab->nextSlab;
	}
	
	currSlab = cachep->partialSlabs;
	while (currSlab != NULL) {
		totalSlots += cachep->objectsPerSlab;
		usedSlots += cachep->objectsPerSlab - currSlab->freeObjectsLeft;
		currSlab = currSlab->nextSlab;
	}

	currSlab = cachep->fullSlabs;
	while (currSlab != NULL) {
		totalSlots += cachep->objectsPerSlab;
		usedSlots += cachep->objectsPerSlab;
		currSlab = currSlab->nextSlab;
	}
	
	printf("Ime: %s ; Velicina jednog podatka: %d ; Velicina kesa u blokovima: %d\n", cachep->name, cachep->objectSize, cachep->slabSizeInBlocks*cachep->numberOfSlabs + 1);
	printf("Broj ploca: %d ; Broj objekata po ploci: %d ; Popunjenost : %f%% (%d/%d)\n", cachep->numberOfSlabs, cachep->objectsPerSlab, (double) usedSlots / (totalSlots == 0 ? 1 : totalSlots) * 100 , usedSlots, totalSlots);
	ReleaseMutex(cachep->mutex);
}

int kmem_cache_error(kmem_cache_t* cachep)
{
	if (slabAllocator == NULL || cachep == NULL) return ERRCODE_INVALID_CACHE; //neispravan argument ili neinicijalizovan alokator
	
	if (!cacheExists(cachep)) return ERRCODE_INVALID_CACHE;

	if (WaitForSingleObject(cachep->mutex, INFINITE) != WAIT_OBJECT_0) return ERRCODE_INVALID_CACHE; //kes je obrisan u medjuvremenu
	
	int retVal = cachep->lastErrorCode;
	ReleaseMutex(cachep->mutex);
	return retVal;
}