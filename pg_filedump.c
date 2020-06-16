/*
 *
 *
 *
 *
 *
 *
 *
 */

#include <stdio.h>
#include <stdlib.h>

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


int main(int argc, char* argv[])
{
    /* If there is a parameter list, validate the options */
    unsigned int validOptions;

    validOptions = (argc < 2) ? OPT_RC_COPYRIGHT : OPT_RC_VALID;

	printf("Hello, PostgreSQL!\n");

	return 0;
}

