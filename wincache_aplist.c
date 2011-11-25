/*
   +----------------------------------------------------------------------------------------------+
   | Windows Cache for PHP                                                                        |
   +----------------------------------------------------------------------------------------------+
   | Copyright (c) 2009, Microsoft Corporation. All rights reserved.                              |
   |                                                                                              |
   | Redistribution and use in source and binary forms, with or without modification, are         |
   | permitted provided that the following conditions are met:                                    |
   | - Redistributions of source code must retain the above copyright notice, this list of        |
   | conditions and the following disclaimer.                                                     |
   | - Redistributions in binary form must reproduce the above copyright notice, this list of     |
   | conditions and the following disclaimer in the documentation and/or other materials provided |
   | with the distribution.                                                                       |
   | - Neither the name of the Microsoft Corporation nor the names of its contributors may be     |
   | used to endorse or promote products derived from this software without specific prior written|
   | permission.                                                                                  |
   |                                                                                              |
   | THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS  |
   | OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF              |
   | MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE   |
   | COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,    |
   | EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE|
   | GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED   |
   | AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING    |
   | NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED |
   | OF THE POSSIBILITY OF SUCH DAMAGE.                                                           |
   +----------------------------------------------------------------------------------------------+
   | Module: wincache_aplist.c                                                                    |
   +----------------------------------------------------------------------------------------------+
   | Author: Kanwaljeet Singla <ksingla@microsoft.com>                                            |
   +----------------------------------------------------------------------------------------------+
*/

#include "precomp.h"

#define PER_RUN_SCAVENGE_COUNT        256
#define DO_PARTIAL_SCAVENGER_RUN      0
#define DO_FULL_SCAVENGER_RUN         1

#define SCAVENGER_STATUS_INVALID      255
#define SCAVENGER_STATUS_INACTIVE     0
#define SCAVENGER_STATUS_ACTIVE       1

#define DO_FILE_CHANGE_CHECK          1
#define NO_FILE_CHANGE_CHECK          0
#define FILE_IS_NOT_CHANGED           0
#define FILE_IS_CHANGED               1

#define APLIST_VALUE(p, o)            ((aplist_value *)alloc_get_cachevalue(p, o))

static int  find_aplist_entry(aplist_context * pcache, const char * filename, unsigned int index, unsigned char docheck, aplist_value ** ppvalue, aplist_value ** ppdelete);
static int  is_file_changed(aplist_context * pcache, aplist_value * pvalue);
static int  create_aplist_data(aplist_context * pcache, const char * filename, aplist_value ** ppvalue);
static void destroy_aplist_data(aplist_context * pcache, aplist_value * pvalue);
static void add_aplist_entry(aplist_context * pcache, unsigned int index, aplist_value * pvalue);
static void remove_aplist_entry(aplist_context * pcache, unsigned int index, aplist_value * pvalue);
static void delete_aplist_fileentry(aplist_context * pcache, const char * filename);
static void run_aplist_scavenger(aplist_context * pcache, unsigned char ffull);
static int  set_lastcheck_time(aplist_context * pcache, const char * filename, unsigned int newvalue TSRMLS_DC);

/* Globals */
unsigned short glcacheid = 1;

/* Private methods */

/* Call this method atleast under a read lock */
/* If an entry for filename with is_deleted is found, return that in ppdelete */
static int find_aplist_entry(aplist_context * pcache, const char * filename, unsigned int index, unsigned char docheck, aplist_value ** ppvalue, aplist_value ** ppdelete)
{
    int             result = NONFATAL;
    aplist_header * header = NULL;
    aplist_value *  pvalue = NULL;

    dprintverbose("start find_aplist_entry");

    _ASSERT(pcache   != NULL);
    _ASSERT(filename != NULL);
    _ASSERT(ppvalue  != NULL);

    *ppvalue  = NULL;
    if(ppdelete != NULL)
    {
        *ppdelete = NULL;
    }

    header = pcache->apheader;
    pvalue = APLIST_VALUE(pcache->apalloc, header->values[index]);

    while(pvalue != NULL)
    {
        if(!_stricmp(pcache->apmemaddr + pvalue->file_path, filename))
        {
            /* Ignore entries which are marked deleted */
            if(pvalue->is_deleted == 1)
            {
                /* If this process is good to delete the opcode */
                /* cache data tell caller to delete it */
                if(ppdelete != NULL && *ppdelete == NULL &&
                   pcache->apctype == APLIST_TYPE_GLOBAL && pcache->pocache != NULL && pcache->polocal == NULL)
                {
                    *ppdelete = pvalue;
                }

                pvalue = APLIST_VALUE(pcache->apalloc, pvalue->next_value);
                continue;
            }

            if(docheck)
            {
                /* If fcnotify is 0, use traditional file change check */
                if(pvalue->fcnotify == 0)
                {
                    if(is_file_changed(pcache, pvalue))
                    {
                        result = FATAL_FCACHE_FILECHANGED;
                    }
                }
                else
                {
                    if(pvalue->is_changed == 1)
                    {
                        result = FATAL_FCACHE_FILECHANGED;
                    }
                    else
                    {
                        /* If a listener is present, just check if listener is still active */
                        if(pcache->pnotify != NULL)
                        {
                            result = fcnotify_check(pcache->pnotify, NULL, &pvalue->fcnotify, &pvalue->fcncount);
                            if(FAILED(result))
                            {
                                /* Do a hard file change check if file listener is suggesting it */
                                if(result == WARNING_FCNOTIFY_FORCECHECK)
                                {
                                    result = NONFATAL;
                                    if(is_file_changed(pcache, pvalue))
                                    {
                                        result = FATAL_FCACHE_FILECHANGED;
                                    }
                                }
                                else
                                {
                                    goto Finished;
                                }
                            }
                        }
                    }
                }
            }

            break;
        }

        pvalue = APLIST_VALUE(pcache->apalloc, pvalue->next_value);
    }

    *ppvalue = pvalue;

Finished:

    dprintverbose("end find_aplist_entry");

    return result;
}

/* Call this method atleast under a read lock */
static int is_file_changed(aplist_context * pcache, aplist_value * pvalue)
{
    HRESULT                   hr        = S_OK;
    int                       retvalue  = 0;
    unsigned int              tickcount = 0;
    unsigned int              difftime  = 0;
    WIN32_FILE_ATTRIBUTE_DATA fileData;

    dprintverbose("start is_file_changed");

    _ASSERT(pcache != NULL);
    _ASSERT(pvalue != NULL);
    
    /* If fchangefreq is set to 0, dont do the file change check */
    /* last_check value of 0 means that a check must be done */
    if(pvalue->last_check != 0 && pcache->fchangefreq == 0)
    {
        goto Finished;
    }

    /* Calculate difftime while taking care of rollover */
    tickcount = GetTickCount();
    difftime = utils_ticksdiff(tickcount, pvalue->last_check);

    if(pvalue->last_check != 0 && difftime < pcache->fchangefreq)
    {
        goto Finished;
    }

    retvalue = 1;
    if (!GetFileAttributesEx(pcache->apmemaddr + pvalue->file_path, GetFileExInfoStandard, &fileData))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        dprintimportant("GetFileAttributesEx returned error %d", hr);

        if (hr == HRESULT_FROM_WIN32(ERROR_BAD_NET_NAME) ||
            hr == HRESULT_FROM_WIN32(ERROR_BAD_NETPATH) ||
            hr == HRESULT_FROM_WIN32(ERROR_NETNAME_DELETED) ||
            hr == HRESULT_FROM_WIN32(ERROR_REM_NOT_LIST) ||
            hr == HRESULT_FROM_WIN32(ERROR_SEM_TIMEOUT) ||
            hr == HRESULT_FROM_WIN32(ERROR_NETWORK_BUSY) ||
            hr == HRESULT_FROM_WIN32(ERROR_UNEXP_NET_ERR) ||
            hr == HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE))
        {
            retvalue = 0;
        }

        goto Finished;
    }

    _ASSERT(fileData.nFileSizeHigh == 0);

    /* If the attributes, WriteTime, and if it is a file, size are same */
    /* then the file has not changed. File sizes greater than 4GB won't */
    /* get added to cache. So comparing only nFileSizeLow is sufficient */
    if (fileData.dwFileAttributes == pvalue->attributes &&
        *(__int64 *)&fileData.ftLastWriteTime / 10000000 ==
         *(__int64 *)&pvalue->modified_time / 10000000 &&
        ((fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
         (fileData.nFileSizeLow == pvalue->file_size)))
    {
        retvalue = 0;
    }
    
    pvalue->last_check = tickcount;

Finished:

    dprintverbose("end is_file_changed");

    return retvalue;
}

/* Call this method without any lock */
static int create_aplist_data(aplist_context * pcache, const char * filename, aplist_value ** ppvalue)
{
    int                        result    = NONFATAL;
    HRESULT                    hr        = S_OK;
    unsigned int               ticks     = 0;
    char *                     filepath  = NULL;

    HANDLE                     hFile     = INVALID_HANDLE_VALUE;
    unsigned int               filesizel = 0;
    unsigned int               filesizeh = 0;
    unsigned int               openflags = 0;
    BY_HANDLE_FILE_INFORMATION finfo;
    aplist_value *             pvalue    = NULL;

    unsigned int               flength   = 0;
    unsigned int               alloclen  = 0;
    char *                     pbaseadr  = NULL;
    char *                     pcurrent  = NULL;

    dprintverbose("start create_aplist_data");

    _ASSERT(pcache   != NULL);
    _ASSERT(filename != NULL);
    _ASSERT(ppvalue  != NULL);

    *ppvalue = NULL;

    flength = strlen(filename);
    alloclen = sizeof(aplist_value) + ALIGNDWORD(flength + 1);

    /* Allocate memory for cache entry in shared memory */
    pbaseadr = (char *)alloc_smalloc(pcache->apalloc, alloclen);
    if(pbaseadr == NULL)
    {
        /* If scavenger is active, run it and try allocating again */
        if(pcache->scstatus == SCAVENGER_STATUS_ACTIVE)
        {
            lock_writelock(pcache->aprwlock);

            run_aplist_scavenger(pcache, DO_FULL_SCAVENGER_RUN);
            pcache->apheader->lscavenge = GetTickCount();

            lock_writeunlock(pcache->aprwlock);

            pbaseadr = (char *)alloc_smalloc(pcache->apalloc, alloclen);
            if(pbaseadr == NULL)
            {
                result = FATAL_OUT_OF_SMEMORY;
                goto Finished;
            }
        }
        else
        {
            result = FATAL_OUT_OF_SMEMORY;
            goto Finished;
        }
    }

    pcurrent = pbaseadr;

    pvalue = (aplist_value *)pcurrent;
    pvalue->file_path = 0;
    
    /* Open the file in shared read mode */
    openflags |= FILE_ATTRIBUTE_ENCRYPTED;
    openflags |= FILE_FLAG_OVERLAPPED;
    openflags |= FILE_FLAG_BACKUP_SEMANTICS; 
    openflags |= FILE_FLAG_SEQUENTIAL_SCAN;

    hFile = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, openflags, NULL);
    if(hFile == INVALID_HANDLE_VALUE)
    {
        error_setlasterror();
        result = FATAL_FCACHE_CREATEFILE;

        goto Finished;
    }

    /* Fail if file type is not of type_disk */
    if(GetFileType(hFile) != FILE_TYPE_DISK)
    {
        result = FATAL_FCACHE_GETFILETYPE;
        goto Finished;
    }

    filesizel = GetFileSize(hFile, &filesizeh);
    if(filesizel == INVALID_FILE_SIZE)
    {
        error_setlasterror();
        result = FATAL_FCACHE_GETFILESIZE;

        goto Finished;
    }

    /* File sizes greater than 4096MB not allowed */
    _ASSERT(filesizeh == 0);

    if(!GetFileInformationByHandle(hFile, &finfo))
    {
        error_setlasterror();
        result = FATAL_FCACHE_FILEINFO;

        goto Finished;
    }

    /* Point pcurrent to end of aplist_value */
    /* No goto Finished should exist after this */
    pcurrent += sizeof(aplist_value);

    /* Fill the details in aplist_value */
    /* memcpy_s error code ignored as buffer is of right size */
    memcpy_s(pcurrent, flength, filename, flength);
    *(pcurrent + flength) = 0;
    pvalue->file_path     = pcurrent - pcache->apmemaddr;

    pvalue->file_size     = filesizel;
    pvalue->modified_time = finfo.ftLastWriteTime;
    pvalue->attributes    = finfo.dwFileAttributes;
    
    ticks               = GetTickCount();
    pvalue->add_ticks   = ticks;
    pvalue->use_ticks   = ticks;
    pvalue->last_check  = ticks;
    pvalue->is_deleted  = 0;
    pvalue->is_changed  = 0;

    pvalue->fcacheval   = 0;
    pvalue->ocacheval   = 0;
    pvalue->resentry    = 0;
    pvalue->fcnotify    = 0;
    pvalue->fcncount    = 0;
    pvalue->prev_value  = 0;
    pvalue->next_value  = 0;

    /* Add file listener only if the file is not pointing to a UNC share */
    if(pcache->pnotify != NULL && IS_UNC_PATH(filename, flength) == 0)
    {
        result = fcnotify_check(pcache->pnotify, filename, &pvalue->fcnotify, &pvalue->fcncount);
        if(FAILED(result))
        {
            goto Finished;
        }
    }

    *ppvalue = pvalue;

    _ASSERT(SUCCEEDED(result));

Finished:

    if(hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }

    if(FAILED(result))
    {
        dprintimportant("failure %d in create_aplist_data", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        if(pbaseadr != NULL)
        {
            alloc_sfree(pcache->apalloc, pbaseadr);
            pbaseadr = NULL;
        }
    }

    dprintverbose("end create_aplist_data");

    return result;
}

/* Call this method under write lock if resentry can be non-zero so that */
/* rplist can never have an offset of aplist which is not valid */
static void destroy_aplist_data(aplist_context * pcache, aplist_value * pvalue)
{
    dprintverbose("start destroy_aplist_data");

    if(pvalue != NULL)
    {
        if(pcache->pnotify != NULL && pvalue->fcnotify != 0)
        {
            fcnotify_close(pcache->pnotify, &pvalue->fcnotify, &pvalue->fcncount);
        }

        /* Resolve path cache, file cache and ocache entries */
        /* should be deleted by a call to remove_aplist_entry */
        _ASSERT(pvalue->resentry  == 0);
        _ASSERT(pvalue->fcacheval == 0);
        _ASSERT(pvalue->ocacheval == 0);

        alloc_sfree(pcache->apalloc, pvalue);
        pvalue = NULL;
    }

    dprintverbose("end destroy_aplist_data");
}

/* Call this method under the write lock */
static void add_aplist_entry(aplist_context * pcache, unsigned int index, aplist_value * pvalue)
{
    aplist_header * header  = NULL;
    aplist_value *  pcheck  = NULL;

    dprintverbose("start add_aplist_entry");

    _ASSERT(pcache            != NULL);
    _ASSERT(pvalue            != NULL);
    _ASSERT(pvalue->file_path != 0);
    
    header = pcache->apheader;
    pcheck = APLIST_VALUE(pcache->apalloc, header->values[index]);

    /* New entry gets added in the end. This is required for is_deleted to work correctly */
    while(pcheck != NULL)
    {
        if(pcheck->next_value == 0)
        {
            break;
        }

        pcheck = APLIST_VALUE(pcache->apalloc, pcheck->next_value);
    }

    if(pcheck != NULL)
    {
        pcheck->next_value = alloc_get_valueoffset(pcache->apalloc, pvalue);
        pvalue->next_value = 0;
        pvalue->prev_value = alloc_get_valueoffset(pcache->apalloc, pcheck);
    }
    else
    {
        header->values[index] = alloc_get_valueoffset(pcache->apalloc, pvalue);
        pvalue->next_value = 0;
        pvalue->prev_value = 0;
    }

    header->itemcount++;

    dprintverbose("end add_aplist_entry");
    return;
}

/* Call this method under write lock */
static void remove_aplist_entry(aplist_context * pcache, unsigned int index, aplist_value * pvalue)
{
    alloc_context * apalloc = NULL;
    aplist_header * header  = NULL;
    aplist_value *  ptemp   = NULL;
    fcache_value *  pfvalue = NULL;
    ocache_value *  povalue = NULL;

    dprintverbose("start remove_aplist_entry");

    _ASSERT(pcache            != NULL);
    _ASSERT(pvalue            != NULL);
    _ASSERT(pvalue->file_path != 0);

    /* If entry from global cache is removed by a process which has */
    /* a local cache, mark the entry deleted but do not delete it */
    if(pcache->apctype == APLIST_TYPE_GLOBAL && pcache->pocache == NULL && pcache->polocal != NULL)
    {
        /* Mark resolve path entries deleted so that it rplist stop */
        /* handing pointer to aplist entries which are marked deleted */
        if(pvalue->resentry != 0)
        {
            _ASSERT(pcache->prplist != NULL);
            rplist_markdeleted(pcache->prplist, pvalue->resentry);
        }

        pvalue->is_deleted = 1;
        goto Finished;
    }

    /* Delete resolve path cache entries */
    if(pvalue->resentry != 0)
    {
        _ASSERT(pcache->prplist != NULL);

        rplist_deleteval(pcache->prplist, pvalue->resentry);
        pvalue->resentry = 0;
    }

    /* Delete file cache entry */
    if(pvalue->fcacheval != 0)
    {
        _ASSERT(pcache->pfcache != NULL);
        pfvalue = fcache_getvalue(pcache->pfcache, pvalue->fcacheval);

        InterlockedExchange(&pfvalue->is_deleted, 1);
        pvalue->fcacheval = 0;

        if(InterlockedCompareExchange(&pfvalue->refcount, 0, 0) == 0)
        {
            fcache_destroyval(pcache->pfcache, pfvalue);
            pfvalue = NULL;
        }
    }

    /* Delete opcode cache entry */
    if(pvalue->ocacheval != 0)
    {
        _ASSERT(pcache->pocache != NULL);
        povalue = ocache_getvalue(pcache->pocache, pvalue->ocacheval);
        
        InterlockedExchange(&povalue->is_deleted, 1);
        pvalue->ocacheval = 0;

        if(InterlockedCompareExchange(&povalue->refcount, 0, 0) == 0)
        {
            ocache_destroyval(pcache->pocache, povalue);
            povalue = NULL;
        }
    }

    apalloc = pcache->apalloc;
    header  = pcache->apheader;

    /* Decrement itemcount and remove entry from hashtable */
    header->itemcount--;
    if(pvalue->prev_value == 0)
    {
        header->values[index] = pvalue->next_value;
        if(pvalue->next_value != 0)
        {
            ptemp = APLIST_VALUE(apalloc, pvalue->next_value);
            ptemp->prev_value = 0;
        }
    }
    else
    {
        ptemp = APLIST_VALUE(apalloc, pvalue->prev_value);
        ptemp->next_value = pvalue->next_value;

        if(pvalue->next_value != 0)
        {
            ptemp = APLIST_VALUE(apalloc, pvalue->next_value);
            ptemp->prev_value = pvalue->prev_value;
        }
    }

    /* Destroy aplist data now that fcache and ocache is deleted */
    destroy_aplist_data(pcache, pvalue);
    pvalue = NULL;
    
Finished:

    dprintverbose("end remove_aplist_entry");
    return;
}

/* Call this method under write lock */
static void delete_aplist_fileentry(aplist_context * pcache, const char * filename)
{
    unsigned int   findex = 0;
    aplist_value * pvalue = NULL;

    dprintverbose("start delete_aplist_fileentry");

    /* This method should be called to remove file entry from a local cache */
    /* list when the file is removed the global list to keep them in sync */
    _ASSERT(pcache          != NULL);
    _ASSERT(pcache->islocal != 0);
    _ASSERT(filename        != NULL);
    _ASSERT(IS_ABSOLUTE_PATH(filename, strlen(filename)));

    findex = utils_getindex(filename, pcache->apheader->valuecount);
    find_aplist_entry(pcache, filename, findex, NO_FILE_CHANGE_CHECK, &pvalue, NULL);

    /* If an entry is found for this file, remove it */
    if(pvalue != NULL)
    {
        remove_aplist_entry(pcache, findex, pvalue);
        pvalue = NULL;
    }

    dprintverbose("end delete_aplist_fileentry");
    return;
}

/* Call this method under write lock */
static void run_aplist_scavenger(aplist_context * pcache, unsigned char ffull)
{
    unsigned int    sindex   = 0;
    unsigned int    eindex   = 0;

    alloc_context * apalloc  = NULL;
    aplist_header * apheader = NULL;
    aplist_value *  ptemp    = NULL;
    aplist_value *  pvalue   = NULL;
    unsigned int    ticks    = 0;

    dprintverbose("start run_aplist_scavenger");

    _ASSERT(pcache           != NULL);
    _ASSERT(pcache->scstatus == SCAVENGER_STATUS_ACTIVE);

    ticks    = GetTickCount();
    apalloc  = pcache->apalloc;
    apheader = pcache->apheader;

    if(ffull)
    {
        sindex = 0;
        eindex = apheader->valuecount;
        apheader->scstart = 0;
    }
    else
    {
        sindex = apheader->scstart;
        eindex = sindex + PER_RUN_SCAVENGE_COUNT;
        apheader->scstart = eindex;

        if(eindex >= apheader->valuecount)
        {
            eindex = apheader->valuecount;
            apheader->scstart = 0;
        }
    }

    dprintimportant("aplist scavenger sindex = %d, eindex = %d", sindex, eindex);
    for( ;sindex < eindex; sindex++)
    {
        if(apheader->values[sindex] == 0)
        {
            continue;
        }

        pvalue = APLIST_VALUE(apalloc, apheader->values[sindex]);
        while(pvalue != NULL)
        {
            ptemp = pvalue;
            pvalue = APLIST_VALUE(apalloc, pvalue->next_value);
            
            /* Remove entry if its marked deleted or is unsed for ttlmax time */
            if(ptemp->is_deleted || (utils_ticksdiff(ticks, ptemp->use_ticks) > apheader->ttlmax))
            {
                remove_aplist_entry(pcache, sindex, ptemp);
                ptemp = NULL;
            }
        }
    }

    dprintverbose("end run_aplist_scavenger");
    return;
}

/* Public functions */
int aplist_create(aplist_context ** ppcache)
{
    int              result = NONFATAL;
    aplist_context * pcache = NULL;

    dprintverbose("start aplist_create");

    _ASSERT(ppcache != NULL);
    *ppcache = NULL;

    pcache = (aplist_context *)alloc_pemalloc(sizeof(aplist_context));
    if(pcache == NULL)
    {
        result = FATAL_OUT_OF_LMEMORY;
        goto Finished;
    }

    pcache->id          = glcacheid++;
    pcache->islocal     = 0;
    pcache->apctype     = APLIST_TYPE_INVALID;
    pcache->scstatus    = SCAVENGER_STATUS_INVALID;
    pcache->hinitdone   = NULL;
    pcache->fchangefreq = 0;

    pcache->apmemaddr   = NULL;
    pcache->apheader    = NULL;
    pcache->apfilemap   = NULL;
    pcache->aprwlock    = NULL;
    pcache->apalloc     = NULL;

    pcache->polocal     = NULL;
    pcache->prplist     = NULL;
    pcache->pnotify     = NULL;
    pcache->pfcache     = NULL;
    pcache->pocache     = NULL;
    pcache->resnumber   = -1;

    *ppcache = pcache;

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_create", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end aplist_create");

    return result;
}

void aplist_destroy(aplist_context * pcache)
{
    dprintverbose("start aplist_destroy");

    if(pcache != NULL)
    {
        alloc_pefree(pcache);
        pcache = NULL;
    }

    dprintverbose("end aplist_destroy");

    return;
}

int aplist_initialize(aplist_context * pcache, unsigned short apctype, unsigned int filecount, unsigned int fchangefreq, unsigned int ttlmax TSRMLS_DC)
{
    int             result   = NONFATAL;
    size_t          mapsize  = 0;
    size_t          segsize  = 0;
    aplist_header * header   = NULL;
    unsigned int    msize    = 0;
    unsigned short  cachekey = 1;

    unsigned int    cticks   = 0;
    unsigned short  mapclass = FILEMAP_MAP_SRANDOM;
    unsigned short  locktype = LOCK_TYPE_SHARED;
    unsigned char   isfirst  = 1;
    char            evtname[   MAX_PATH];

    dprintverbose("start aplist_initialize");

    _ASSERT(pcache       != NULL);
    _ASSERT(apctype      == APLIST_TYPE_GLOBAL  || apctype     == APLIST_TYPE_OPCODE_LOCAL);
    _ASSERT(filecount    >= NUM_FILES_MINIMUM   && filecount   <= NUM_FILES_MAXIMUM);
    _ASSERT((fchangefreq >= FCHECK_FREQ_MINIMUM && fchangefreq <= FCHECK_FREQ_MAXIMUM) || fchangefreq == 0);
    _ASSERT((ttlmax      >= TTL_VALUE_MINIMUM   && ttlmax      <= TTL_VALUE_MAXIMUM)   || ttlmax == 0);

    /* If more APLIST_TYPE entries are added, add code to control value of islocal carefully */
    pcache->apctype = apctype;
    pcache->islocal = pcache->apctype;

    /* Disable scavenger if opcode cache is local only */
    /* Or if ttlmax value is set to 0 */
    pcache->scstatus = SCAVENGER_STATUS_ACTIVE;
    if(pcache->apctype == APLIST_TYPE_OPCODE_LOCAL || ttlmax == 0)
    {
        pcache->scstatus = SCAVENGER_STATUS_INACTIVE;
    }

    /* Initialize memory map to store file list */
    result = filemap_create(&pcache->apfilemap);
    if(FAILED(result))
    {
        goto Finished;
    }

    /* Desired cache size 2MB for 1024 entries. Expression below gives that */
    mapsize = ((filecount + 1023) * 2) / 1024;
    if(pcache->islocal)
    {
        mapclass = FILEMAP_MAP_LRANDOM;
        locktype = LOCK_TYPE_LOCAL;
    }

    /* shmfilepath = NULL to make it use page file */
    result = filemap_initialize(pcache->apfilemap, FILEMAP_TYPE_FILELIST, cachekey, mapclass, mapsize, NULL TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    pcache->apmemaddr = (char *)pcache->apfilemap->mapaddr;
    segsize = filemap_getsize(pcache->apfilemap TSRMLS_CC);

    /* Create allocator for file list segment */
    result = alloc_create(&pcache->apalloc);
    if(FAILED(result))
    {
        goto Finished;
    }

    /* initmemory = 1 for all page file backed shared memory allocators */
    result = alloc_initialize(pcache->apalloc, pcache->islocal, "FILELIST_SEGMENT", cachekey, pcache->apfilemap->mapaddr, segsize, 1 TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    /* Get memory for cache header */
    msize = sizeof(aplist_header) + ((filecount - 1) * sizeof(size_t));
    pcache->apheader = (aplist_header *)alloc_get_cacheheader(pcache->apalloc, msize, CACHE_TYPE_FILELIST);
    if(pcache->apheader == NULL)
    {
        result = FATAL_FCACHE_INITIALIZE;
        goto Finished;
    }

    header = pcache->apheader;

    /* Create reader writer locks for the aplist */
    result = lock_create(&pcache->aprwlock);
    if(FAILED(result))
    {
        goto Finished;
    }

    result = lock_initialize(pcache->aprwlock, "FILELIST_CACHE", cachekey, locktype, LOCK_USET_SREAD_XWRITE, &header->rdcount TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    /* Initialize both aplist_header and rplist_header */
    result = lock_getnewname(pcache->aprwlock, "APLIST_INIT", evtname, MAX_PATH);
    if(FAILED(result))
    {
        goto Finished;
    }

    /* Create resolve path cache for global aplist cache */
    if(apctype == APLIST_TYPE_GLOBAL)
    {
        result = rplist_create(&pcache->prplist);
        if(FAILED(result))
        {
            goto Finished;
        }

        result = rplist_initialize(pcache->prplist, pcache->islocal, cachekey, filecount TSRMLS_CC);
        if(FAILED(result))
        {
            goto Finished;
        }

        /* If file change notification is enabled, create fcnotify_context */
        if(WCG(fcndetect))
        {
            result = fcnotify_create(&pcache->pnotify);
            if(FAILED(result))
            {
                goto Finished;
            }

            /* Number of folders on which listeners will be active will */
            /* be very small. Using filecount as 32 so that scavenger is quick */
            result = fcnotify_initialize(pcache->pnotify, pcache->islocal, pcache, pcache->apalloc, 32 TSRMLS_CC);
            if(FAILED(result))
            {
                goto Finished;
            }
        }
    }

    /* Even with manual reset and initial state not set */
    pcache->hinitdone = CreateEvent(NULL, TRUE, FALSE, evtname);
    if(pcache->hinitdone == NULL)
    {
        result = FATAL_APLIST_INIT_EVENT;
        goto Finished;
    }

    if(GetLastError() == ERROR_ALREADY_EXISTS)
    {
        _ASSERT(pcache->islocal == 0);
        isfirst = 0;

        /* Wait for other process to initialize completely */
        WaitForSingleObject(pcache->hinitdone, INFINITE);
    }

    /* Initialize the aplist_header if this is the first process */
    if(pcache->islocal || isfirst)
    {
        /* No need to get a write lock as other processes */
        /* are blocked waiting for hinitdone event */

        /* Initialize resolve path cache header */
        if(pcache->prplist != NULL)
        {
            rplist_initheader(pcache->prplist, filecount);
        }

        if(pcache->pnotify != NULL)
        {
            fcnotify_initheader(pcache->pnotify, 32);
        }

        cticks = GetTickCount();

        /* We can set rdcount to 0 safely as other processes are */
        /* blocked and this process is right now not using lock */
        header->mapcount     = 1;
        header->init_ticks   = cticks;
        header->rdcount      = 0;
        header->itemcount    = 0;

        _ASSERT(filecount > PER_RUN_SCAVENGE_COUNT);
        
        /* Calculate scavenger frequency if ttlmax is not 0 */
        if(ttlmax != 0)
        {
            header->ttlmax       = ttlmax * 1000;
            header->scfreq       = header->ttlmax/(filecount/PER_RUN_SCAVENGE_COUNT);
            header->lscavenge    = cticks;
            header->scstart      = 0;

            _ASSERT(header->scfreq > 0);
        }

        header->valuecount   = filecount;
        memset((void *)header->values, 0, sizeof(size_t) * filecount);

        SetEvent(pcache->hinitdone);
    }
    else
    {
        /* Increment the mapcount */
        InterlockedIncrement(&header->mapcount);
    }

    /* Keep fchangefreq in aplist_context */
    pcache->fchangefreq = fchangefreq * 1000;

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_initialize", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        if(pcache->pnotify != NULL)
        {
            fcnotify_terminate(pcache->pnotify);
            fcnotify_destroy(pcache->pnotify);

            pcache->pnotify = NULL;
        }

        if(pcache->prplist != NULL)
        {
            rplist_terminate(pcache->prplist);
            rplist_destroy(pcache->prplist);

            pcache->prplist = NULL;
        }

        if(pcache->apfilemap != NULL)
        {
            filemap_terminate(pcache->apfilemap);
            filemap_destroy(pcache->apfilemap);

            pcache->apfilemap = NULL;
        }

        if(pcache->apalloc != NULL)
        {
            alloc_terminate(pcache->apalloc);
            alloc_destroy(pcache->apalloc);

            pcache->apalloc = NULL;
        }

        if(pcache->aprwlock != NULL)
        {
            lock_terminate(pcache->aprwlock);
            lock_destroy(pcache->aprwlock);

            pcache->aprwlock = NULL;
        }

        if(pcache->hinitdone != NULL)
        {
            CloseHandle(pcache->hinitdone);
            pcache->hinitdone = NULL;
        }

        pcache->apheader = NULL;
    }

    dprintverbose("end aplist_initialize");

    return result;
}

int aplist_fcache_initialize(aplist_context * plcache, unsigned int size, unsigned int maxfilesize TSRMLS_DC)
{
    int              result  = NONFATAL;
    fcache_context * pfcache = NULL;

    dprintverbose("start aplist_fcache_initialize");

    _ASSERT(plcache != NULL);

    result = fcache_create(&pfcache);
    if(FAILED(result))
    {
        goto Finished;
    }

    result = fcache_initialize(pfcache, plcache->islocal, 1, size, maxfilesize TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    plcache->pfcache = pfcache;

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_fcache_initialize", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        if(pfcache != NULL)
        {
            fcache_terminate(pfcache);
            fcache_destroy(pfcache);

            pfcache = NULL;
        }
    }

    dprintverbose("end aplist_fcache_initialize");

    return result;
}

int aplist_ocache_initialize(aplist_context * plcache, int resnumber, unsigned int size TSRMLS_DC)
{
    int              result  = NONFATAL;
    ocache_context * pocache = NULL;
    HANDLE           hfirst  = NULL;
    unsigned char    isfirst = 1;
    char             evtname[  MAX_PATH];

    unsigned int     count   = 0;
    aplist_value *   pvalue  = NULL;
    unsigned int     index   = 0;
    size_t           offset  = 0;
 
    dprintverbose("start aplist_ocache_initialize");

    _ASSERT(plcache != NULL);

    if(!plcache->islocal)
    {
        /* Create a new eventname using aplist lockname */
        result = lock_getnewname(plcache->aprwlock, "GLOBAL_OCACHE_FINIT", evtname, MAX_PATH);
        if(FAILED(result))
        {
            goto Finished;
        }

        /* First process reaching here will set all ocacheval to 0 if required */
        /* Even with manual reset and initial state not set */
        hfirst = CreateEvent(NULL, TRUE, FALSE, evtname);
        if(hfirst == NULL)
        {
            result = FATAL_APLIST_INIT_EVENT;
            goto Finished;
        }

        /* If this is not first process, wait for first process to be done with ocache init */
        if(GetLastError() == ERROR_ALREADY_EXISTS)
        {
            isfirst = 0;
            WaitForSingleObject(hfirst, INFINITE);
        }
    }

    result = ocache_create(&pocache);
    if(FAILED(result))
    {
        goto Finished;
    }

    result = ocache_initialize(pocache, plcache->islocal, 1, resnumber, size TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    if(!plcache->islocal && isfirst)
    {
        _ASSERT(hfirst != NULL);

        /* A new opcode cache is created for global aplist. It is possible that */
        /* global aplist is old with obsolete ocacheval offsets. Set ocachevals to 0 */
        lock_writelock(plcache->aprwlock);
       
        count = plcache->apheader->valuecount;
        for(index = 0; index < count; index++)
        {
            offset = plcache->apheader->values[index];
            if(offset == 0)
            {
                continue;
            }

            pvalue = (aplist_value *)alloc_get_cachevalue(plcache->apalloc, offset);
            while(pvalue != NULL)
            {
                pvalue->ocacheval = 0;

                offset = pvalue->next_value;
                if(offset == 0)
                {
                    break;
                }

                pvalue = (aplist_value *)alloc_get_cachevalue(plcache->apalloc, offset);
            }
        }

        lock_writeunlock(plcache->aprwlock);

        /* Set the event now that ocacheval is set to 0 */
        SetEvent(hfirst);
    }

    plcache->resnumber = resnumber;
    plcache->pocache   = pocache;

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_ocache_initialize", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        /* Make sure process with local cache doesn't increment */
        /* refcount of named object created above */
        if(hfirst != NULL)
        {
            CloseHandle(hfirst);
            hfirst = NULL;
        }

        if(pocache != NULL)
        {
            ocache_terminate(pocache);
            ocache_destroy(pocache);

            pocache = NULL;
        }
    }

    dprintverbose("end aplist_ocache_initialize");

    return result;
}

void aplist_terminate(aplist_context * pcache)
{
    dprintverbose("start aplist_terminate");

    if(pcache != NULL)
    {
        if(pcache->apheader != NULL)
        {
            InterlockedDecrement(&pcache->apheader->mapcount);
            pcache->apheader = NULL;
        }

        if(pcache->pnotify != NULL)
        {
            fcnotify_terminate(pcache->pnotify);
            fcnotify_destroy(pcache->pnotify);

            pcache->pnotify = NULL;
        }

        if(pcache->prplist != NULL)
        {
            rplist_terminate(pcache->prplist);
            rplist_destroy(pcache->prplist);

            pcache->prplist = NULL;
        }

        if(pcache->pfcache != NULL)
        {
            fcache_terminate(pcache->pfcache);
            fcache_destroy(pcache->pfcache);

            pcache->pfcache = NULL;
        }

        if(pcache->pocache != NULL)
        {
            ocache_terminate(pcache->pocache);
            ocache_destroy(pcache->pocache);

            pcache->pocache   = NULL;
            pcache->resnumber = -1;
        }

        if(pcache->apalloc != NULL)
        {
            alloc_terminate(pcache->apalloc);
            alloc_destroy(pcache->apalloc);

            pcache->apalloc = NULL;
        }

        if(pcache->apfilemap != NULL)
        {
            filemap_terminate(pcache->apfilemap);
            filemap_destroy(pcache->apfilemap);

            pcache->apfilemap = NULL;
        }

        if(pcache->aprwlock != NULL)
        {
            lock_terminate(pcache->aprwlock);
            lock_destroy(pcache->aprwlock);

            pcache->aprwlock = NULL;
        }

        if(pcache->hinitdone != NULL)
        {
            CloseHandle(pcache->hinitdone);
            pcache->hinitdone = NULL;
        }
    }

    dprintverbose("end aplist_terminate");

    return;
}

void aplist_setsc_olocal(aplist_context * pcache, aplist_context * plocal)
{
    _ASSERT(pcache != NULL);
    _ASSERT(plocal != NULL);

    pcache->scstatus = SCAVENGER_STATUS_INACTIVE;
    pcache->polocal  = plocal;

    return;
}

int aplist_getentry(aplist_context * pcache, const char * filename, unsigned int findex, aplist_value ** ppvalue)
{
    int              result   = NONFATAL;
    unsigned int     ticks    = 0;
    unsigned char    flock    = 0;
    unsigned char    fchange  = FILE_IS_NOT_CHANGED;
    unsigned int     index    = 0;
    unsigned int     addtick  = 0;

    aplist_value *   pvalue   = NULL;
    aplist_value *   pdelete  = NULL;
    aplist_value *   pdummy   = NULL;
    aplist_value *   pnewval  = NULL;
    aplist_header *  apheader = NULL;

    dprintverbose("start aplist_getentry");

    _ASSERT(pcache   != NULL);
    _ASSERT(filename != NULL);
    _ASSERT(ppvalue  != NULL);

    *ppvalue = NULL;

    apheader = pcache->apheader;
    ticks    = GetTickCount();

    /* Check if scavenger is active for this process and if yes run the partual scavenger */
    if(pcache->scstatus == SCAVENGER_STATUS_ACTIVE && (utils_ticksdiff(ticks, apheader->lscavenge) > apheader->scfreq))
    {
        /* run scavenger under write lock */
        lock_writelock(pcache->aprwlock);

        if(utils_ticksdiff(ticks, apheader->lscavenge) > apheader->scfreq)
        {
            run_aplist_scavenger(pcache, DO_PARTIAL_SCAVENGER_RUN);
            apheader->lscavenge = GetTickCount();
        }

        lock_writeunlock(pcache->aprwlock);
    }

    lock_readlock(pcache->aprwlock);

    /* Find file in hashtable and also get any entry for the file if mark deleted */
    result = find_aplist_entry(pcache, filename, findex, DO_FILE_CHANGE_CHECK, &pvalue, &pdelete);
    if(pvalue != NULL)
    {
        addtick = pvalue->add_ticks;
        if(SUCCEEDED(result))
        {
            pvalue->use_ticks = ticks;
        }
    }

    lock_readunlock(pcache->aprwlock);

    if(FAILED(result))
    {
        if(result != FATAL_FCACHE_FILECHANGED)
        {
            goto Finished;
        }

        fchange = FILE_IS_CHANGED;
    }

    /* If the entry was not found in cache */
    /* or if the cache entry is stale, create a new entry */
    if(fchange == FILE_IS_CHANGED || pvalue == NULL)
    {
        /* If we are about to create an entry in global aplist, */
        /* remove the cache entry in local cache if one is present */
        if(pcache->pocache == NULL && pcache->polocal != NULL)
        {
            lock_writelock(pcache->polocal->aprwlock);
            delete_aplist_fileentry(pcache->polocal, filename);
            lock_writeunlock(pcache->polocal->aprwlock);
        }

        result = create_aplist_data(pcache, filename, &pnewval);
        if(FAILED(result))
        {
            goto Finished;
        }
        
        lock_writelock(pcache->aprwlock);
        flock = 1;

        /* Check again after getting write lock to see if something changed */
        result = find_aplist_entry(pcache, filename, findex, NO_FILE_CHANGE_CHECK, &pvalue, &pdelete);
        if(FAILED(result))
        {
            goto Finished;
        }

        /* If entry still needs to be deleted, delete it now */
        if(pdelete != NULL)
        {
            remove_aplist_entry(pcache, findex, pdelete);
            pdelete = NULL;
        }

        if(pvalue != NULL)
        {
            if(pvalue->add_ticks == addtick)
            {
                /* Only possible if a file change was detected. Remove this */
                /* entry from the hashtable. If some other process already */
                /* deleted and added the entry add_ticks value won't match */
                _ASSERT(fchange == FILE_IS_CHANGED);

                remove_aplist_entry(pcache, findex, pvalue);
                pvalue = NULL;
            }
            else
            {
                /* Some other process created a new value for this file */
                /* Destroy cache data we created and use value from cache */
                destroy_aplist_data(pcache, pnewval);
                pnewval = NULL;
            }
        }

        if(pnewval != NULL)
        {
            pvalue = pnewval;
            pnewval = NULL;

            add_aplist_entry(pcache, findex, pvalue);
        }

        lock_writeunlock(pcache->aprwlock);
        flock = 0;
    }

    /* If an entry is to be deleted and is not already */
    /* deleted while creating new value, delete it now */
    if(pdelete != NULL)
    {
        lock_writelock(pcache->aprwlock);
        flock = 1;

        /* Check again to see if anything changed before getting write lock */
        result = find_aplist_entry(pcache, filename, findex, NO_FILE_CHANGE_CHECK, &pdummy, &pdelete);
        if(FAILED(result))
        {
            goto Finished;
        }

        /* If entry still needs to be deleted, delete it now */
        if(pdelete != NULL)
        {
            remove_aplist_entry(pcache, findex, pdelete);
            pdelete = NULL;
        }
        
        lock_writeunlock(pcache->aprwlock);
        flock = 0;
    }

    _ASSERT(pvalue != NULL);
    *ppvalue = pvalue;

    _ASSERT(SUCCEEDED(result));

Finished:

    if(flock)
    {
        lock_writeunlock(pcache->aprwlock);
        flock = 0;
    }

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_getentry", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        if(pnewval != NULL)
        {
            destroy_aplist_data(pcache, pnewval);
            pnewval = NULL;
        }
    }

    dprintverbose("end aplist_getentry");

    return result;
}

int aplist_force_fccheck(aplist_context * pcache, zval * filelist TSRMLS_DC)
{
    int              result    = NONFATAL;
    zval **          fileentry = NULL;
    aplist_value *   pvalue    = NULL;
    unsigned char    flock     = 0;

    unsigned int     count     = 0;
    unsigned int     index     = 0;
    size_t           offset    = 0;
    char *           execfile  = NULL;

    dprintverbose("start aplist_force_fccheck");

    _ASSERT(pcache   != NULL);
    _ASSERT(filelist == NULL || Z_TYPE_P(filelist) == IS_STRING || Z_TYPE_P(filelist) == IS_ARRAY);

    /* Get the write lock once instead of taking the lock for each file */
    lock_writelock(pcache->aprwlock);
    flock = 1;

    /* Always make currently executing file refresh on next load */
    if(filelist != NULL && zend_is_executing(TSRMLS_C))
    {
        execfile = zend_get_executed_filename(TSRMLS_C);

        result = set_lastcheck_time(pcache, execfile, 0 TSRMLS_CC);
        if(FAILED(result))
        {
            goto Finished;
        }
    }

    if(filelist == NULL)
    {
        /* Go through all the entries and set last_check to 0 */
        count = pcache->apheader->valuecount;
        for(index = 0; index < count; index++)
        {
            offset = pcache->apheader->values[index];
            if(offset == 0)
            {
                continue;
            }

            pvalue = (aplist_value *)alloc_get_cachevalue(pcache->apalloc, offset);
            while(pvalue != NULL)
            {
                _ASSERT(pvalue->file_path != 0);
                pvalue->last_check = 0;

                offset = pvalue->next_value;
                if(offset == 0)
                {
                    break;
                }

                pvalue = (aplist_value *)alloc_get_cachevalue(pcache->apalloc, offset);
            }
        }
    }
    else if(Z_TYPE_P(filelist) == IS_STRING)
    {
        if(Z_STRLEN_P(filelist) == 0)
        {
            goto Finished;
        }

        /* Set the last_check time of this file to 0 */
        result = set_lastcheck_time(pcache, Z_STRVAL_P(filelist), 0 TSRMLS_CC);
        if(FAILED(result))
        {
            goto Finished;
        }
    }
    else if(Z_TYPE_P(filelist) == IS_ARRAY)
    {
        zend_hash_internal_pointer_reset(Z_ARRVAL_P(filelist));
        while(zend_hash_get_current_data(Z_ARRVAL_P(filelist), (void **)&fileentry) == SUCCESS)
        {
            /* If array contains an entry which is not string, return false */
            if(Z_TYPE_PP(fileentry) != IS_STRING)
            {
                result = FATAL_INVALID_ARGUMENT;
                goto Finished;
            }

            if(Z_STRLEN_PP(fileentry) == 0)
            {
                continue;
            }

            result = set_lastcheck_time(pcache, Z_STRVAL_PP(fileentry), 0 TSRMLS_CC);
            if(FAILED(result))
            {
                goto Finished;
            }

            zend_hash_move_forward(Z_ARRVAL_P(filelist));
        }
    }

    _ASSERT(SUCCEEDED(result));

Finished:

    if(flock)
    {
        lock_writeunlock(pcache->aprwlock);
        flock = 0;
    }

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_force_fccheck", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end aplist_force_fccheck");

    return result;
}

/* This method is called by change listener thread which will run in parallel */
/* to main thread. Make sure functions used can be used in multithreaded environment */
void aplist_mark_changed(aplist_context * pcache, char * folderpath, char * filename)
{
    unsigned int   findex   = 0;
    aplist_value * pvalue   = NULL;
    char           filepath[  MAX_PATH];

    _ASSERT(pcache     != NULL);
    _ASSERT(folderpath != NULL);
    _ASSERT(filename   != NULL);

    dprintverbose("start aplist_mark_changed");

    ZeroMemory(filepath, MAX_PATH);
    _snprintf_s(filepath, MAX_PATH, MAX_PATH - 1, "%s\\%s", folderpath, filename);

    lock_readlock(pcache->aprwlock);

    /* Find the entry for filepath and mark it deleted */
    findex = utils_getindex(filepath, pcache->apheader->valuecount);
    find_aplist_entry(pcache, filepath, findex, NO_FILE_CHANGE_CHECK, &pvalue, NULL);

    if(pvalue != NULL)
    {
        pvalue->is_changed = 1;
    }

    lock_readunlock(pcache->aprwlock);

    dprintverbose("end aplist_mark_changed");

    return;
}

static int set_lastcheck_time(aplist_context * pcache, const char * filename, unsigned int newvalue TSRMLS_DC)
{
    int           result       = NONFATAL;
    char *        resolve_path = NULL;
    unsigned int  findex       = 0;
    aplist_value * pvalue      = NULL;

    dprintverbose("start set_lastcheck_time");

    _ASSERT(pcache   != NULL);
    _ASSERT(filename != NULL);

    /* Ok to call aplist_fcache_get with lock acquired when last param is NULL */
    /* If file is not accessible or not present, we will throw an error */
    result = aplist_fcache_get(pcache, filename, SKIP_STREAM_OPEN_CHECK, &resolve_path, NULL TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }
    
    findex = utils_getindex(resolve_path, pcache->apheader->valuecount);

    /* failure to find the entry in cache should be ignored */
    find_aplist_entry(pcache, resolve_path, findex, NO_FILE_CHANGE_CHECK, &pvalue, NULL);
    if(pvalue != NULL)
    {
        pvalue->last_check = 0;
    }

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in set_lastcheck_time", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end set_lastcheck_time");

    return result;
}

/* Used by wincache_resolve_path and wincache_stream_open_function */
/* If ppvalue is passed as null, this function return the standardized form of */
/* filename which can include resolve path to absolute path mapping as well */
/* Make sure this function is called without write lock when ppvalue is non-null */
int aplist_fcache_get(aplist_context * pcache, const char * filename, unsigned char usesopen, char ** ppfullpath, fcache_value ** ppvalue TSRMLS_DC)
{
    int              result   = NONFATAL;
    unsigned int     length   = 0;
    unsigned int     findex   = 0;
    unsigned int     addticks = 0;
    unsigned char    fchanged = 0;
    unsigned char    flock    = 0;

    aplist_value *   pvalue   = NULL;
    fcache_value *   pfvalue  = NULL;
    rplist_value *   rpvalue  = NULL;
    size_t           resentry = 0;
    char *           fullpath = NULL;
    zend_file_handle fhandle  = {0};

    dprintverbose("start aplist_fcache_get");

    _ASSERT(pcache     != NULL);
    _ASSERT(filename   != NULL);
    _ASSERT(ppfullpath != NULL);

    _ASSERT(pcache->prplist != NULL);

    /* Look for absolute path in resolve path cache first */
    /* All paths in resolve path cache are resolved using */
    /* include_path and don't need checks against open_basedir */
    lock_readlock(pcache->aprwlock);
    flock = 1;

    result = rplist_getentry(pcache->prplist, filename, &rpvalue, &resentry TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    _ASSERT(rpvalue  != NULL);
    _ASSERT(resentry != 0);

    /* If found, use new path to look into absolute path cache */
    if(rpvalue->absentry != 0)
    {
        pvalue = APLIST_VALUE(pcache->apalloc, rpvalue->absentry);

        /* If pvalue came directly from resolve path absentry, and if the */
        /* call is not just to get the fullpath, do a file change check here */
        if(pvalue != NULL && ppvalue != NULL)
        {
            if(pvalue->fcnotify == 0)
            {
                if(is_file_changed(pcache, pvalue))
                {
                    fchanged = 1;
                }
            }
            else
            {
                if(pvalue->is_changed == 1)
                {
                    fchanged = 1;
                }
                else
                {
                    /* If a listener is present, just check if listener is still active */
                    if(pcache->pnotify != NULL)
                    {
                        result = fcnotify_check(pcache->pnotify, NULL, &pvalue->fcnotify, &pvalue->fcncount);
                        if(FAILED(result))
                        {
                            /* Do a hard file change check if listener suggested it */
                            if(result == WARNING_FCNOTIFY_FORCECHECK)
                            {
                                result = NONFATAL;
                                if(is_file_changed(pcache, pvalue))
                                {
                                    fchanged = 1;
                                }
                            }
                            else
                            {
                                goto Finished;
                            }
                        }
                    }
                }
            }

            if(fchanged)
            {
                addticks = pvalue->add_ticks;

                lock_readunlock(pcache->aprwlock);
                lock_writelock(pcache->aprwlock);
                flock = 2;

                /* If the entry is unchanged during lock change, remove it from hashtable */
                /* Deleting the aplist entry will delete rplist entries as well */
                if(pvalue->add_ticks == addticks)
                {
                    findex = utils_getindex(pcache->apmemaddr + pvalue->file_path, pcache->apheader->valuecount);
                    remove_aplist_entry(pcache, findex, pvalue);
                }

                /* These values should not be used as they belong to changed file */
                pvalue   = NULL;
                rpvalue  = NULL;
                resentry = 0;

                lock_writeunlock(pcache->aprwlock);
                flock = 0;
            }
        }

        if(pvalue != NULL)
        {
            fullpath = alloc_estrdup(pcache->apmemaddr + pvalue->file_path);    
            if(fullpath == NULL)
            {    
                result = FATAL_OUT_OF_LMEMORY;
                goto Finished;
            }

            dprintimportant("stored fullpath in aplist is %s", fullpath);
        }
    }

    if(flock == 1)
    {
        lock_readunlock(pcache->aprwlock);
        flock = 0;
    }

    _ASSERT(flock == 0);

    /* If no valid absentry is found so far, get the fullpath from php-core */
    if(pvalue == NULL)
    {
        /* Get fullpath by using copy of php_resolve_path */
        fullpath = utils_resolve_path(filename, strlen(filename), PG(include_path) TSRMLS_CC);
        if(fullpath == NULL)
        {
            result = WARNING_ORESOLVE_FAILURE;
            goto Finished;
        }

        /* Convert to lower case and change / to \\ */
        length = strlen(fullpath);
        for(findex = 0; findex < length; findex++)
        {
            if(fullpath[findex] == '/')
            {
                fullpath[findex] = '\\';
            }
        }
    }

    /* If ppvalue is NULL, just set the fullpath and return */
    /* Resolve path cache entry won't have a corresponding absentry offset */
    if(ppvalue == NULL)
    {
        *ppfullpath = fullpath;
        goto Finished;
    }

    if(pvalue == NULL)
    {
        findex = utils_getindex(fullpath, pcache->apheader->valuecount);

        result = aplist_getentry(pcache, fullpath, findex, &pvalue);
        if(FAILED(result))
        {
            goto Finished;
        }
    }

    lock_readlock(pcache->aprwlock);
    flock = 1;

    if(pvalue->fcacheval == 0)
    {
        lock_readunlock(pcache->aprwlock);
        flock = 0;

        if(usesopen == USE_STREAM_OPEN_CHECK)
        {
            if(rpvalue == NULL || rpvalue->is_verified == VERIFICATION_NOTDONE)
            {
                /* Do a check for include_path, open_basedir validity */
                /* by calling original stream open function */
                result = original_stream_open_function(fullpath, &fhandle TSRMLS_CC);

                /* Set is_verified status in rpvalue */
                if(rpvalue != NULL)
                {
                    InterlockedExchange(&rpvalue->is_verified, ((result == SUCCESS) ? VERIFICATION_PASSED : VERIFICATION_FAILED));
                }

                if(result != SUCCESS)
                {
                    result = FATAL_FCACHE_ORIGINAL_OPEN;
                    goto Finished;
                }

                if(fhandle.handle.stream.closer && fhandle.handle.stream.handle)
                {
                    fhandle.handle.stream.closer(fhandle.handle.stream.handle TSRMLS_CC);
                    fhandle.handle.stream.handle = NULL;
                }

                if(fhandle.opened_path)
                {
                    efree(fhandle.opened_path);
                    fhandle.opened_path = NULL;
                }

                if(fhandle.free_filename && fhandle.filename)
                {
                    efree(fhandle.filename);
                    fhandle.filename = NULL;
                }
            }
            else
            {
                /* Fail if is_verified is set to verification failed */
                if(rpvalue->is_verified == VERIFICATION_FAILED)
                {
                    result = FATAL_FCACHE_ORIGINAL_OPEN;
                    goto Finished;
                }
            }
        }

        result = fcache_createval(pcache->pfcache, fullpath, &pfvalue);
        if(FAILED(result))
        {
            goto Finished;
        }

        lock_writelock(pcache->aprwlock);
        flock = 2;

        if(pvalue->fcacheval == 0)
        {
            pvalue->fcacheval = fcache_getoffset(pcache->pfcache, pfvalue);
            if(rpvalue != NULL)
            {
                rplist_setabsval(pcache->prplist, rpvalue, alloc_get_valueoffset(pcache->apalloc, pvalue), pvalue->resentry);
                pvalue->resentry = resentry;
            }
        }
        else
        {
            fcache_destroyval(pcache->pfcache, pfvalue);
            pfvalue = NULL;
        }
        
        lock_writeunlock(pcache->aprwlock);
        lock_readlock(pcache->aprwlock);
        flock = 1;
    }
    else
    {
        if(rpvalue != NULL && rpvalue->absentry == 0)
        {
            rplist_setabsval(pcache->prplist, rpvalue, alloc_get_valueoffset(pcache->apalloc, pvalue), pvalue->resentry);
            InterlockedExchange(&pvalue->resentry, resentry);
        }
    }

    if(pfvalue == NULL)
    {
        _ASSERT(pvalue->fcacheval != 0);
        pfvalue = fcache_getvalue(pcache->pfcache, pvalue->fcacheval);
        _ASSERT(pfvalue != NULL);
    }

    if(pfvalue != NULL)
    {
        /* Increment refcount while holding aplist readlock */
        fcache_refinc(pcache->pfcache, pfvalue);
    }

    /* If this is the first time this entry */
    /* got created, let original function do its job */
    *ppvalue = pfvalue;
    *ppfullpath = fullpath;

    lock_readunlock(pcache->aprwlock);
    flock = 0;
 
    _ASSERT(SUCCEEDED(result));

Finished:

    if(flock == 1)
    {
        lock_readunlock(pcache->aprwlock);
        flock = 0;
    }
    else if(flock == 2)
    {
        lock_writeunlock(pcache->aprwlock);
        flock = 0;
    }

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_fcache_get", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        if(fullpath != NULL)
        {
            alloc_efree(fullpath);
            fullpath = NULL;
        }
    }

    dprintverbose("end aplist_fcache_get");

    return result;
}

int aplist_fcache_use(aplist_context * pcache, const char * fullpath, fcache_value * pfvalue, zend_file_handle ** pphandle)
{
    int result = NONFATAL;

    dprintverbose("start aplist_fcache_use");

    _ASSERT(pfvalue  != NULL);
    _ASSERT(fullpath != NULL);
    _ASSERT(pphandle != NULL);

    result = fcache_useval(pcache->pfcache, fullpath, pfvalue, pphandle);
    if(FAILED(result))
    {
        goto Finished;
    }

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_fcache_use", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end aplist_fcache_use");
    return result;
}

void aplist_fcache_close(aplist_context * pcache, fcache_value * pfvalue)
{
    fcache_refdec(pcache->pfcache, pfvalue);
    return;
}

int aplist_ocache_get(aplist_context * pcache, const char * filename, zend_file_handle * file_handle, int type, zend_op_array ** poparray, ocache_value ** ppvalue TSRMLS_DC)
{
    int            result  = NONFATAL;
    unsigned int   findex  = 0;
    aplist_value * pvalue  = NULL;
    ocache_value * povalue = NULL;

    dprintverbose("start aplist_ocache_get");

    _ASSERT(pcache      != NULL);
    _ASSERT(filename    != NULL);
    _ASSERT(file_handle != NULL);
    _ASSERT(poparray    != NULL);
    _ASSERT(ppvalue     != NULL);

    *poparray = NULL;
    *ppvalue  = NULL;

    findex = utils_getindex(filename, pcache->apheader->valuecount);

    result = aplist_getentry(pcache, filename, findex, &pvalue);
    if(FAILED(result))
    {
        goto Finished;
    }

    lock_readlock(pcache->aprwlock);

    /* If opcode cache value is not created for this file, create one */
    if(pvalue->ocacheval == 0)
    {
        lock_readunlock(pcache->aprwlock);
        
        /* Create opcode cache entry in shared segment */
        result = ocache_createval(pcache->pocache, filename, file_handle, type, poparray, &povalue TSRMLS_CC);
        if(FAILED(result))
        {
            goto Finished;
        }

        /* Get a write lock and update ocacheval if some other */
        /* process didn't beat this process in updating the value */
        lock_writelock(pcache->aprwlock);
        if(pvalue->ocacheval == 0)
        {
            pvalue->ocacheval = ocache_getoffset(pcache->pocache, povalue);
        }
        else
        {
            ocache_destroyval(pcache->pocache, povalue);
            povalue = NULL;
        }
        
        lock_writeunlock(pcache->aprwlock);
        lock_readlock(pcache->aprwlock);
    }

    if(povalue == NULL)
    {
        _ASSERT(pvalue->ocacheval != 0);
        povalue = ocache_getvalue(pcache->pocache, pvalue->ocacheval);
    }

    _ASSERT(povalue != NULL);
    *ppvalue = povalue;

    if(*poparray == NULL)
    {
        /* We found an entry in the opcode cache */
        /* Do ref increment before releasing the readlock */
        ocache_refinc(pcache->pocache, povalue);
    }

    lock_readunlock(pcache->aprwlock);

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_ocache_get", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end aplist_ocache_get");

    return result;
}

int aplist_ocache_get_value(aplist_context * pcache, const char * filename, ocache_value ** ppvalue)
{
    int            result  = NONFATAL;
    unsigned int   findex  = 0;
    aplist_value * pvalue  = NULL;
    ocache_value * povalue = NULL;

    dprintverbose("start aplist_ocache_get_value");

    _ASSERT(pcache   != NULL);
    _ASSERT(filename != NULL);
    _ASSERT(ppvalue  != NULL);

    *ppvalue  = NULL;

    findex = utils_getindex(filename, pcache->apheader->valuecount);
    result = aplist_getentry(pcache, filename, findex, &pvalue);
    if(FAILED(result))
    {
        goto Finished;
    }

    lock_readlock(pcache->aprwlock);
    if(pvalue->ocacheval == 0)
    {
        lock_readunlock(pcache->aprwlock);
        result = FATAL_UNEXPECTED_DATA;

        goto Finished;
    }

    povalue = ocache_getvalue(pcache->pocache, pvalue->ocacheval);

    _ASSERT(povalue != NULL);
    *ppvalue = povalue;

    /* Do refinc while holding the lock so that ocache */
    /* entry doesn't get deleted while before refinc */
    ocache_refinc(pcache->pocache, povalue);

    lock_readunlock(pcache->aprwlock);

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_ocache_get_value", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end aplist_ocache_get_value");
    return result;
}

int aplist_ocache_use(aplist_context * pcache, ocache_value * povalue, zend_op_array ** pparray TSRMLS_DC)
{
    int result         = NONFATAL;

    dprintverbose("start aplist_ocache_use");

    _ASSERT(pcache      != NULL);
    _ASSERT(povalue     != NULL);
    _ASSERT(pparray     != NULL);

    result = ocache_useval(pcache->pocache, povalue, pparray TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    _ASSERT(SUCCEEDED(result));

Finished:

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_ocache_use", result);
        _ASSERT(result > WARNING_COMMON_BASE);
    }

    dprintverbose("end aplist_ocache_use");
    return result;
}

void aplist_ocache_close(aplist_context * pcache, ocache_value * povalue)
{
    ocache_refdec(pcache->pocache, povalue);
    return;
}

int aplist_getinfo(aplist_context * pcache, unsigned char type, zend_bool summaryonly, cache_info ** ppinfo)
{
    int                  result  = NONFATAL;
    cache_info *         pcinfo  = NULL;
    cache_entry_info *   peinfo  = NULL;
    cache_entry_info *   ptemp   = NULL;
    aplist_value *       pvalue  = NULL;

    unsigned char        flock   = 0;
    fcache_value *       pfvalue = NULL;
    ocache_value *       povalue = NULL;

    unsigned int         ticks   = 0;
    unsigned int         index   = 0;
    unsigned int         count   = 0;
    size_t               offset  = 0;

    dprintverbose("start aplist_getinfo");

    _ASSERT(pcache != NULL);
    _ASSERT(ppinfo != NULL);

    *ppinfo = NULL;

    pcinfo = (cache_info *)alloc_emalloc(sizeof(cache_info));
    if(pcinfo == NULL)
    {
        result = FATAL_OUT_OF_LMEMORY;
        goto Finished;
    }

    ticks = GetTickCount();

    lock_readlock(pcache->aprwlock);
    flock = 1;

    pcinfo->initage = utils_ticksdiff(ticks, pcache->apheader->init_ticks) / 1000;
    pcinfo->islocal = pcache->islocal;

    if(type == CACHE_TYPE_FILECONTENT)
    {
        pcinfo->itemcount = pcache->pfcache->header->itemcount;
        pcinfo->hitcount  = pcache->pfcache->header->hitcount;
        pcinfo->misscount = pcache->pfcache->header->misscount;
    }
    else if(type == CACHE_TYPE_BYTECODES)
    {
        pcinfo->itemcount = pcache->pocache->header->itemcount;
        pcinfo->hitcount  = pcache->pocache->header->hitcount;
        pcinfo->misscount = pcache->pocache->header->misscount;
    }

    pcinfo->entries = NULL;

    /* Leave count to 0 when only summary is required */
    if(!summaryonly)
    {
        count = pcache->apheader->valuecount;
    }

    for(index = 0; index < count; index++)
    {
        offset = pcache->apheader->values[index];
        if(offset == 0)
        {
            continue;
        }

        pvalue = (aplist_value *)alloc_get_cachevalue(pcache->apalloc, offset);
        while(pvalue != NULL)
        {
            if((type == CACHE_TYPE_FILECONTENT && pvalue->fcacheval != 0) || (type == CACHE_TYPE_BYTECODES && pvalue->ocacheval != 0))
            {
                ptemp = (cache_entry_info *)alloc_emalloc(sizeof(cache_entry_info));
                if(ptemp == NULL)
                {
                    result = FATAL_OUT_OF_LMEMORY;
                    goto Finished;
                }

                _ASSERT(pvalue->file_path != 0);

                ptemp->filename = alloc_estrdup(pcache->apmemaddr + pvalue->file_path);
                ptemp->addage   = utils_ticksdiff(ticks, pvalue->add_ticks) / 1000;
                ptemp->useage   = utils_ticksdiff(ticks, pvalue->use_ticks) / 1000;

                /* If last_check value is 0, leave it as 0 */
                ptemp->lchkage = 0;
                if(pvalue->last_check != 0)
                {
                    ptemp->lchkage  = utils_ticksdiff(ticks, pvalue->last_check) / 1000;
                }

                ptemp->cdata    = NULL;
                ptemp->next     = NULL;

                if(type == CACHE_TYPE_FILECONTENT)
                {
                    pfvalue = fcache_getvalue(pcache->pfcache, pvalue->fcacheval);
                    result = fcache_getinfo(pfvalue, (fcache_entry_info **)&ptemp->cdata);
                }
                else if(type == CACHE_TYPE_BYTECODES)
                {
                    povalue = ocache_getvalue(pcache->pocache, pvalue->ocacheval);
                    result = ocache_getinfo(povalue, (ocache_entry_info **)&ptemp->cdata);
                }

                if(FAILED(result))
                {
                    goto Finished;
                }

                if(peinfo != NULL)
                {
                    peinfo->next = ptemp;
                }
                else
                {
                    pcinfo->entries = ptemp;
                }

                peinfo = ptemp;
            }

            offset = pvalue->next_value;
            if(offset == 0)
            {
                break;
            }

            pvalue = (aplist_value *)alloc_get_cachevalue(pcache->apalloc, offset);
        }
    }

    *ppinfo = pcinfo;
    _ASSERT(SUCCEEDED(result));

Finished:

    if(flock)
    {
        lock_readunlock(pcache->aprwlock);
        flock = 0;
    }

    if(FAILED(result))
    {
        dprintimportant("failure %d in aplist_getinfo", result);
        _ASSERT(result > WARNING_COMMON_BASE);

        if(pcinfo != NULL)
        {
            peinfo = pcinfo->entries;
            while(peinfo != NULL)
            {
                ptemp = peinfo;
                peinfo = peinfo->next;

                if(ptemp->filename != NULL)
                {
                    alloc_efree(ptemp->filename);
                    ptemp->filename = NULL;
                }

                if(ptemp->cdata != NULL)
                {
                    if(type == CACHE_TYPE_FILECONTENT)
                    {
                        fcache_freeinfo(ptemp->cdata);
                    }
                    else if(type == CACHE_TYPE_BYTECODES)
                    {
                        ocache_freeinfo(ptemp->cdata);
                    }

                    ptemp->cdata = NULL;
                }

                alloc_efree(ptemp);
                ptemp = NULL;
            }

            alloc_efree(pcinfo);
            pcinfo = NULL;
        }
    }

    dprintverbose("start aplist_getinfo");

    return result;
}

void aplist_freeinfo(unsigned char type, cache_info * pinfo)
{
    cache_entry_info * peinfo = NULL;
    cache_entry_info * petemp = NULL;

    dprintverbose("start aplist_freeinfo");

    if(pinfo != NULL)
    {
        peinfo = pinfo->entries;
        while(peinfo != NULL)
        {
            petemp = peinfo;
            peinfo = peinfo->next;

            if(petemp->filename != NULL)
            {
                alloc_efree(petemp->filename);
                petemp->filename = NULL;
            }

            if(petemp->cdata != NULL)
            {
                if(type == CACHE_TYPE_FILECONTENT)
                {
                    fcache_freeinfo(petemp->cdata);
                }
                else if(type == CACHE_TYPE_BYTECODES)
                {
                    ocache_freeinfo(petemp->cdata);
                }

                petemp->cdata = NULL;
            }

            alloc_efree(petemp);
            petemp = NULL;
        }

        alloc_efree(pinfo);
        pinfo = NULL;
    }

    dprintverbose("end aplist_freeinfo");
    return;
}

void aplist_runtest()
{
    int              result    = NONFATAL;
    aplist_context * pcache    = NULL;
    aplist_value *   pvalue    = NULL;
    unsigned short   islocal   = 0;
    unsigned int     filecount = 1040;
    unsigned int     fchfreq   = 5;
    unsigned int     ttlmax    = 0;
    char *           filename  = "testfile.php";

    TSRMLS_FETCH();
    dprintverbose("*** STARTING APLIST TESTS ***");

    result = aplist_create(&pcache);
    if(FAILED(result))
    {
        goto Finished;
    }

    result = aplist_initialize(pcache, APLIST_TYPE_GLOBAL, filecount, fchfreq, ttlmax TSRMLS_CC);
    if(FAILED(result))
    {
        goto Finished;
    }

    _ASSERT(pcache->id          != 0);
    _ASSERT(pcache->fchangefreq == fchfreq * 1000);
    _ASSERT(pcache->apheader    != NULL);
    _ASSERT(pcache->apfilemap   != NULL);
    _ASSERT(pcache->aprwlock    != NULL);
    _ASSERT(pcache->apalloc     != NULL);

    _ASSERT(pcache->apheader->valuecount == filecount);
    _ASSERT(pcache->apheader->itemcount  == 0);

Finished:

    if(pcache != NULL)
    {
        aplist_terminate(pcache);
        aplist_destroy(pcache);

        pcache =  NULL;
    }

    dprintverbose("*** ENDING APLIST TESTS ***");

    return;
}
