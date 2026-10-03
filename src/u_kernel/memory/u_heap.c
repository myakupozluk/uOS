#include <u_kernel/memory/u_heap.h>

uint64_t slab_size_table[] = {
	4,
	8,
	16,
	32,
	64,
	128,
	128 + 64,
	256,
	256 + 64,
	256 + 128,
	512,
	512 + 128,
	512 + 256,
	1024,
	1024 + 512,
	2048,
	2048 + 128 + 32,
};

void UHP_init_heap_context(UHP_CONTEXT *ctx, uint8_t _memory_region)
{
	ctx->total_heap_size = 0;
	ctx->allocated_block_count = 0;
	ctx->block_list = nullptr;
	ctx->memory_region = _memory_region;
}

slab_block_header *create_slab(size_t slab_size, slab_block_header *parent, UHP_CONTEXT *ctx)
{
	uint64_t pageCount = UHP_SLAB_BLOCK_PAGE_COUNT;
	slab_block_header *slab = nullptr;

	if (slab_size >= UHP_MAX_SLAB_SIZE)
	{
		pageCount = (slab_size + sizeof(slab_block_header) + 16) / UHP_PAGE_SIZE;
		if ((slab_size + sizeof(slab_block_header) + 16) % UHP_PAGE_SIZE)
			pageCount++;
	}

	if (ctx->memory_region == MEMORY_REGION_MMR)
	{
		slab = ump_palloc(pageCount);
	}
	else if (ctx->memory_region == MEMORY_REGION_VMMR)
	{
		slab = ump_vpalloc(pageCount);
	}
	else
	{
		udbP("UHP WARNING! Unknown memory region for UHP! E0");
		return nullptr;
	}

	if (slab == nullptr)
	{
		udbP("UHP WARNING! Couldnt allocate slab! E1");
		return nullptr;
	}

	if (parent != nullptr)
	{
		parent->next = slab;
	}

	slab->page_count = pageCount;

	slab->slab_size = slab_size;
	if (slab_size < UHP_MAX_SLAB_SIZE)
	{
	    size_t usable_size = UHP_SLAB_BLOCK_SIZE - sizeof(slab_block_header);

	    size_t free_table_size = 0;
	    size_t slab_count = 0;

	    while (true)
	    {
	        slab_count =(usable_size - free_table_size) / slab_size;
	        size_t required_table_size = (slab_count + 7) / 8;
	        required_table_size = ALIGN_UP(required_table_size, 16);
	        if (required_table_size == free_table_size) break;
	        free_table_size = required_table_size;
	    }

	    slab->free_table_size = free_table_size;
	    slab->slab_count = slab_count;
	}
	else
	{
	    slab->free_table_size = 16;
	    slab->slab_count = 1;
	}
	slab->free_slab_count = slab->slab_count;

	slab->freeTable = (uint8_t *)slab + sizeof(slab_block_header);
	slab->data = slab->freeTable + slab->free_table_size;

	for (int i = 0; i < slab->free_table_size; i++)
	{
		slab->freeTable[i] = 0x00;
	}

	slab->next = nullptr;

	return slab;
}

void *UHP_malloc(size_t size, UHP_CONTEXT *ctx)
{
	if (ctx == nullptr){
		udbP("UHP ERROR: Empty context is not allowed!");
		return nullptr;
	}

	// find suitable size from table
	uint64_t suitable_slab_size = size;
	if (size <= UHP_MAX_SLAB_SIZE)
	{
		for (int i = 0; i < sizeof(slab_size_table) / sizeof(slab_size_table[0]); i++)
		{
			if (slab_size_table[i] >= size)
			{
				suitable_slab_size = slab_size_table[i];
				break;
			}
		}
	}

	if (ctx->block_list == nullptr)
	{
		ctx->block_list = create_slab(suitable_slab_size, nullptr, ctx);
		if (ctx->block_list == nullptr){
			udbP("UHP ERROR: Failed to allocate slab!");
			return nullptr;
		}
		ctx->total_heap_size += ctx->block_list->page_count;
		ctx->allocated_block_count++;
	}

	slab_block_header *last = nullptr;
	slab_block_header *current = ctx->block_list;

	while (current != nullptr)
	{
		if (current->slab_size == suitable_slab_size)
		{
			// find free slab
			for (int i = 0; i < current->free_table_size; i++)
			{
				if (current->free_slab_count == 0)
					break;

				if (current->freeTable[i] != 0xff)
				{
					uint8_t byte = current->freeTable[i];
    				int in_byte_offset = __builtin_ctz((unsigned int)((uint8_t)~byte));
    				uint64_t slab_index = ((uint64_t)i * 8) + (uint64_t)in_byte_offset;

					// check its a valid index
					if (slab_index >= current->slab_count)
					{
						break;
					}
					// set bit to 1 for allocating it
					current->freeTable[i] |= ONEBIT(in_byte_offset);

					// finish
					current->free_slab_count--;
					//udbPs();
					//udbP_STR("allocated slab addr: ");
					//udbP_HEX((current->data + (slab_index * current->slab_size)));
					//udbPe();
					return current->data + (slab_index * current->slab_size);
				}
				else
				{
					continue;
				}
			}

			last = current;
			current = current->next;
			continue;
		}
		else
		{
			last = current;
			current = current->next;
			continue;
		}
	}

	// no suitable block found
	// create a new block
	slab_block_header *newBlock = create_slab(suitable_slab_size, last, ctx);
	if (newBlock == nullptr){
		udbP("UHP ERROR: Failed to allocate slab!");
		return nullptr;
	}
	ctx->total_heap_size += newBlock->page_count;
	ctx->allocated_block_count++;

	newBlock->freeTable[0] = 0b1;
	newBlock->free_slab_count--;

	//udbPs();
	//udbP_STR("allocated slab addr: ");
	//udbP_HEX((newBlock->data));
	//udbPe();
	return newBlock->data;
}

void UHP_free(void *slab, UHP_CONTEXT *ctx)
{
    if (ctx == nullptr)
    {
        udbP("UHP ERROR! UHP_free called with null context!");
        return;
    }

    if (slab == nullptr)
    {
        udbP("UHP WARNING! Attempt to free nullptr!");
        return;
    }

    slab_block_header *last = nullptr;
    slab_block_header *current = ctx->block_list;

    uintptr_t slab_addr = (uintptr_t)slab;

    while (current != nullptr)
    {
        uintptr_t data_start = (uintptr_t)current->data;
        uintptr_t data_end = data_start + ((uintptr_t)current->slab_size * (uintptr_t)current->slab_count);

        if (slab_addr >= data_start && slab_addr < data_end){
            uintptr_t offset = slab_addr - data_start;

            if ((offset % current->slab_size) != 0)
            {
                udbP("UHP WARNING! Misaligned slab free attempt! E0");
                return;
            }

            uint64_t slab_index = offset / current->slab_size;

            if (slab_index >= current->slab_count)
            {
                udbP("UHP WARNING! Out of range slab free attempt! E1");
                return;
            }

            uint8_t mask =  ONEBIT(slab_index % 8);

            if ((current->freeTable[slab_index / 8] & mask) == 0)
            {
                udbP("UHP WARNING! Double slab free attempt! E2");
                return;
            }

            current->freeTable[slab_index / 8] &= ~mask;
            current->free_slab_count++;

            if (current->free_slab_count == current->slab_count)
            {
                slab_block_header *next = current->next;

                if (last == nullptr)
                {
                    ctx->block_list = next;
                }
                else
                {
                    last->next = next;
                }

                uint64_t page_count = current->page_count;

                if (ctx->memory_region == MEMORY_REGION_MMR)
                {
                    ump_pfree(current, page_count);
                }
                else if (ctx->memory_region == MEMORY_REGION_VMMR)
                {
                    ump_vpfree(current, page_count);
                }
                else
                {
                    udbP("UHP WARNING! Unknown memory region for UHP! E3");
                    return;
                }

                return;
            }

            return;
        }

        last = current;
        current = current->next;
    }

    udbP("UHP WARNING! Out of range slab free attempt! E4");

	/*
    udbPs();
    udbP_STR("free slab addr: ");
    udbP_HEX(slab);
    udbPe();

    udbPs();
    udbP_STR("free ctx: ");
    udbP_HEX(ctx);
    udbP_STR(" block_list: ");
    udbP_HEX(ctx->block_list);
    udbPe();
	*/
}

size_t UHP_query_free_slabs(UHP_CONTEXT *ctx)
{
	size_t result = 0;

	slab_block_header *last = nullptr;
	slab_block_header *current = ctx->block_list;

	while (current != nullptr)
	{
		result += current->free_slab_count * current->slab_size;
		last = current;
		current = current->next;
		continue;
	}

	return result;
}

size_t UHP_query_free_slabs_with_filter(UHP_CONTEXT *ctx, uint64_t searching_slab_size)
{
	size_t result = 0;

	slab_block_header *current = ctx->block_list;

	while (current != nullptr)
	{
		if (current->slab_size == searching_slab_size)
		{
			result += current->free_slab_count * current->slab_size;
		}
		current = current->next;
		continue;
	}

	return result;
}