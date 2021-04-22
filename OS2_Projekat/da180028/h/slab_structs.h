#pragma once

#define MIN_OBJECTS_PER_SLAB 8
#define MAX_NAME_LENGTH 32
#define MIN_DEG_SMALL 5
#define MAX_DEG_SMALL 17

#define ERRCODE_NO_SPACE -1
#define ERRCODE_INVALID_OBJECT -2
#define ERRCODE_CACHE_NOT_EMPTY -3
#define ERRCODE_INVALID_CACHE -4

typedef struct slab_alloc_metadata SlabAllocMetadata;

typedef struct slab_metadata SlabMetadata;