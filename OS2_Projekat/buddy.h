#pragma once
#include "slab.h"

#define MAX_BLOCK_DEG 30
//sizeof(int) = 32b -> MAX_INT = 2^31 - 1, stoga je dovoljna velicina niza 30
//ako je int veci od 32b onda je problem...

typedef struct buddy_block BuddyBlock;

typedef struct buddy_metadata BuddyMetadata;


BuddyMetadata* buddy_init(void* space, int block_num);

void* buddy_take(BuddyMetadata* buddy, size_t size);

void buddy_give(BuddyMetadata* buddy, void* block, size_t size);

void buddy_print(BuddyMetadata* buddy);