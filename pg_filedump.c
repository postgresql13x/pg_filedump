/*
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define FD_VERSION      "12.0"                  /* version ID of pg_filedump */
#define FD_PG_VERSION   "PostgreSQL 12.x"       /* PG version it works with */

#define BLCKSZ 8192

/* RELSEG_SIZE is the maximum number of blocks allowed in one disk file. Thus,
 * the maximum size of a single file is RELSEG_SIZE * BLCKSZ; relations bigger
 * than that are divided into multiple files. RELSEG_SIZE * BLCKSZ must be
 * less than your OS' limit on file size. This is often 2 GB or 4GB in a
 * 32-bit operating system, unless you have large file support enabled. By
 * default, we make the limit 1 GB to avoid any possible integer-overflow
 * problems within the OS. A limit smaller than necessary only means we divide
 * a large relation into more chunks than necessary, so it seems best to err
 * in the direction of a small limit. A power-of-2 value is recommended to
 * save a few cycles in md.c, but is not absolutely required. Changing
 * RELSEG_SIZE requires an initdb. */
#define RELSEG_SIZE 131072

/*
 *  offsetof
 *    Offset of a structure/union field within that structure/union.
 *  
 *    XXX This is supposed to be part of stddef.h, but isn't on
 *    some systems (like SunOS 4).
 */

#ifndef offsetof
#define offsetof(type, field)   ((long) &((type *)0)->field)
#endif           

typedef enum blockSwitches
{
    BLOCK_ABSOLUTE     = 0x00000001,    /* -a: Absolute(vs Relative) addressing */
    BLOCK_BINARY       = 0x00000002,    /* -b: Binary dump of block */
    BLOCK_FORMAT       = 0x00000004,    /* -f: Formatted dump of blocks / control file */
    BLOCK_FORCED       = 0x00000008,    /* -S: Block size forced */
    BLOCK_NO_INTR      = 0x00000010,    /* -d: Dump straight blocks */
    BLOCK_RANGE        = 0x00000020,    /* -R: Specific block range to dump */
    BLOCK_CHECKSUMS    = 0x00000040,    /* -k: verify block checksums */
    BLOCK_DECODE       = 0x00000080,    /* -D: Try to decode tuples */
    BLOCK_DECODE_TOAST = 0x00000100,    /* -t: Try to decode TOAST values */
    BLOCK_IGNORE_OLD   = 0x00000200     /* -o: Decode old values */
} blockSwitches;

typedef enum segmentSwitches
{
    SEGMENT_SIZE_FORCED   = 0x00000001,  /* -s: Segment size forced */
    SEGMENT_NUMBER_FORCED = 0x00000002,  /* -n: Segment number forced */
} segmentSwitches;

typedef enum itemSwitches
{
    ITEM_DETAIL    = 0x00000001,  /* -i: Display interpreted items */
    ITEM_HEAP      = 0x00000002,  /* -y: Blocks contain HeapTuple items */
    ITEM_INDEX     = 0x00000004,  /* -x: Blocks contain IndexTuple items */
    ITEM_SPG_INNER = 0x00000008,  /* Blocks contain SpGistInnerTuple items */
    ITEM_SPG_LEAF  = 0x00000010   /* Blocks contain SpGistLeafTuple items */
} itemSwitches;

/* Possible return codes from option validation routine.
 *  * pg_filedump doesn't do much with them now but maybe in
 *   * the future... */
typedef enum optionReturnCodes
{
    OPT_RC_VALID,               /* All options are valid */
    OPT_RC_INVALID,             /* Improper option string */
    OPT_RC_FILE,                /* File problems */
    OPT_RC_DUPLICATE,           /* Duplicate option encountered */
    OPT_RC_COPYRIGHT            /* Copyright should be displayed */
}   optionReturnCodes;

typedef enum controlSwitches
{
    CONTROL_DUMP   = 0x00000001,    /* -c: Dump control file */
    CONTROL_FORMAT = BLOCK_FORMAT,  /* -f: Formatted dump of control file */
    CONTROL_FORCED = BLOCK_FORCED   /* -S: Block size forced */
} controlSwitches;

/* Possible value types for the Special Section */
typedef enum specialSectionTypes
{
    SPEC_SECT_NONE,             /* No special section on block */
    SPEC_SECT_SEQUENCE,         /* Sequence info in special section */
    SPEC_SECT_INDEX_BTREE,      /* BTree index info in special section */
    SPEC_SECT_INDEX_HASH,       /* Hash index info in special section */
    SPEC_SECT_INDEX_GIST,       /* GIST index info in special section */
    SPEC_SECT_INDEX_GIN,        /* GIN index info in special section */
    SPEC_SECT_INDEX_SPGIST,     /* SP - GIST index info in special section */
    SPEC_SECT_ERROR_UNKNOWN,    /* Unknown error */
    SPEC_SECT_ERROR_BOUNDARY    /* Boundary error */
} specialSectionTypes;

/* Simple macro to check for duplicate options and then set
 * an option flag for later consumption */
#define SET_OPTION(_x,_y,_z) if (_x & _y)               \
                               {                        \
                                 rc = OPT_RC_DUPLICATE; \
                                 duplicateSwitch = _z;  \
                               }                        \
                             else                       \
                               _x |= _y;

#define SEQUENCE_MAGIC 0x1717   /* PostgreSQL defined magic number */
#define EOF_ENCOUNTERED (-1)    /* Indicator for partial read */
#define BYTES_PER_LINE 16       /* Format the binary 16 bytes per line */

typedef unsigned char uint8;    /* == 8 bits */
typedef unsigned short uint16;  /* == 16 bits */
typedef unsigned int uint32;    /* == 32 bits */

typedef size_t Size;
typedef unsigned int Oid;
typedef uint32 BlockNumber;

/*
 *  For historical reasons, the 64-bit LSN value is stored as two 32-bit
 *   values.
 */
typedef struct
{
        uint32          xlogid;                 /* high bits */
        uint32          xrecoff;                /* low bits */
} PageXLogRecPtr;

/*
 *  location (byte offset) within a page.
 *  
 *  note that this is actually limited to 2^15 because we have limited
 *  ItemIdData.lp_off and ItemIdData.lp_len to 15 bits (see itemid.h).
 */
typedef uint16 LocationIndex;

typedef uint32 TransactionId;

/*
 *  A line pointer on a buffer page.  See buffer page definitions and comments
 *  for an explanation of how line pointers are used.
 *  
 *  In some cases a line pointer is "in use" but does not have any associated
 *  storage on the page.  By convention, lp_len == 0 in every line pointer
 *  that does not have storage, independently of its lp_flags state.
 */
typedef struct ItemIdData
{
        unsigned        lp_off:15,              /* offset to tuple (from start of page) */
                        lp_flags:2,             /* state of line pointer, see below */
                        lp_len:15;              /* byte length of tuple */
} ItemIdData;

typedef ItemIdData *ItemId;

/*
 *  lp_flags has these possible states.  An UNUSED line pointer is available
 *  for immediate re-use, the other states are not.
 */
#define LP_UNUSED               0               /* unused (should always have lp_len=0) */
#define LP_NORMAL               1               /* used (should always have lp_len>0) */
#define LP_REDIRECT             2               /* HOT redirect (should have lp_len=0) */
#define LP_DEAD                 3               /* dead, may or may not have storage */


typedef struct PageHeaderData
{
        /* XXX LSN is member of *any* block, not only page-organized ones */
        PageXLogRecPtr pd_lsn;          /* LSN: next byte after last byte of xlog
                                         * record for last change to this page */
        uint16         pd_checksum;     /* checksum */
        uint16         pd_flags;               /* flag bits, see below */
        LocationIndex  pd_lower;         /* offset to start of free space */
        LocationIndex  pd_upper;         /* offset to end of free space */
        LocationIndex  pd_special;       /* offset to start of special space */
        uint16         pd_pagesize_version;
        TransactionId  pd_prune_xid; /* oldest prunable XID, or zero if none */
        ItemIdData     pd_linp[1]; /* line pointer array */
} PageHeaderData;

typedef PageHeaderData *PageHeader;

/*
 *  * line pointer(s) do not count as part of header
 *   */
#define SizeOfPageHeaderData (offsetof(PageHeaderData, pd_linp))

/*
 *  PageGetPageSize
 *     Returns the page size of a page.
 *  
 *  this can only be called on a formatted page (unlike
 *  BufferGetPageSize, which can be called on an unformatted page).
 *  however, it can be called on a page that is not stored in a buffer.
 */
#define PageGetPageSize(page) \
        ((Size) (((PageHeader) (page))->pd_pagesize_version & (uint16) 0xFF00))

/*
 * PageGetMaxOffsetNumber
 *               Returns the maximum offset number used by the given page.
 *               Since offset numbers are 1-based, this is also the number
 *               of items on the page.
 * 
 *               NOTE: if the page is not initialized (pd_lower == 0), we must
 *               return zero to ensure sane behavior.  Accept double evaluation
 *               of the argument so that we can ensure this.
 */
#define PageGetMaxOffsetNumber(page) \
        (((PageHeader) (page))->pd_lower <= SizeOfPageHeaderData ? 0 : \
         ((((PageHeader) (page))->pd_lower - SizeOfPageHeaderData) \
          / sizeof(ItemIdData)))

typedef char *Pointer;
typedef Pointer Page;

/*
 *  PageGetItemId
 *        Returns an item identifier of a page.
 */

#define PageGetItemId(page, offsetNumber) \
        ((ItemId) (&((PageHeader) (page))->pd_linp[(offsetNumber) - 1]))

#if 0
typedef struct HeapTupleHeaderData HeapTupleHeaderData;

typedef HeapTupleHeaderData *HeapTupleHeader;

#endif

/*
 *  Global variables for ease of use mostly
 *   
 */

/*  Options for Block formatting operations */
unsigned int blockOptions = 0;

/* Segment-related options */
unsigned int segmentOptions = 0;

/* -R[start]:Block range start */
int blockStart = -1;

/* -R[end]:Block range end */
int blockEnd = -1;

/* Options for Item formatting operations */
unsigned int itemOptions = 0;

/* Options for Control File formatting operations */
unsigned int controlOptions = 0;

unsigned int specialType = SPEC_SECT_NONE;

static int verbose = 0;  /* 0 - false, non-zeor: true */


/* File name for display */
char *fileName = NULL;

/* Current block size */
static unsigned int blockSize = 0;

/* Segment size in bytes */
static unsigned int segmentSize = RELSEG_SIZE * BLCKSZ;

/* Number of current segment */
static unsigned int segmentNumber = 0;

/* Offset of current block */
static unsigned int pageOffset = 0;

/* Number of bytes to format */
static unsigned int bytesToFormat = 0;

/* Block version number */
static unsigned int blockVersion = 0;

/* Program exit code */
static int  exitCode = 0;

static void
DisplayOptions(unsigned int validOptions)
{
    if(OPT_RC_COPYRIGHT == validOptions) {
        printf("\nVersion %s (for %s)"
               "\nCopyright (c) 2002-2010 Red Hat, Inc."
               "\nCopyright (c) 2011-2019, PostgreSQL Global Development Group\n",
               FD_VERSION, FD_PG_VERSION);
    }

    printf
        ("\nUsage: pg_filedump [-abcdfhikxy] [-R startblock [endblock]] [-D attrlist] [-S blocksize] [-s segsize] [-n segnumber] file\n\n"
         "Display formatted contents of a PostgreSQL heap/index/control file\n"
         "Defaults are: relative addressing, range of the entire file, block\n"
         "               size as listed on block 0 in the file\n\n"
         "The following options are valid for heap and index files:\n"
         "  -a  Display absolute addresses when formatting (Block header\n"
         "      information is always block relative)\n"
         "  -b  Display binary block images within a range (Option will turn\n"
         "      off all formatting options)\n"
         "  -d  Display formatted block content dump (Option will turn off\n"
         "      all other formatting options)\n"
         "  -D  Decode tuples using given comma separated list of types\n"
         "      Supported types:\n"
         "        bigint bigserial bool char charN date float float4 float8 int\n"
         "        json macaddr name oid real serial smallint smallserial text\n"
         "        time timestamp timetz uuid varchar varcharN xid xml\n"
         "      ~ ignores all attributes left in a tuple\n"
         "  -f  Display formatted block content dump along with interpretation\n"
         "  -h  Display this information\n"
         "  -i  Display interpreted item details\n"
         "  -k  Verify block checksums\n"
         "  -o  Do not dump old values.\n"
         "  -R  Display specific block ranges within the file (Blocks are\n"
         "      indexed from 0)\n"
         "        [startblock]: block to start at\n"
         "        [endblock]: block to end at\n"
         "      A startblock without an endblock will format the single block\n"
         "  -s  Force segment size to [segsize]\n"
         "  -t  Dump TOAST files\n"
         "  -v  Ouput additional information about TOAST relations\n"
         "  -n  Force segment number to [segnumber]\n"
         "  -S  Force block size to [blocksize]\n"
         "  -x  Force interpreted formatting of block items as index items\n"
         "  -y  Force interpreted formatting of block items as heap items\n\n"
         "The following options are valid for control files:\n"
         "  -c  Interpret the file listed as a control file\n"
         "  -f  Display formatted content dump along with interpretation\n"
         "  -S  Force block size to [blocksize]\n"
         "\nReport bugs to <pgsql-bugs@postgresql.org>\n");

}

/* Given the index into the parameter list, convert and return the
 * current string to a number if possible */
static int
GetOptionValue(char *optionString)
{
    unsigned int x;
    int          value = -1;
    int          optionStringLength = strlen(optionString);

    /* Verify the next option looks like a number */
    for (x = 0; x < optionStringLength; x++)
        if (!isdigit((int) optionString[x]))
            break;

    /* Convert the string to a number if it looks good */
    if (x == optionStringLength)
        value = atoi(optionString);

    return (value);
}


/* Read the page header off of block 0 to determine the block size
 * used in this file.  Can be overridden using the -S option. The
 * returned value is the block size of block 0 on disk */
unsigned int
GetBlockSize(FILE *fp)
{
    unsigned int localSize = 0;
    int          bytesRead = 0;
    char         localCache[sizeof(PageHeaderData)];

    /* Read the first header off of block 0 to determine the block size */
    bytesRead = fread(&localCache, 1, sizeof(PageHeaderData), fp);
    rewind(fp);

    if(sizeof(PageHeaderData) == bytesRead) {
        localSize = (unsigned int) PageGetPageSize(&localCache);
        printf("((PageHeader) (page))->pd_pagesize_version=%4X | localSize = %d\n", ((PageHeader)localCache)->pd_pagesize_version, localSize);
    }
    else 
    {
        printf("Error: Unable to read full page header from block 0.\n"
               "  ===> Read %u bytes\n", bytesRead);
        exitCode = 1;
    }

    if(0 == localSize)
    {
        printf("Notice: Block size determined from reading block 0 is zero, using default %d instead.\n", BLCKSZ);
        printf("Hint: Use -S <size> to specify the size manually.\n");
        localSize = BLCKSZ;
    }

    return localSize;
}




/*  Iterate through the provided options and set the option flags.
 *  An error will result in a positive rc and will force a display
 *  of the usage information.  This routine returns enum
 *  optionReturnCode values. 
 */

static unsigned int
ConsumeOptions(int numOptions, char **options)
{
#if 0
    unsigned int rc = OPT_RC_VALID;
    unsigned int x;
    unsigned int optionStringLength;
    char        *optionString;
    char         duplicateSwitch = 0x00;

    for(x=1; x < numOptions; x++)
    {
        optionString = options[x];
        optionStringLength = strlen(optionString);
        
        if((2 == optionStringLength) && (0 == strcmp(optionString, "-R")))
        {
            int range = 0;

            SET_OPTION(blockOptions, BLOCK_RANGE, 'R');
            /* Only accept the range option once */
            if(OPT_RC_DUPLICATE == rc)
                break;

            /* Make sure there are options after the range identifier */
            if (x >= (numOptions - 2))
            {
                rc = OPT_RC_INVALID;
                printf("Error: Missing range start identifier.\n");
                exitCode = 1;rc = OPT_RC_INVALID;
                break;
            }

            /*
             * Mark that we have the range and advance the option to what
             * should be the range start. Check the value of the next
             * parameter */
            optionString = options[++x];
            if ((range = GetOptionValue(optionString)) < 0)
            {
                rc = OPT_RC_INVALID;
                printf("Error: Invalid range start identifier <%s>.\n",
                       optionString);
                exitCode = 1;
                break;
            }

            /* The default is to dump only one block */
            blockStart = blockEnd = (unsigned int) range;

            /* We have our range start marker, check if there is an end
             * marker on the option line.  Assume that the last option
             * is the file we are dumping, so check if there are options
             * range start marker and the file */
            if (x <= (numOptions - 3))
            {
                if ((range = GetOptionValue(options[x + 1])) >= 0)
                {
                    /* End range must be => start range */
                    if (blockStart <= range)
                    {
                        blockEnd = (unsigned int) range;
                        x++;
                    }
                    else
                    {
                        rc = OPT_RC_INVALID;
                        printf("Error: Requested block range start <%d> is "
                               "greater than end <%d>.\n", blockStart, range);
                        exitCode = 1;
                        break;
                    }
                }
            }

                
        }

    }
#endif
    return 0;
}

/*  Dump out a formatted block header for the requested block */
static int
FormatHeader(char *buffer, Page page, BlockNumber blkno, unsigned int isToast)
{
    int rc = 0;
    unsigned int headerBytes;
    PageHeader pageHeader = (PageHeader)page;
    char       *indent = isToast ? "\t" : "";

    if(!isToast || verbose)
        printf("%s<Header> -----\n", indent);

    /* Only attempt to format the header if the entire header (minus the item array) is available */
    int o = offsetof(PageHeaderData, pd_linp[0]);
    /* printf("bytesToFormat = %4X | offset = %4X\n", bytesToFormat, o); */

    if (bytesToFormat < offsetof(PageHeaderData, pd_linp[0]))
    {
        headerBytes = bytesToFormat;
        rc = EOF_ENCOUNTERED;
    }
    else {
        PageXLogRecPtr *plsn = &pageHeader->pd_lsn;
        int maxOffset = PageGetMaxOffsetNumber(page); /* how many records */
        char flagString[100];

        headerBytes = offsetof(PageHeaderData, pd_linp[0]);
        /* printf("headerBytes = %d\n", headerBytes); */
        blockVersion = (unsigned int)(pageHeader->pd_pagesize_version & (uint16) 0x00FF);
        unsigned int pageSize = (unsigned int)(pageHeader->pd_pagesize_version & (uint16) 0xFF00);

        /* The full header exists but we have to check that the item array
         * is available or how far we can index into it */
        if(maxOffset > 0) {
            unsigned int itemsLength = maxOffset * sizeof(ItemIdData);

            if(bytesToFormat < (headerBytes + itemsLength)) {
                headerBytes = bytesToFormat;
                rc = EOF_ENCOUNTERED;
            }
            else
                headerBytes += itemsLength;
        }

        flagString[0] = '\0';

        if(!isToast || verbose) {
            printf("%s Block Offset: 0x%08X        Offsets: Lower    %4u (0x%04hX)\n",
                    indent, pageOffset, pageHeader->pd_lower, pageHeader->pd_lower);
            printf("%s Block: Size %4d  Version %4u           Upper    %4u (0x%04hX)\n",
                    indent, pageSize, blockVersion,
                    pageHeader->pd_upper, pageHeader->pd_upper);
            printf("%s LSN logid %6d recoff 0x%08X       Special  %4u (0x%04hX)\n",
                    indent, plsn->xlogid, plsn->xrecoff, pageHeader->pd_special, pageHeader->pd_special);
            printf("%s Items: %4d                     Free Space: %4u (bytes)\n",
                    indent, maxOffset, pageHeader->pd_upper - pageHeader->pd_lower);
            printf("%s Checksum: 0x%04X  Prune XID: 0x%08X  Flags: 0x%04X (%s)\n",
                    indent, pageHeader->pd_checksum, pageHeader->pd_prune_xid,
                    pageHeader->pd_flags, flagString);
            printf("%s Length (including item array): %u\n\n",
                    indent, headerBytes);
        }
    }

    return (rc);
}

/*  Dump out formatted items that reside on this block */
static void
FormatItemBlock(char *buffer,
                Page page,
                unsigned int isToast,
                Oid toastOid,
                unsigned int toastExternalSize,
                char *toastValue,
                unsigned int *toastRead)
{
    unsigned int x;
    unsigned int itemSize;
    unsigned int itemOffset;
    unsigned int itemFlags;
    ItemId       itemId;
    int          maxOffset = PageGetMaxOffsetNumber(page);
    char       *indent = isToast ? "\t" : "";

    if(!isToast || verbose)
        printf("%s<Data> -----\n", indent);

    /* Loop through the items on the block.  Check if the block is
     * empty and has a sensible item array listed before running
     * through each item */
    if(0 == maxOffset) {
        if (!isToast || verbose)
            printf("%s Empty block - no items listed \n\n", indent);
    }
    else {
        int          formatAs = ITEM_HEAP; 
        char         textFlags[16];
        uint32       chunkId;
        unsigned int chunkSize = 0;

        for(x=1; x<(maxOffset + 1); x++)
        {
            itemId = PageGetItemId(page, x);
            itemFlags  = (unsigned int)itemId->lp_flags;
            itemSize   = (unsigned int)itemId->lp_len;
            itemOffset = (unsigned int)itemId->lp_off;

            switch(itemFlags) {
                case LP_UNUSED:
                    strcpy(textFlags, "UNUSED");
                    break;
                case LP_NORMAL:
                    strcpy(textFlags, "NORMAL");
                    break;
                case LP_REDIRECT:
                    strcpy(textFlags, "REDIRECT");
                    break;
                case LP_DEAD:
                    strcpy(textFlags, "DEAD");
                    break;
                default:
                    /* shouldn't be possible */
                    sprintf(textFlags, "0x%02x", itemFlags);
                    break;
            }
            
#if 1
            unsigned int *p = (unsigned int*)itemId;
            printf("==> RAW: %08X | lp_off = %08X | lp_flag = %08X | lp_len = %08X [%s]\n", *p, itemOffset, itemFlags, itemSize, textFlags);
#endif 
            /* Make sure the item can physically fit on this block before formatting */
            if ((itemOffset + itemSize > blockSize) || (itemOffset + itemSize > bytesToFormat)) {
                if (!isToast || verbose)
                    printf("%s  Error: Item contents extend beyond block.\n"
                           "%s         BlockSize<%d> Bytes Read<%d> Item Start<%d>.\n",
                           indent, indent, blockSize, bytesToFormat, itemOffset + itemSize);
                exitCode = 1;
            }
            else {
                 
            }
        }
    }

}

/* Determine the contents of the special section on the block and return this enum value */
static unsigned int
GetSpecialSectionType(char *buffer, Page page)
{
    unsigned int rc;
    unsigned int specialOffset;
    unsigned int specialSize;
    unsigned int specialValue;
    PageHeader pageHeader = (PageHeader)page;

    /* If this is not a partial header, check the validity of the
     * special section offset and contents */ 
    if (bytesToFormat > sizeof(PageHeaderData)) {
        specialOffset = (unsigned int)pageHeader->pd_special;
        /* Check that the special offset can remain on the block or the partial block */
        printf("pageHeader->pd_special = %4X | blockSize=%4X | bytesToFormat = %4X\n", specialOffset, blockSize, bytesToFormat); 
        if ((specialOffset == 0) ||
            (specialOffset > blockSize) || (specialOffset > bytesToFormat))
            rc = SPEC_SECT_ERROR_BOUNDARY;
        else {
            /* we may need to examine last 2 bytes of page to identify index */
            uint16     *ptype = (uint16 *) (buffer + blockSize - sizeof(uint16));

            specialSize = blockSize - specialOffset;
            if (specialSize == 0)
                rc = SPEC_SECT_NONE;

        }
    }

    rc = SPEC_SECT_NONE;
    return (rc);
}

/*  For each block, dump out formatted header and content information */
static void
FormatBlock(unsigned int blockOptions,
            unsigned int controlOptions,
            char *buffer,
            BlockNumber currentBlock,
            unsigned int blockSize,
            unsigned int isToast,
            Oid   toastOid,
            unsigned int toastExternalSize,
            char *toastValue,
            unsigned int *toastRead)
{
    Page  page = (Page)buffer;
    char *indent = isToast? "\t" : "";

    pageOffset = blockSize * currentBlock;
    specialType = GetSpecialSectionType(buffer, page);

    if (!isToast || verbose)
        printf("\n%sBlock %4u **%s***************************************\n",
               indent,
               currentBlock,
               (bytesToFormat ==
                blockSize) ? "***************" : " PARTIAL BLOCK ");
    
    /* Either dump out the entire block in hex+acsii fashion or
     * interpret the data based on block structure */
#if 0
    if (blockOptions & BLOCK_NO_INTR)
        FormatBinary(buffer, bytesToFormat, 0);
    else
#endif 
    {
        int rc;

        rc = FormatHeader(buffer, page, currentBlock, isToast);

        /* If we didn't encounter a partial read in the header, carry on... */
        if(EOF_ENCOUNTERED != rc) {
            FormatItemBlock(buffer,
                    page,
                    isToast,
                    toastOid,
                    toastExternalSize,
                    toastValue,
                    toastRead);
#if 0
            if (specialType != SPEC_SECT_NONE)
                FormatSpecial(buffer);
#endif 

        }
    }

}

static void
DumpBinaryBlock(char *buffer)
{
    unsigned int x;
    
    for(x = 0; x < bytesToFormat; x++)
        putchar(buffer[x]);
}

/* Control the dumping of the blocks within the file */
int 
DumpFileContents(unsigned int blockOptions,
                 unsigned int controlOptions,
                 FILE *fp,
                 unsigned int blockSize,
                 int blockStart,
                 int blockEnd,
                 int isToast,
                 Oid toastOid,
                 unsigned int toastExternalSize,
                 char *toastValue)
{
    unsigned int    initialRead    = 1;
    unsigned int    contentsToDump = 1;
    unsigned int    toastDataRead  = 0;
    BlockNumber     currentBlock   = 0;
    int             result         = 0;
    char           *block          = NULL;

    /* On a positive block size, allocate a local buffer to store the subsequent blocks */
    block = (char*)malloc(blockSize); 
    if(NULL == block) {
        printf("\nError: Unable to create buffer of size <%d>.\n", blockSize);
        result = 1;
    }

    /* If the user requested a block range, seek to the correct position within the file for the start block. */
    if( 0 == result && (blockOptions & BLOCK_RANGE)) {
        unsigned int position = blockSize * blockStart;

        if(0 != fseek(fp, position, SEEK_SET)) {
            printf("Error: Seek error encountered before requested "
                   "start block <%d>.\n", blockStart);
            contentsToDump = 0;
            result = 1;
        }
        else 
            currentBlock = blockStart;
    }

    /* Iterate through the blocks in the file until you reach the end or the requested range end */
    while (0 != contentsToDump && 0 == result)
    {
        bytesToFormat = fread(block, 1, blockSize, fp);
        
        if(0 == bytesToFormat) {
            /* fseek() won't pop an error if you seek passed eof. The next subsequent read gets the error. */
            if (initialRead)
                printf("Error: Premature end of file encountered.\n");
            else if (!(blockOptions & BLOCK_BINARY))
                printf("\n*** End of File Encountered. Last Block "
                        "Read: %d ***\n", currentBlock - 1);
            contentsToDump = 0;
        }
        else {
            if (blockOptions & BLOCK_BINARY)
                DumpBinaryBlock(block);
            else {
                FormatBlock(blockOptions,
                            controlOptions,
                            block,
                            currentBlock,
                            blockSize,
                            isToast,
                            toastOid,
                            toastExternalSize,
                            toastValue,
                            &toastDataRead);
            }
        }
        
        /* Check to see if we are at the end of the requested range. */
        if((blockOptions & BLOCK_RANGE) && (currentBlock >= blockEnd) && (contentsToDump)) {
            /* Don't print out message if we're doing a binary dump */
            if (!(blockOptions & BLOCK_BINARY))
                printf("\n*** End of Requested Range Encountered. "
                       "Last Block Read: %d ***\n", currentBlock);
            contentsToDump = 0;
        }
        else
            currentBlock++;

        initialRead = 0;

        /* If TOAST data is read */
        if (isToast && toastDataRead >= toastExternalSize)
            break;
    }

    free(block);

    return (result);

}

int main(int argc, char* argv[])
{
    /* If there is a parameter list, validate the options */
    unsigned int validOptions;

    FILE *fp = NULL;  /* File to dump or format */

    validOptions = (argc < 2) ? OPT_RC_COPYRIGHT : ConsumeOptions(argc, argv);

    if(OPT_RC_VALID != validOptions) {
        DisplayOptions(validOptions);
        return 0;
    }

    if(argc != 2) return 0;

    fp = fopen(argv[1], "rb");
    if(NULL == fp) 
    {
        printf("Error: Could not open file <%s>.\n", argv[1]);
        return 0;
    }
    
    blockSize = GetBlockSize(fp);
    printf("blockSize = %d\n", blockSize);

    exitCode =  DumpFileContents(blockOptions,
                    controlOptions,
                    fp,
                    blockSize,
                    blockStart,
                    blockEnd,
                    0,  /* is toast realtion */
                    0,  /* no toast Oid */
                    0,  /* no toast external size */
                    NULL /* no out toast value */
                    );

    fclose(fp);

	return (exitCode);
}

