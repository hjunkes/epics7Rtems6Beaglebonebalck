/* devAiTestAsyn.c */
/* share/src/dev $Id$ */

/* devAiTestAsyn.c - Device Support Routines for testing asynchronous processing*/


#include	<vxWorks.h>
#include	<types.h>
#include	<stdioLib.h>
#include	<wdLib.h>

#include	<alarm.h>
#include	<cvtTable.h>
#include	<dbDefs.h>
#include	<dbAccess.h>
#include	<recSup.h>
#include	<devSup.h>
#include	<link.h>
#include	<aiRecord.h>

/* Create the dset for devAiTestAsyn */
long init_record();
long read_ai();
struct {
	long		number;
	DEVSUPFUN	report;
	DEVSUPFUN	init;
	DEVSUPFUN	init_record;
	DEVSUPFUN	get_ioint_info;
	DEVSUPFUN	read_ai;
	DEVSUPFUN	special_linconv;
}devAiTestAsyn={
	6,
	NULL,
	NULL,
	init_record,
	NULL,
	read_ai,
	NULL};

/* control block for callback*/
struct callback {
	void (*callback)();
	struct dbAddr dbAddr;
	WDOG_ID wd_id;
	short   completion;
};

void callbackRequest();

static void myCallback(pcallback)
    struct callback *pcallback;
{
    struct aiRecord *pai=(struct aiRecord *)(pcallback->dbAddr.precord);

    dbScanLock(pai);
    pcallback->completion = TRUE;
    pai->pact=0;
    dbScanPassive(&(pcallback->dbAddr));
    dbScanUnlock(pai);
}
    
    

static long init_record(pai)
    struct aiRecord	*pai;
{
    char message[100];
    struct callback *pcallback;
    int  precTypeIndex;
    struct rset *prset;

    /* ai.inp must be a CONSTANT*/
    switch (pai->inp.type) {
    case (CONSTANT) :
	pcallback = (struct callback *)(calloc(1,sizeof(struct callback *)));
	pai->dpvt = (caddr_t)pcallback;
	pcallback->callback = myCallback;
	if(dbNameToAddr(pai->name,&(pcallback->dbAddr))) {
		logMsg("dbNameToAddr failed in init_record for devAiTestAsyn\n");
		exit(1);
	}
	pcallback->wd_id = wdCreate();
	pai->val = pai->inp.value.value;
	break;
    default :
	strcpy(message,pai->name);
	strcat(message,": devAiTestAsyn (init_record) Illegal INP field");
	errMessage(S_db_badField,message);
	return(S_db_badField);
    }
    /* Make sure record processing routine does not perform any conversion*/
    pai->linr=0;
    return(0);
}

static long read_ai(pai)
    struct aiRecord	*pai;
{
    char message[100];
    long status,options,nRequest;
    struct callback *pcallback=(struct callback *)(pai->dpvt);
    short	wait_time;

    /* ai.inp must be a CONSTANT*/
    switch (pai->inp.type) {
    case (CONSTANT) :
	if(pcallback->completion==TRUE) {
		printf("%s Completed\n",pai->name);
		pcallback->completion=FALSE;
		return(0);
	} else {
		wait_time = (short)(pai->val);
		if(wait_time<=0) return(0);
		printf("%s Starting asynchronous processing\n",pai->name);
		wdStart(pcallback->wd_id,wait_time,callbackRequest,pcallback);
		return(1);
	}
    default :
	if(pai->nsev<MAJOR_ALARM) {
		pai->nsev = MAJOR_ALARM;
		pai->nsta = SOFT_ALARM;
		if(pai->stat!=SOFT_ALARM) {
			strcpy(message,pai->name);
			strcat(message,": devAiTestAsyn (read_ai) Illegal INP field");
			errMessage(S_db_badField,message);
		}
	}
    }
    return(0);
}
