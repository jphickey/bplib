/************************************************************************
 * File: ut_flash.c
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

#include "ut_assert.h"
#include "bplib_store_flash.h"
#include "bplib_flash_sim.h"

/******************************************************************************
 EXTERNAL PROTOTYPES
 ******************************************************************************/

extern int flash_free_reclaim (bp_flash_index_t block);
extern int flash_free_allocate (bp_flash_index_t* block);
extern int flash_data_write (bp_flash_addr_t* addr, uint8_t* data, int size);
extern int flash_data_read (bp_flash_addr_t* addr, uint8_t* data, int size);
extern int flash_data_delete (bp_flash_addr_t* addr, int size);

/******************************************************************************
 FILE DATA
 ******************************************************************************/

bp_flash_driver_t flash_driver = {
    .num_blocks = FLASH_SIM_NUM_BLOCKS,
    .pages_per_block = FLASH_SIM_PAGES_PER_BLOCK,
    .data_size = FLASH_SIM_DATA_SIZE,
    .read = bplib_flash_sim_page_read,
    .write = bplib_flash_sim_page_write,
    .erase = bplib_flash_sim_block_erase,
    .is_bad = bplib_flash_sim_block_is_bad,
    .mark_bad = bplib_flash_sim_block_mark_bad
};

/******************************************************************************
 LOCAL FUNCTIONS
 ******************************************************************************/


/******************************************************************************
 TEST FUNCTIONS
 ******************************************************************************/

/*--------------------------------------------------------------------------------------
 * Test #1
 *--------------------------------------------------------------------------------------*/
static void test_1(void)
{
    unsigned int i;
    bp_flash_index_t block;

    printf("\n==== Test 1: Free Block Management ====\n");

    int reclaimed_blocks = bplib_store_flash_init(flash_driver, BP_FLASH_INIT_FORMAT);
    printf("Number of Blocks Reclaimed: %d\n", reclaimed_blocks);

    printf("\n==== Step 1.1: Allocate All ====\n");
    for(i = 0; i < flash_driver.num_blocks; i++)
    {
        ut_assert(flash_free_allocate(&block) == BP_SUCCESS, "Failed to allocate block\n");
        ut_assert(block == i, "Failed to allocate block %d, allocated %d instead\n", i, block);        
    }

    printf("\n==== Step 1.2: Reclaim All In Reverse Order ====\n");
    for(i = 0; i < flash_driver.num_blocks; i++)
    {
        ut_assert(flash_free_reclaim(flash_driver.num_blocks - i - 1) == BP_SUCCESS, "Failed to reclaim block\n");
    }

    printf("\n==== Step 1.3: Re-Allocate All ====\n");
    for(i = 0; i < flash_driver.num_blocks; i++)
    {
        ut_assert(flash_free_allocate(&block) == BP_SUCCESS, "Failed to allocate block\n");
        ut_assert(block == flash_driver.num_blocks - i - 1, "Failed to allocate block %d, allocated %d instead\n", flash_driver.num_blocks - i - 1, block);        
    }

    printf("\n==== Step 1.4: Attempt Allocate On Empty List ====\n");
    ut_assert(flash_free_allocate(&block) == BP_FAILEDSTORE, "Incorrectly succeeded to allocate block when no blocks available\n");
}

/*--------------------------------------------------------------------------------------
 * Test #2
 *--------------------------------------------------------------------------------------*/
static void test_2(void)
{
    int i, j;
    int h[FLASH_MAX_STORES];

    printf("\n==== Test 2: Service Creation/Deletion ====\n");

    printf("\n==== Step 2.1: Create Max ====\n");
    for(i = 0; i < FLASH_MAX_STORES; i++)
    {
        h[i] = bplib_store_flash_create(NULL);
        ut_assert(h[i] != BP_INVALID_HANDLE, "Failed to create store on %dth iteration\n", i);
    }

    printf("\n==== Step 2.2: Check Full ====\n");
    j = bplib_store_flash_create(NULL);
    ut_assert(j == BP_INVALID_HANDLE, "Incorrectly created store when no more handles available\n");

    printf("\n==== Step 2.2: Clean Up Stores ====\n");
    for(i = 0; i < FLASH_MAX_STORES; i++)
    {
        ut_assert(bplib_store_flash_destroy(h[i]), "Failed to destroy handle %d\n", h[i]);
    }

    printf("\n==== Step 2.3: Check Holes ====\n");
    for(i = 0; i < FLASH_MAX_STORES; i++)
    {
        h[i] = bplib_store_flash_create(NULL);
        ut_assert(h[i] != BP_INVALID_HANDLE, "Failed to create store on %dth iteration\n", i);
    }
    ut_assert(bplib_store_flash_destroy(h[3]), "Failed to destroy handle %d\n", h[i]);
    h[3] = bplib_store_flash_create(NULL);
    ut_assert(h[3] != BP_INVALID_HANDLE, "Failed to create store\n", i);

    printf("\n==== Step 2.4: Clean Up Stores ====\n");
    for(i = 0; i < FLASH_MAX_STORES; i++)
    {
        ut_assert(bplib_store_flash_destroy(h[i]), "Failed to destroy handle %d\n", h[i]);
    }
}

/*--------------------------------------------------------------------------------------
 * Test #3
 *--------------------------------------------------------------------------------------*/
#define TEST_DATA_SIZE 50
//#define TEST_DATA_SIZE (FLASH_SIM_DATA_SIZE * 1.5)
static void test_3(void)
{
    int status, i;
    bp_flash_index_t saved_block;
    bp_flash_addr_t addr;
    static uint8_t test_data[TEST_DATA_SIZE], read_data[TEST_DATA_SIZE];

    printf("\n==== Test 3: Read/Write Data ====\n");

    /* Initialize Test Data */
    for(i = 0; i < TEST_DATA_SIZE; i++)
    {
        test_data[i] = i % 0xFF;
        read_data[i] = 0;
    }

    /* Write Test Data */
    ut_assert(flash_free_allocate(&addr.block) == BP_SUCCESS, "Failed to allocate free block\n");
    saved_block = addr.block;
    addr.page = 0;    
    status = flash_data_write (&addr, test_data, TEST_DATA_SIZE);
    ut_assert(status == BP_SUCCESS, "Failed to write data: %d\n", status);
    ut_assert(addr.page == 2, "Failed to increment page number: %d\n", addr.page);

    /* Read Test Data */
    addr.block = saved_block;
    addr.page = 0;
    status = flash_data_read (&addr, read_data, TEST_DATA_SIZE);
    ut_assert(status == BP_SUCCESS, "Failed to write data: %d\n", status);
    ut_assert(addr.page == 2, "Failed to increment page number: %d\n", addr.page);
    for(i = 0; i < TEST_DATA_SIZE; i++)
    {
        ut_assert(read_data[i] == test_data[i], "Failed to read correct data at %d, %02X != %02X\n", i, read_data[i], test_data[i]);
    }
}

/******************************************************************************
 EXPORTED FUNCTIONS
 ******************************************************************************/

int ut_flash (void)
{
    test_1();
    test_2();
    test_3();

    return ut_failures();
}
