/************************************************************************
 * File: flash_store.c
 *
 *  Copyright 2019 United States Government as represented by the
 *  Administrator of the National Aeronautics and Space Administration.
 *  All Other Rights Reserved.
 *
 *  This software was created at NASA's Goddard Space Flight Center.
 *  This software is governed by the NASA Open Source Agreement and may be
 *  used, distributed and modified only pursuant to the terms of that
 *  agreement.
 *
 * Maintainer(s):
 *  Joe-Paul Swinski, Code 582 NASA GSFC
 *
 *************************************************************************/

/******************************************************************************
 INCLUDES
 ******************************************************************************/

#include "bplib.h"
#include "bplib_os.h"
#include "bplib_store_flash.h"

/******************************************************************************
 DEFINES
 ******************************************************************************/

#define FLASH_OBJECT_SYNC                   0x425020464C415348LLU
#define FLASH_PAGE_USE_BYTES                (FLASH_MAX_PAGES_PER_BLOCK / 8)

/******************************************************************************
 MACROS
 ******************************************************************************/

#define FLASH_GET_SID(addr)                 ((((addr).block * FLASH_DRIVER.pages_per_block) + (addr).page) + 1)
#define FLASH_GET_BLOCK(sid)                (((sid)-1) / FLASH_DRIVER.pages_per_block)
#define FLASH_GET_PAGE(sid)                 (((sid)-1) % FLASH_DRIVER.pages_per_block)

/******************************************************************************
 TYPEDEFS
 ******************************************************************************/

typedef struct {
    uint64_t    sync;
    uint64_t    timestamp;
    bp_object_t object;
} flash_object_hdr_t;

typedef struct {
    bp_flash_index_t    next_block;
    bp_flash_index_t    prev_block;
    bp_flash_index_t    max_pages;
    uint8_t             page_use[FLASH_PAGE_USE_BYTES];
} flash_block_control_t;

typedef struct {
    bp_flash_index_t    out;
    bp_flash_index_t    in;
    int                 count;
} flash_block_list_t;

typedef struct {
    bool                in_use;
    bp_flash_attr_t     attributes;
    bp_flash_addr_t     write_addr;
    bp_flash_addr_t     read_addr;
    uint8_t*            write_stage; /* holding buffer to construct data object before write */
    uint8_t*            read_stage; /* lockable buffer that holds data object for read */
    bool                stage_locked;
    int                 object_count;
} flash_store_t;

/******************************************************************************
 LOCAL FILE DATA
 ******************************************************************************/

/* Constants - set only in initialization function */

static bp_flash_driver_t        FLASH_DRIVER;

/* Globals */

static flash_store_t            flash_stores[FLASH_MAX_STORES];
static int                      flash_device_lock;
static flash_block_list_t       flash_free_blocks;
static flash_block_list_t       flash_bad_blocks;
static flash_block_control_t*   flash_blocks;
static int                      flash_error_count;
static int                      flash_used_block_count;

/******************************************************************************
 LOCAL FUNCTIONS - BLOCK LEVEL
 ******************************************************************************/

/*--------------------------------------------------------------------------------------
 * flash_block_list_add -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE void flash_block_list_add (flash_block_list_t* list, bp_flash_index_t block)
{
    if(list->out == BP_FLASH_INVALID_INDEX)
    {
        /* First Block Successfully Added */
        list->out = block;
    }
    else
    {
        /* Link from Previous Block  */
        flash_blocks[list->in].next_block = block;
    }

    /* Add Block to List */
    flash_blocks[block].prev_block = list->in;
    list->in = block;
    list->count++;
}

/*--------------------------------------------------------------------------------------
 * flash_free_reclaim -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_free_reclaim (bp_flash_index_t block)
{
    /* Clear Block Control Entry */
    flash_blocks[block].next_block = BP_FLASH_INVALID_INDEX;
    flash_blocks[block].prev_block = BP_FLASH_INVALID_INDEX;
    flash_blocks[block].max_pages = FLASH_DRIVER.pages_per_block;
    memset(flash_blocks[block].page_use, 0xFF, sizeof(flash_blocks[block].page_use));

    /* Block No Longer In Use */
    flash_used_block_count--;

    /* Add to Free or Bad List */
    if(!FLASH_DRIVER.isbad(block))
    {
        flash_block_list_add(&flash_free_blocks, block);
        return BP_SUCCESS;
    }
    else
    {
        flash_block_list_add(&flash_bad_blocks, block);
        return BP_ERROR;
    }
}

/*--------------------------------------------------------------------------------------
 * flash_free_allocate -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_free_allocate (bp_flash_index_t* block)
{
    int status = BP_FAILEDSTORE;

    /* Erase Block */
    while(status != BP_SUCCESS && flash_free_blocks.out != BP_FLASH_INVALID_INDEX)
    {
        status = FLASH_DRIVER.erase(flash_free_blocks.out);
        if(status == BP_SUCCESS)
        {
            /* Return Block */
            *block = flash_free_blocks.out;
            flash_used_block_count++;
        }
        else
        {
            /* Failed to Erase - Add to Bad Block List */
            flash_error_count++;
            flash_block_list_add(&flash_bad_blocks, flash_free_blocks.out);
            bplog(status, "Failed to erase block %d when allocating it... adding as bad block\n", FLASH_DRIVER.phyblk(flash_free_blocks.out));
        }

        /* Move to Next Free Block */
        flash_free_blocks.out = flash_blocks[flash_free_blocks.out].next_block;
        flash_free_blocks.count--;
    }

    /* Log Error */
    if(status != BP_SUCCESS)
    {
        bplog(status, "No free blocks available\n");
    }

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * flash_data_write -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_data_write (bp_flash_addr_t* addr, uint8_t* data, int size)
{
    int status;
    int data_index = 0;
    int bytes_left = size;

    /* Check for Valid Address */
    if(addr->page >= flash_blocks[addr->block].max_pages || addr->block >= FLASH_DRIVER.num_blocks)
    {
        return bplog(BP_FAILEDSTORE, "Invalid address provided to write function: %d.%d\n", FLASH_DRIVER.phyblk(addr->block), addr->page);
    }

    /* Copy Into and Write Pages */
    while(bytes_left > 0)
    {
        /* Write Data into Page */
        int bytes_to_copy = bytes_left < FLASH_DRIVER.page_size ? bytes_left : FLASH_DRIVER.page_size;
        status = FLASH_DRIVER.write(*addr, &data[data_index], bytes_to_copy);
        if(status == BP_SUCCESS)
        {
            data_index += bytes_to_copy;
            bytes_left -= bytes_to_copy;
        }
        else /* Write Failed */
        {
            /* Log and Count Error */
            flash_error_count++;
            bplog(status, "Error encountered writing data to flash address: %d.%d\n", FLASH_DRIVER.phyblk(addr->block), addr->page);

            /* Set Maximum Number of Pages for Block */
            if(addr->page > 0)
            {
                flash_blocks[addr->block].max_pages = addr->page;
            }
            else
            {
                status = flash_free_reclaim(addr->block);
                if(status != BP_SUCCESS)
                {
                    bplog(BP_FAILEDSTORE, "Failed (%d) to reclaim block %d as a free block after write error\n", status, FLASH_DRIVER.phyblk(addr->block));
                }
            }

            /* Move to Next Block */
            bp_flash_index_t next_write_block;
            status = flash_free_allocate(&next_write_block);
            if(status == BP_SUCCESS)
            {
                /* Link in New Next Block */
                bp_flash_index_t prev_write_block = flash_blocks[addr->block].prev_block;
                if(prev_write_block != BP_FLASH_INVALID_INDEX) flash_blocks[prev_write_block].next_block = next_write_block;
                flash_blocks[next_write_block].prev_block = prev_write_block;

                /* Try Again with Next Block */
                addr->block = next_write_block;
                addr->page = 0;
                continue;
            }
            else
            {
                /* Cannot Proceed */
                return bplog(status, "Failed to write data to flash address: %d.%d\n", FLASH_DRIVER.phyblk(addr->block), addr->page);
            }
        }

        /* Increment Page
         *  - always increment page as data cannot start in the middle of a page
         *  - check for new block needing to be allocated  */
        addr->page++;
        if(addr->page == flash_blocks[addr->block].max_pages)
        {
            bp_flash_index_t next_write_block;
            status = flash_free_allocate(&next_write_block);
            if(status == BP_SUCCESS)
            {
                flash_blocks[addr->block].next_block = next_write_block;
                flash_blocks[next_write_block].prev_block = addr->block;
                addr->block = next_write_block;
                addr->page = 0;
            }
            else
            {
                return bplog(status, "Failed to retrieve next free block in middle of flash write at block: %ld\n", FLASH_DRIVER.phyblk(addr->block));
            }
        }
    }

    return BP_SUCCESS;
}

/*--------------------------------------------------------------------------------------
 * flash_data_read -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_data_read (bp_flash_addr_t* addr, uint8_t* data, int size)
{
    int data_index = 0;
    int bytes_left = size;

    /* Check for Valid Address */
    if(addr->page >= flash_blocks[addr->block].max_pages || addr->block >= FLASH_DRIVER.num_blocks)
    {
        return bplog(BP_FAILEDSTORE, "Invalid address provided to read function: %d.%d\n", FLASH_DRIVER.phyblk(addr->block), addr->page);
    }

    /* Copy Into and Write Pages */
    while(bytes_left > 0)
    {
        /* Read Data from Page */
        int bytes_to_copy = bytes_left < FLASH_DRIVER.page_size ? bytes_left : FLASH_DRIVER.page_size;
        int status = FLASH_DRIVER.read(*addr, &data[data_index], bytes_to_copy);
        if(status == BP_SUCCESS)
        {
            data_index += bytes_to_copy;
            bytes_left -= bytes_to_copy;
            addr->page++;
        }
        else
        {
            /* Read Failed */
            flash_error_count++;
            return bplog(status, "Failed to read data to flash address: %d.%d\n", FLASH_DRIVER.phyblk(addr->block), addr->page);
        }

        /* Check Need to go to Next Block */
        if(addr->page == flash_blocks[addr->block].max_pages)
        {
            bp_flash_index_t next_read_block = flash_blocks[addr->block].next_block;
            if(next_read_block == BP_FLASH_INVALID_INDEX)
            {
                return bplog(BP_FAILEDSTORE, "Failed to retrieve next block in middle of flash read at block: %ld\n", FLASH_DRIVER.phyblk(addr->block));
            }

            /* Goto Next Read Block */
            addr->block = next_read_block;
            addr->page = 0;
        }
    }

    return BP_SUCCESS;
}

/******************************************************************************
 LOCAL FUNCTIONS - OBJECT LEVEL
 ******************************************************************************/

/*--------------------------------------------------------------------------------------
 * flash_object_write -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_object_write (flash_store_t* fs, int handle, uint8_t* data1, int data1_size, uint8_t* data2, int data2_size)
{
    int status = BP_SUCCESS;

    /* Check if Room Available */
    uint64_t bytes_available = (uint64_t)flash_free_blocks.count * (uint64_t)FLASH_DRIVER.pages_per_block * (uint64_t)FLASH_DRIVER.page_size;
    int bytes_needed = sizeof(flash_object_hdr_t) + data1_size + data2_size;
    if(bytes_available >= (uint64_t)bytes_needed && fs->attributes.max_data_size >= bytes_needed)
    {
        /* Get Current Time */
        unsigned long sysnow = 0;
        bplib_os_systime(&sysnow);

        /* Calculate Object Information */
        unsigned long sid = FLASH_GET_SID(fs->write_addr);
        flash_object_hdr_t object_hdr = {
            .sync = FLASH_OBJECT_SYNC,
            .timestamp = sysnow,
            .object = {
                .handle = handle,
                .size = data1_size + data2_size,
                .sid = (bp_sid_t)sid
            }
        };

        /* Copy into Write Stage */
        memcpy(&fs->write_stage[0], &object_hdr, sizeof(object_hdr));
        if(data1) memcpy(&fs->write_stage[sizeof(object_hdr)], data1, data1_size);
        if(data2) memcpy(&fs->write_stage[sizeof(object_hdr) + data1_size], data2, data2_size);

        /* Write Data into Flash */
        status = flash_data_write(&fs->write_addr, fs->write_stage, bytes_needed);
    }
    else
    {
        status = bplog(BP_STOREFULL, "Insufficient room in flash storage, max: %llu, available: %llu, needed: %llu\n", (long long unsigned)fs->attributes.max_data_size, (long long unsigned)bytes_available, (long long unsigned)bytes_needed);
    }

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * flash_object_read -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_object_read (flash_store_t* fs, int handle, bp_flash_addr_t* addr, bp_object_t** object)
{
    int status;

    /* Check Stage Lock */
    if(fs->stage_locked == false)
    {
        flash_object_hdr_t* object_hdr = (flash_object_hdr_t*)fs->read_stage;

        /* Read Object Header from Flash */
        status = flash_data_read(addr, fs->read_stage, FLASH_DRIVER.page_size);
        if(status == BP_SUCCESS)
        {
            if( (object_hdr->object.size <= fs->attributes.max_data_size) &&
                (object_hdr->object.handle == handle) &&
                (object_hdr->sync == FLASH_OBJECT_SYNC) )
            {
                int bytes_read = FLASH_DRIVER.page_size - sizeof(flash_object_hdr_t);
                int remaining_bytes = object_hdr->object.size - bytes_read;
                if(remaining_bytes > 0)
                {
                    status = flash_data_read(addr, &fs->read_stage[FLASH_DRIVER.page_size], remaining_bytes);
                }
            }
            else
            {
                status = bplog(BP_FAILEDSTORE, "Object read from flash fails validation, size (%d, %d), handle (%d, %d), sync (%016X, %016X)\n",
                                                object_hdr->object.size, fs->attributes.max_data_size,
                                                object_hdr->object.handle, handle,
                                                object_hdr->sync, FLASH_OBJECT_SYNC);
            }
        }

        /* Object Successfully Dequeued */
        if(status == BP_SUCCESS)
        {
            /* Return Locked Object */
            *object = &object_hdr->object;
            fs->stage_locked = true;
        }
    }
    else
    {
        status = bplog(BP_FAILEDSTORE, "Object read cannot proceed when object stage is locked\n");
    }

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * flash_object_scan -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_object_scan (bp_flash_addr_t* addr)
{
    int status = BP_ERROR;

    /* Loop while you have a valid address and you have not found the next sync;
     *   this will exit with an error if the address goes invalid before a sync is found */
    while(addr->block != BP_FLASH_INVALID_INDEX && status != BP_SUCCESS)
    {
        /* Read Object Header from Flash */
        flash_object_hdr_t object_hdr;
        int read_status = flash_data_read(addr, (uint8_t*)&object_hdr, sizeof(object_hdr));
        if( (read_status == BP_SUCCESS) &&
            (object_hdr.sync == FLASH_OBJECT_SYNC) )
        {
            status = BP_SUCCESS;
        }
        else
        {
            /* Go To Next Page */
            addr->page++;
            if(addr->page == flash_blocks[addr->block].max_pages)
            {
                addr->block = flash_blocks[addr->block].next_block;
                addr->page = 0;
            }
        }
    }

    return status;
}

/*--------------------------------------------------------------------------------------
 * flash_object_delete -
 *-------------------------------------------------------------------------------------*/
BP_LOCAL_SCOPE int flash_object_delete (bp_sid_t sid)
{
    int status;

    /* Get Address from SID */
    bp_flash_addr_t addr = {FLASH_GET_BLOCK((unsigned long)sid), FLASH_GET_PAGE((unsigned long)sid)};
    if(addr.page >= flash_blocks[addr.block].max_pages || addr.block >= FLASH_DRIVER.num_blocks)
    {
        return bplog(BP_FAILEDSTORE, "Invalid address provided to delete function: %d.%d\n", FLASH_DRIVER.phyblk(addr.block), addr.page);
    }

    /* Retrieve Object Header */
    flash_object_hdr_t object_hdr;
    bp_flash_addr_t hdr_addr = addr;
    status = flash_data_read(&hdr_addr, (uint8_t*)&object_hdr, sizeof(object_hdr));
    if(status != BP_SUCCESS)
    {
        return bplog(status, "Unable to read object header at %d.%d in delete function\n", FLASH_DRIVER.phyblk(addr.block), addr.page);
    }
    else if(object_hdr.object.sid != sid)
    {
        return bplog(BP_FAILEDSTORE, "Attempting to delete object with invalid SID: %lu != %lu\n", (unsigned long)object_hdr.object.sid, (unsigned long)sid);
    }

    /* Setup Loop Parameters */
    bp_flash_index_t current_block = BP_FLASH_INVALID_INDEX;
    unsigned int current_block_free_pages = 0;
    int bytes_left = object_hdr.object.size;

    /* Delete Each Page of Data */
    while(bytes_left > 0)
    {
        /* Count Number of Free Pages in Current Block */
        if(current_block != addr.block)
        {
            current_block = addr.block;

            /* Iterate over Page Use Array and Count '0' Bits */
            unsigned int byte_index, bit_index;
            current_block_free_pages = 0;
            for(byte_index = 0; byte_index < (FLASH_DRIVER.pages_per_block / 8); byte_index++)
            {
                for(bit_index = 0; bit_index < 8; bit_index++)
                {
                    if((flash_blocks[current_block].page_use[byte_index] & (0x80 >> bit_index)) == 0x00)
                    {
                        current_block_free_pages++;
                    }
                }
            }
        }

        /* Mark Data on Page as Deleted */
        int byte_offset = addr.page / 8;
        int bit_offset = addr.page % 8;
        uint8_t bit_byte = 0x80 >> bit_offset;
        if(flash_blocks[addr.block].page_use[byte_offset] & bit_byte)
        {
            flash_blocks[addr.block].page_use[byte_offset] &= ~bit_byte;
            current_block_free_pages++;
        }

        /* Update Bytes Left and Address */
        int bytes_to_delete = bytes_left < FLASH_DRIVER.page_size ? bytes_left : FLASH_DRIVER.page_size;
        bytes_left -= bytes_to_delete;
        addr.page++;

        /* Check if Address Needs to go to Next Block */
        if(addr.page == flash_blocks[addr.block].max_pages)
        {
            bp_flash_index_t next_delete_block = flash_blocks[addr.block].next_block;
            if(next_delete_block == BP_FLASH_INVALID_INDEX)
            {
                return bplog(BP_FAILEDSTORE, "Failed to retrieve next block in middle of flash delete at block: %d\n", FLASH_DRIVER.phyblk(addr.block));
            }

            /* Goto Next Block to Delete */
            addr.block = next_delete_block;
            addr.page = 0;
        }

        /* Check if Block can be Erased */
        if(current_block_free_pages >= flash_blocks[current_block].max_pages)
        {
            /* Consistency Check */
            if(bytes_left != 0)
            {
                return bplog(BP_FAILEDSTORE, "Reclaiming block %d which contains undeleted data at page %d\n", FLASH_DRIVER.phyblk(current_block), addr.page);
            }

            /* Bridge Over Block */
            bp_flash_index_t prev_block = flash_blocks[current_block].prev_block;
            bp_flash_index_t next_block = flash_blocks[current_block].next_block;
            if(prev_block != BP_FLASH_INVALID_INDEX) flash_blocks[prev_block].next_block = next_block;
            if(next_block != BP_FLASH_INVALID_INDEX) flash_blocks[next_block].prev_block = prev_block;

            /* Reclaim Block as Free */
            status = flash_free_reclaim (current_block);
            if(status != BP_SUCCESS)
            {
                bplog(BP_FAILEDSTORE, "Failed (%d) to reclaim block %d as a free block\n", status, FLASH_DRIVER.phyblk(current_block));
            }
        }
    }

    /* Return Success */
    return BP_SUCCESS;
}

/******************************************************************************
 EXPORTED FUNCTIONS
 ******************************************************************************/

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_init -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_init (bp_flash_driver_t driver, int init_mode)
{
    /* Initialize Flash Stores to Zero */
    memset(flash_stores, 0, sizeof(flash_stores));

    /* Initialize Flash Driver */
    FLASH_DRIVER = driver;

    /* Initialize Flash Device Lock */
    flash_device_lock = bplib_os_createlock();
    if(flash_device_lock < 0)
    {
        return bplog(BP_FAILEDOS, "Failed (%d) to create flash device lock\n", flash_device_lock);
    }

    /* Initialize Free Blocks List */
    flash_free_blocks.out = BP_FLASH_INVALID_INDEX;
    flash_free_blocks.in = BP_FLASH_INVALID_INDEX;
    flash_free_blocks.count = 0;

    /* Initialize Bad Blocks List */
    flash_bad_blocks.out = BP_FLASH_INVALID_INDEX;
    flash_bad_blocks.in = BP_FLASH_INVALID_INDEX;
    flash_bad_blocks.count = 0;

    /* Allocate Memory for Block Control */
    flash_blocks = (flash_block_control_t*)malloc(sizeof(flash_block_control_t) * FLASH_DRIVER.num_blocks);
    if(flash_blocks == NULL)
    {
        bplib_os_destroylock(flash_device_lock);
        return bplog(BP_FAILEDMEM, "Unable to allocate memory for flash block control information\n");
    }

    /* Format Flash for Use */
    int reclaimed_blocks = 0;
    if(init_mode == BP_FLASH_INIT_FORMAT)
    {
        /* Build Free Block List */
        bp_flash_index_t block;
        for(block = 0; block < FLASH_DRIVER.num_blocks; block++)
        {
            if(flash_free_reclaim(block) == BP_SUCCESS)
            {
                reclaimed_blocks++;
            }
        }
    }
    else if(init_mode == BP_FLASH_INIT_RECOVER)
    {
        /* TODO Implement Recovery Logic */
    }

    /* Zero Counters */
    flash_error_count = 0;
    flash_used_block_count = 0;

    /* Return Number of Reclaimed Blocks */
    return reclaimed_blocks;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_stats -
 *-------------------------------------------------------------------------------------*/
void bplib_store_flash_stats (bp_flash_stats_t* stats, bool log_stats, bool reset_stats)
{
    /* Copy Stats */
    if(stats)
    {
        stats->num_free_blocks = flash_free_blocks.count;
        stats->num_used_blocks = flash_used_block_count;
        stats->num_bad_blocks = flash_bad_blocks.count;
        stats->error_count = flash_error_count;
    }

    /* Log Stats */
    if(log_stats)
    {
        bplog(BP_DEBUG, "Number of free blocks: %d\n", flash_free_blocks.count);
        bplog(BP_DEBUG, "Number of used blocks: %d\n", flash_used_block_count);
        bplog(BP_DEBUG, "Number of bad blocks: %d\n", flash_bad_blocks.count);
        bplog(BP_DEBUG, "Number of flash errors: %d\n", flash_error_count);

        int block = flash_bad_blocks.out;
        while(block != BP_FLASH_INVALID_INDEX)
        {
            bplog(BP_DEBUG, "Block <%d> bad\n", FLASH_DRIVER.phyblk(block));
            block = flash_blocks[block].next_block;
        }
    }

    /* Reset Stats */
    if(reset_stats)
    {
        flash_error_count = 0;
    }
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_create -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_create (void* parm)
{
    bp_flash_attr_t* attr = (bp_flash_attr_t*)parm;

    int s;
    for(s = 0; s < FLASH_MAX_STORES; s++)
    {
        if(flash_stores[s].in_use == false)
        {
            /* Initialize Attributes */
            if(attr)
            {
                /* Check User Provided Attributes */
                if(attr->max_data_size < FLASH_DRIVER.page_size)
                {
                   return bplog(BP_INVALID_HANDLE, "Invalid attributes - must supply sufficient sizes\n");
                }

                /* Copy User Provided Attributes */
                flash_stores[s].attributes = *attr;
            }
            else
            {
                /* Use Default Attributes */
                flash_stores[s].attributes.max_data_size = FLASH_DRIVER.page_size;
            }

            /* Account for Overhead */
            flash_stores[s].attributes.max_data_size += sizeof(flash_object_hdr_t);

            /* Initialize Read/Write Pointers */
            flash_stores[s].write_addr.block    = BP_FLASH_INVALID_INDEX;
            flash_stores[s].write_addr.page     = 0;
            flash_stores[s].read_addr.block     = BP_FLASH_INVALID_INDEX;
            flash_stores[s].read_addr.page      = 0;

            /* Allocate Stage */
            flash_stores[s].stage_locked = false;
            flash_stores[s].write_stage = (uint8_t*)malloc(flash_stores[s].attributes.max_data_size);
            flash_stores[s].read_stage = (uint8_t*)malloc(flash_stores[s].attributes.max_data_size);
            if((flash_stores[s].write_stage == NULL) ||
               (flash_stores[s].read_stage == NULL) )
            {
                if(flash_stores[s].write_stage) free(flash_stores[s].write_stage);
                if(flash_stores[s].read_stage) free(flash_stores[s].read_stage);
                return bplog(BP_INVALID_HANDLE, "Unable to allocate data stages\n");
            }

            /* Set Counts to Zero */
            flash_stores[s].object_count = 0;

            /* Set In Use (necessary to be able to destroy later) */
            flash_stores[s].in_use = true;

            /* Return Handle */
            return s;
        }
    }

    return BP_INVALID_HANDLE;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_destroy -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_destroy (int handle)
{
    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);

    free(flash_stores[handle].write_stage);
    free(flash_stores[handle].read_stage);
    flash_stores[handle].in_use = false;

    return BP_SUCCESS;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_enqueue -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_enqueue (int handle, void* data1, int data1_size, void* data2, int data2_size, int timeout)
{
    (void)timeout;

    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);

    flash_store_t* fs = (flash_store_t*)&flash_stores[handle];
    int status = BP_SUCCESS;

    bplib_os_lock(flash_device_lock);
    {
        /* Check if First Write Block Available */
        if(fs->write_addr.block == BP_FLASH_INVALID_INDEX)
        {
            status = flash_free_allocate(&fs->write_addr.block);
            if(status != BP_SUCCESS)
            {
                return bplog(BP_FAILEDSTORE, "Failed (%d) to allocate write block first time\n");
            }
        }

        /* Check if Read Address Needs to be Set */
        if(fs->read_addr.block == BP_FLASH_INVALID_INDEX)
        {
            fs->read_addr = fs->write_addr;
        }

        /* Write Object into Flash */
        status = flash_object_write(fs, handle, data1, data1_size, data2, data2_size);
        if(status == BP_SUCCESS)
        {
            fs->object_count++;
        }
    }
    bplib_os_unlock(flash_device_lock);

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_dequeue -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_dequeue (int handle, bp_object_t** object, int timeout)
{
    (void)timeout;

    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);
    assert(object);

    flash_store_t* fs = (flash_store_t*)&flash_stores[handle];
    int status = BP_SUCCESS;

    bplib_os_lock(flash_device_lock);
    {
        /* Check if Data Objects Available */
        if((fs->read_addr.block != fs->write_addr.block) || (fs->read_addr.page != fs->write_addr.page))
        {
            status = flash_object_read(fs, handle, &fs->read_addr, object);
            if(status != BP_SUCCESS)
            {
                /* Scan to Next Address
                 *  yet still return an error so that
                 *  bplib can report failure of store */
                flash_object_scan(&fs->read_addr);
            }
        }
        else
        {
            /* No Data to Return */
            status = BP_TIMEOUT;
        }
    }
    bplib_os_unlock(flash_device_lock);

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_retrieve -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_retrieve (int handle, bp_sid_t sid, bp_object_t** object, int timeout)
{
    (void)timeout;

    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);
    assert(object);

    flash_store_t* fs = (flash_store_t*)&flash_stores[handle];
    int status = BP_SUCCESS;

    bplib_os_lock(flash_device_lock);
    {
        bp_flash_addr_t page_addr = {FLASH_GET_BLOCK((unsigned long)sid), FLASH_GET_PAGE((unsigned long)sid)};
        status = flash_object_read(fs, handle, &page_addr, object);
    }
    bplib_os_unlock(flash_device_lock);

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_release -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_release (int handle, bp_sid_t sid)
{
    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);

    flash_store_t* fs = (flash_store_t*)&flash_stores[handle];

    /* Check SID Matches */
    flash_object_hdr_t* object_hdr = (flash_object_hdr_t*)fs->read_stage;
    if(object_hdr->object.sid != sid)
    {
        return bplog(BP_FAILEDSTORE, "Object being released does not have correct SID, requested: %lu, actual: %lu\n", (unsigned long)sid, (unsigned long)object_hdr->object.sid);
    }

    /* Unlock Stage */
    fs->stage_locked = false;

    /* Return Success */
    return BP_SUCCESS;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_relinquish -
 *
 *  there is no need to check the blocks being deleted against the read and write pointers
 *  because a block should only be deleted after it is dequeued and therefore no longer apart
 *  of the queue of blocks in storage
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_relinquish (int handle, bp_sid_t sid)
{
    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);

    flash_store_t* fs = (flash_store_t*)&flash_stores[handle];
    int status = BP_SUCCESS;

    bplib_os_lock(flash_device_lock);
    {
        /* Delete Pages Containing Object */
        status = flash_object_delete(sid);
        if(status == BP_SUCCESS)
        {
            fs->object_count--;
        }
    }
    bplib_os_unlock(flash_device_lock);

    /* Return Status */
    return status;
}

/*--------------------------------------------------------------------------------------
 * bplib_store_flash_getcount -
 *-------------------------------------------------------------------------------------*/
int bplib_store_flash_getcount (int handle)
{
    assert(handle >= 0 && handle < FLASH_MAX_STORES);
    assert(flash_stores[handle].in_use);

    flash_store_t* fs = (flash_store_t*)&flash_stores[handle];

    return fs->object_count;
}
