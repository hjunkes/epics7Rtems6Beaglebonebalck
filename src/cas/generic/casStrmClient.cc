/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE Versions 3.13.7
* and higher are distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/*
 *      $Id$
 *
 *      Author  Jeffrey O. Hill
 *              johill@lanl.gov
 *              505 665 1831
 */

// *must* be defined before including net_convert.h
typedef unsigned long arrayElementCount;

#include "osiWireFormat.h"
#include "net_convert.h"	// byte order conversion from libca
#include "dbMapper.h"		// ait to dbr types
#include "gddAppTable.h"    // EPICS application type table
#include "gddApps.h"		// gdd predefined application type codes

#define epicsExportSharedSymbols
#include "casStrmClient.h"
#include "casChannelI.h"
#include "casAsyncIOI.h"
#include "channelDestroyEvent.h"

static const caHdr nill_msg = { 0u, 0u, 0u, 0u, 0u, 0u };

casStrmClient::pCASMsgHandler const casStrmClient::msgHandlers[] =
{
	& casStrmClient::versionAction,
	& casStrmClient::eventAddAction,
	& casStrmClient::eventCancelAction,
	& casStrmClient::readAction,
	& casStrmClient::writeAction,
    & casStrmClient::uknownMessageAction, 
    & casStrmClient::uknownMessageAction, 
    & casStrmClient::uknownMessageAction, 
	& casStrmClient::eventsOffAction,
	& casStrmClient::eventsOnAction,
	& casStrmClient::readSyncAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::clearChannelAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::readNotifyAction,
	& casStrmClient::ignoreMsgAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::claimChannelAction,
	& casStrmClient::writeNotifyAction,
	& casStrmClient::clientNameAction,
	& casStrmClient::hostNameAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::echoAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::uknownMessageAction,
	& casStrmClient::uknownMessageAction
};

//
// casStrmClient::casStrmClient()
//
casStrmClient::casStrmClient ( caServerI & cas, clientBufMemoryManager & mgrIn ) :
    casCoreClient ( cas ),
    in ( *this, mgrIn, 1 ), 
    out ( *this, mgrIn ),
    pUserName ( 0 ),
    pHostName ( 0 ),
    incommingBytesToDrain ( 0 ),
    minor_version_number ( 0 )
{
    this->pHostName = new char [1u];
    *this->pHostName = '\0';

    this->pUserName = new ( std::nothrow ) char [1u];
    if ( ! this->pUserName ) {
        free ( this->pHostName );
        throw std::bad_alloc();
    }
    *this->pUserName= '\0';
}

//
// casStrmClient::~casStrmClient ()
//
casStrmClient::~casStrmClient ()
{
    while ( casChannelI * pChan = this->chanList.get() ) {
        pChan->uninstallFromPV ( this->eventSys );
        this->chanTable.remove ( *pChan );
        delete pChan;
    }
	delete [] this->pUserName;
	delete [] this->pHostName;
}

//
// casStrmClient::processMsg ()
//
caStatus casStrmClient::processMsg ()
{
    epicsGuard < casClientMutex > guard ( this->mutex );
	int status = S_cas_success;

    try {

        // drain message that does not fit
        if ( this->incommingBytesToDrain ) {
            unsigned bytesLeft = this->in.bytesPresent();
            if ( bytesLeft  < this->incommingBytesToDrain ) {
                this->in.removeMsg ( bytesLeft );
                this->incommingBytesToDrain -= bytesLeft;
                return S_cas_success;
            }
            else {
                this->in.removeMsg ( this->incommingBytesToDrain );
                this->incommingBytesToDrain = 0u;
            }
        }

	    //
	    // process any messages in the in buffer
	    //
	    unsigned bytesLeft;
	    while ( ( bytesLeft = this->in.bytesPresent() ) ) {
            caHdrLargeArray msgTmp;
            unsigned msgSize;
            ca_uint32_t hdrSize;
            char * rawMP;
            {
	            //
	            // copy as raw bytes in order to avoid
	            // alignment problems
	            //
                caHdr smallHdr;
                if ( bytesLeft < sizeof ( smallHdr ) ) {
                    break;
                }

                rawMP = this->in.msgPtr ();
	            memcpy ( & smallHdr, rawMP, sizeof ( smallHdr ) );

                ca_uint32_t payloadSize = epicsNTOH16 ( smallHdr.m_postsize );
                ca_uint32_t nElem = epicsNTOH16 ( smallHdr.m_count );
                if ( payloadSize != 0xffff && nElem != 0xffff ) {
                    hdrSize = sizeof ( smallHdr );
                }
                else {
                    ca_uint32_t LWA[2];
                    hdrSize = sizeof ( smallHdr ) + sizeof ( LWA );
                    if ( bytesLeft < hdrSize ) {
                        break;
                    }
                    //
                    // copy as raw bytes in order to avoid
                    // alignment problems
                    //
                    memcpy ( LWA, rawMP + sizeof ( caHdr ), sizeof( LWA ) );
                    payloadSize = epicsNTOH32 ( LWA[0] );
                    nElem = epicsNTOH32 ( LWA[1] );
                }

                msgTmp.m_cmmd = epicsNTOH16 ( smallHdr.m_cmmd );
                msgTmp.m_postsize = payloadSize;
                msgTmp.m_dataType = epicsNTOH16 ( smallHdr.m_dataType );
                msgTmp.m_count = nElem;
                msgTmp.m_cid = epicsNTOH32 ( smallHdr.m_cid );
                msgTmp.m_available = epicsNTOH32 ( smallHdr.m_available );


                msgSize = hdrSize + payloadSize;
                if ( bytesLeft < msgSize ) {
                    if ( msgSize > this->in.bufferSize() ) {
                        this->in.expandBuffer ();
                        // msg to large - set up message drain
                        if ( msgSize > this->in.bufferSize() ) {
                            caServerI::dumpMsg ( this->pHostName, this->pUserName, & msgTmp, 0, 
                                "The client requested transfer is greater than available " 
                                "memory in server or EPICS_CA_MAX_ARRAY_BYTES\n" );
                            status = this->sendErr ( guard, & msgTmp, invalidResID, ECA_TOLARGE, 
                                "client's request didnt fit within the CA server's message buffer" );
                            this->in.removeMsg ( bytesLeft );
                            this->incommingBytesToDrain = msgSize - bytesLeft;
                        }
                    }
                    break;
                }

                this->ctx.setMsg ( msgTmp, rawMP + hdrSize );

		        if ( this->getCAS().getDebugLevel() > 2u ) {
			        caServerI::dumpMsg ( this->pHostName, this->pUserName, 
                        & msgTmp, rawMP + hdrSize, 0 );
		        }

            }

		    //
		    // Reset the context to the default
		    // (guarantees that previous message does not get mixed 
		    // up with the current message)
		    //
		    this->ctx.setChannel ( NULL );
		    this->ctx.setPV ( NULL );

		    //
		    // Call protocol stub
		    //
            casStrmClient::pCASMsgHandler pHandler;
		    if ( msgTmp.m_cmmd < NELEMENTS ( casStrmClient::msgHandlers ) ) {
                pHandler = this->casStrmClient::msgHandlers[msgTmp.m_cmmd];
		    }
            else {
                pHandler = & casStrmClient::uknownMessageAction;
            }
		    status = ( this->*pHandler ) ( guard );
		    if ( status ) {
			    break;
		    }

            this->in.removeMsg ( msgSize );
	    }
    }
    catch ( std::bad_alloc & ) {
        status = this->sendErr ( guard,
            this->ctx.getMsg(), invalidResID, ECA_ALLOCMEM, 
            "inablility to allocate memory in "
            "the server disconnected client" );
        status = S_cas_noMemory;
    }
    catch ( std::exception & except ) {
		status = this->sendErr ( guard,
            this->ctx.getMsg(), invalidResID, ECA_INTERNAL, 
            "C++ exception \"%s\" in server "
            "diconnected client",
            except.what () );
        status = S_cas_internal;
    }
    catch (...) {
		status = this->sendErr ( guard,
            this->ctx.getMsg(), invalidResID, ECA_INTERNAL, 
            "unexpected C++ exception in server "
            "diconnected client" );
        status = S_cas_internal;
    }

	return status;
}

//
// casStrmClient::uknownMessageAction()
//
caStatus casStrmClient::uknownMessageAction ( epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray *mp = this->ctx.getMsg();
	caStatus status;

    caServerI::dumpMsg ( this->pHostName, 
        this->pUserName, mp, this->ctx.getData(),
        "bad request code from virtual circuit=%u\n", mp->m_cmmd );

	/* 
	 *	most clients dont recover from this
	 */
	status = this->sendErr ( guard, mp, invalidResID, 
        ECA_INTERNAL, "Invalid Request Code" );
	if (status) {
		return status;
	}

	/*
	 * returning S_cas_internal here disconnects
	 * the client with the bad message
	 */
	return S_cas_internal;
}

/*
 * casStrmClient::ignoreMsgAction()
 */
caStatus casStrmClient::ignoreMsgAction ( epicsGuard < casClientMutex > & )
{
	return S_cas_success;
}

//
// versionAction()
//
caStatus casStrmClient::versionAction ( epicsGuard < casClientMutex > & )
{
#if 1
	return S_cas_success;
#else
    //
    // eventually need to set the priority here
    //
	const caHdrLargeArray * mp = this->ctx.getMsg();

    if ( mp->m_dataType > CA_PROTO_PRIORITY_MAX ) {
        return S_cas_badProtocol;
    }

    double tmp = mp->m_dataType - CA_PROTO_PRIORITY_MIN;
    tmp *= epicsThreadPriorityCAServerHigh - epicsThreadPriorityCAServerLow;
    tmp /= CA_PROTO_PRIORITY_MAX - CA_PROTO_PRIORITY_MIN;
    tmp += epicsThreadPriorityCAServerLow;
    unsigned epicsPriorityNew = (unsigned) tmp;
    unsigned epicsPrioritySelf = epicsThreadGetPrioritySelf();
    if ( epicsPriorityNew != epicsPrioritySelf ) {
        epicsThreadBooleanStatus tbs;
        unsigned priorityOfEvents;
        tbs  = epicsThreadHighestPriorityLevelBelow ( epicsPriorityNew, &priorityOfEvents );
        if ( tbs != epicsThreadBooleanStatusSuccess ) {
            priorityOfEvents = epicsPriorityNew;
        }

        if ( epicsPriorityNew > epicsPrioritySelf ) {
            epicsThreadSetPriority ( epicsThreadGetIdSelf(), epicsPriorityNew );
            db_event_change_priority ( client->evuser, priorityOfEvents );
        }
        else {
            db_event_change_priority ( client->evuser, priorityOfEvents );
            epicsThreadSetPriority ( epicsThreadGetIdSelf(), epicsPriorityNew );
        }
        client->priority = mp->m_dataType;
    }
    return S_cas_success;
#endif
}

//
// echoAction()
//
caStatus casStrmClient::echoAction ( epicsGuard < casClientMutex > & )
{
	const caHdrLargeArray * mp = this->ctx.getMsg();
	const void * dp = this->ctx.getData();
    void * pPayloadOut;

    caStatus status = this->out.copyInHeader ( mp->m_cmmd, mp->m_postsize, 
        mp->m_dataType, mp->m_count, mp->m_cid, mp->m_available,
        & pPayloadOut );
    if ( ! status ) {
        memcpy ( pPayloadOut, dp, mp->m_postsize );
        this->out.commitMsg ();
    }
	return S_cas_success;
}

//
// casStrmClient::verifyRequest()
//
caStatus casStrmClient::verifyRequest ( casChannelI * & pChan )
{
	const caHdrLargeArray * mp = this->ctx.getMsg();

	//
	// channel exists for this resource id ?
	//
    chronIntId tmpId ( mp->m_cid );
	pChan = this->chanTable.lookup ( tmpId );
	if ( ! pChan ) {
		return ECA_BADCHID;
	}

	//
	// data type out of range ?
	//
	if ( mp->m_dataType > ((unsigned)LAST_BUFFER_TYPE) ) {
		return ECA_BADTYPE;
	}

	//
	// element count out of range ?
	//
	if ( mp->m_count > pChan->getPVI().nativeCount() || mp->m_count == 0u ) {
		return ECA_BADCOUNT;
	}

	this->ctx.setChannel ( pChan );
	this->ctx.setPV ( &pChan->getPVI() );

	return ECA_NORMAL;
}

void casStrmClient::show ( unsigned level ) const
{
    epicsGuard < epicsMutex > locker ( this->mutex );
	printf ( "casStrmClient at %p\n", 
        static_cast <const void *> ( this ) );
	if ( level > 1u ) {
		printf ("\tuser %s at %s\n", this->pUserName, this->pHostName);
	    this->casCoreClient::show ( level - 1 );
	    this->in.show ( level - 1 );
	    this->out.show ( level - 1 );
        this->chanTable.show ( level - 1 );
	}
}

/*
 * casStrmClient::readAction()
 */
caStatus casStrmClient::readAction ( epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray * mp = this->ctx.getMsg();
	caStatus status;
	casChannelI * pChan;
	const gdd * pDesc;

	status = this->verifyRequest ( pChan );
	if ( status != ECA_NORMAL ) {
        if ( pChan ) {
		    return this->sendErr ( guard, mp, pChan->getCID(), 
                status, "get request" );
        }
        else {
		    return this->sendErr ( guard, mp, invalidResID, 
                status, "get request" );
        }
	}

	/*
	 * verify read access
	 */
	if ( ! pChan->readAccess() ) {
		int	v41;

		v41 = CA_V41 ( this->minor_version_number );
		if ( v41 ) {
			status = ECA_NORDACCESS;
		}
		else{
			status = ECA_GETFAIL;
		}

		return this->sendErr ( guard, mp, pChan->getCID(), 
            status, "read access denied" );
	}

	status = this->read ( pDesc ); 
	if ( status == S_casApp_success ) {
		status = this->readResponse ( guard, pChan, *mp, *pDesc, S_cas_success );
	}
	else if ( status == S_casApp_asyncCompletion ) {
		status = S_cas_success;
	}
	else if ( status == S_casApp_postponeAsyncIO ) {
		pChan->getPVI().addItemToIOBLockedList ( *this );
	}
	else {
		status = this->sendErrWithEpicsStatus ( guard, mp, 
            pChan->getCID(), status, ECA_GETFAIL );
	}

    if ( pDesc ) {
        pDesc->unreference ();
    }

	return status;
}

//
// casStrmClient::readResponse()
//
caStatus casStrmClient::readResponse ( epicsGuard < casClientMutex > & guard,
                    casChannelI * pChan, const caHdrLargeArray & msg, 
					const gdd & desc, const caStatus status )
{
	if ( status != S_casApp_success ) {
		return this->sendErrWithEpicsStatus ( guard, & msg, 
            pChan->getCID(), status, ECA_GETFAIL );
	}

    void * pPayload;
    {
	    unsigned payloadSize = dbr_size_n ( msg.m_dataType, msg.m_count );
        caStatus localStatus = this->out.copyInHeader ( msg.m_cmmd, payloadSize,
            msg.m_dataType, msg.m_count, pChan->getCID (), 
            msg.m_available, & pPayload );
	    if ( localStatus ) {
		    if ( localStatus==S_cas_hugeRequest ) {
			    localStatus = sendErr ( guard, & msg, pChan->getCID(), ECA_TOLARGE, 
                    "unable to fit read response into server's buffer" );
		    }
		    return localStatus;
	    }
    }

	//
	// convert gdd to db_access type
	// (places the data in network format)
	//
	int mapDBRStatus = gddMapDbr[msg.m_dataType].conv_dbr(
        pPayload, msg.m_count, desc, pChan->enumStringTable() );
	if ( mapDBRStatus < 0 ) {
		desc.dump ();
		errPrintf ( S_cas_badBounds, __FILE__, __LINE__, "- get with PV=%s type=%u count=%u",
				pChan->getPVI().getName(), msg.m_dataType, msg.m_count );
		return this->sendErrWithEpicsStatus ( 
            guard, & msg, pChan->getCID(), S_cas_badBounds, ECA_GETFAIL );
	}
#ifdef CONVERSION_REQUIRED
	( * cac_dbr_cvrt[msg.m_dataType] )
		( pPayload, pPayload, true, msg.m_count );
#endif
    if ( msg.m_dataType == DBR_STRING && msg.m_count == 1u ) {
		unsigned reducedPayloadSize = strlen ( static_cast < char * > ( pPayload ) ) + 1u;
	    this->out.commitMsg ( reducedPayloadSize );
	}
    else {
	    this->out.commitMsg ();
    }

	return S_cas_success;
}

//
// casStrmClient::readNotifyAction()
//
caStatus casStrmClient::readNotifyAction ( epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray * mp = this->ctx.getMsg();
	casChannelI * pChan;
	const gdd * pDesc;
	int status;

	status = this->verifyRequest ( pChan );
	if ( status != ECA_NORMAL ) {
		return this->readNotifyFailureResponse ( guard, * mp, status );
	}

	//
	// verify read access
	// 
	if ( ! pChan->readAccess() ) {
		return this->readNotifyFailureResponse ( guard, *mp, ECA_NORDACCESS );
	}

	status = this->read ( pDesc ); 
	if ( status == S_casApp_success ) {
		status = this->readNotifyResponse ( guard, pChan, *mp, *pDesc, status );
	}
	else if ( status == S_casApp_asyncCompletion ) {
		status = S_cas_success;
	}
	else if ( status == S_casApp_postponeAsyncIO ) {
		pChan->getPVI().addItemToIOBLockedList ( *this );
	}
	else {
		status = this->readNotifyResponse ( guard, pChan, *mp, *pDesc, status );
	}

    if ( pDesc ) {
        pDesc->unreference ();
    }

	return status;
}

//
// casStrmClient::readNotifyResponse()
//
caStatus casStrmClient::readNotifyResponse ( epicsGuard < casClientMutex > & guard, 
        casChannelI * pChan, const caHdrLargeArray & msg, const gdd & desc, 
        const caStatus completionStatus )
{
	if ( completionStatus != S_cas_success ) {
        caStatus ecaStatus =  this->readNotifyFailureResponse ( guard, msg, ECA_GETFAIL );
    	//
	    // send independent warning exception to the client so that they
	    // will see the error string associated with this error code 
	    // since the error string cant be sent with the get call back 
	    // response (hopefully this is useful information)
	    //
	    // order is very important here because it determines that the get 
	    // call back response is always sent, and that this warning exception
	    // message will be sent at most one time (in rare instances it will
	    // not be sent, but at least it will not be sent multiple times).
	    // The message is logged to the console in the rare situations when
	    // we are unable to send.
	    //
		caStatus tmpStatus = this->sendErrWithEpicsStatus ( guard, & msg, pChan->getCID(),
                    completionStatus, ECA_NOCONVERT );
		if ( tmpStatus ) {
			errMessage ( completionStatus, "<= get callback failure detail not passed to client" );
		}
        return ecaStatus;
	}

    void *pPayload;
    {
	    unsigned size = dbr_size_n ( msg.m_dataType, msg.m_count );
        caStatus status = this->out.copyInHeader ( msg.m_cmmd, size,
                    msg.m_dataType, msg.m_count, ECA_NORMAL, 
                    msg.m_available, & pPayload );
	    if ( status ) {
		    if ( status == S_cas_hugeRequest ) {
			    status = sendErr ( guard, & msg, pChan->getCID(), ECA_TOLARGE, 
                    "unable to fit read notify response into server's buffer" );
		    }
		    return status;
	    }
    }

    //
	// convert gdd to db_access type
	//
	int mapDBRStatus = gddMapDbr[msg.m_dataType].conv_dbr ( pPayload, 
        msg.m_count, desc, pChan->enumStringTable() );
	if ( mapDBRStatus < 0 ) {
		desc.dump();
		errPrintf ( S_cas_badBounds, __FILE__, __LINE__, 
            "- get notify with PV=%s type=%u count=%u",
			pChan->getPVI().getName(), msg.m_dataType, msg.m_count );
        return this->readNotifyFailureResponse ( guard, msg, ECA_NOCONVERT );
	}

#ifdef CONVERSION_REQUIRED
	( * cac_dbr_cvrt[ msg.m_dataType ] )
		( pPayload, pPayload, true, msg.m_count );
#endif

	if ( msg.m_dataType == DBR_STRING && msg.m_count == 1u ) {
		unsigned reducedPayloadSize = strlen ( static_cast < char * > ( pPayload ) ) + 1u;
	    this->out.commitMsg ( reducedPayloadSize );
	}
    else {
	    this->out.commitMsg ();
    }

	return S_cas_success;
}

//
// casStrmClient::readNotifyFailureResponse ()
//
caStatus casStrmClient::readNotifyFailureResponse ( 
    epicsGuard < casClientMutex > &, const caHdrLargeArray & msg, const caStatus ECA_XXXX )
{
    assert ( ECA_XXXX != ECA_NORMAL );
    void *pPayload;
	unsigned size = dbr_size_n ( msg.m_dataType, msg.m_count );
    caStatus status = this->out.copyInHeader ( msg.m_cmmd, size,
                msg.m_dataType, msg.m_count, ECA_XXXX, 
                msg.m_available, & pPayload );
	if ( ! status ) {
	    memset ( pPayload, '\0', size );
	}
    return status;
}

//
// set bounds on an application type within a container, but dont 
// preallocate space (not preallocating buffer space allows gdd::put 
// to be more efficent if it discovers that the source has less data 
// than the destination)
//
bool convertContainerMemberToAtomic ( gdd & dd, 
         aitUint32 appType, aitUint32 elemCount )
{
    if ( elemCount <= 1 ) {
        return true;
    }

    gdd * pVal;
    if ( dd.isContainer() ) {
 	    // All DBR types have a value member 
        aitUint32 valIndex;
 	    int gdds = gddApplicationTypeTable::app_table.mapAppToIndex
 		    ( dd.applicationType(), appType, valIndex );
 	    if ( gdds ) {
 		    return false;
 	    }

 	    pVal = dd.getDD ( valIndex );
 	    if ( ! pVal ) {
 		    return false;
 	    }
    }
    else {
        if ( appType != dd.applicationType() ) {
            return false;
        }
        pVal = & dd;
    }

    // we cant changed a managed type that is 
    // already atomic (array)
    if ( ! pVal->isScalar () ) {
        return false;
    }
        
 	// convert to atomic
 	gddBounds bds;
 	bds.setSize ( elemCount );
 	bds.setFirst ( 0u );
 	pVal->setDimension ( 1u, & bds );
    return true;
}

//
// createDBRDD ()
//
static gdd * createDBRDD ( unsigned dbrType, unsigned elemCount )
{	
	/*
	 * DBR type has already been checked, but it is possible
	 * that "gddDbrToAit" will not track with changes in
	 * the DBR_XXXX type system
	 */
	if ( dbrType >= NELEMENTS ( gddDbrToAit ) ) {
		return 0;
	}

	if ( gddDbrToAit[dbrType].type == aitEnumInvalid ) {
		return 0;
	}

	aitUint16 appType = gddDbrToAit[dbrType].app;
	
	//
	// create the descriptor
	//
	gdd * pDescRet = 
        gddApplicationTypeTable::app_table.getDD ( appType );
	if ( ! pDescRet ) {
		return pDescRet;
	}

    // fix the value element count
    bool success = convertContainerMemberToAtomic ( 
        *pDescRet, gddAppType_value, elemCount );
    if ( ! success ) {
 		return NULL;
    }

    // fix the enum string table element count
    // (this is done here because the application type table in gdd 
    // does not appear to handle this correctly)
    if ( dbrType == DBR_CTRL_ENUM || dbrType == DBR_GR_ENUM ) {
        bool tmpSuccess = convertContainerMemberToAtomic ( 
            *pDescRet, gddAppType_enums, MAX_ENUM_STATES );
        if ( ! tmpSuccess ) {
 		    return NULL;
        }
    }

	return pDescRet;
}

//
// casStrmClient::monitorFailureResponse ()
//
caStatus casStrmClient::monitorFailureResponse ( 
    epicsGuard < casClientMutex > &, const caHdrLargeArray & msg, 
    const caStatus ECA_XXXX )
{
    assert ( ECA_XXXX != ECA_NORMAL );
    void *pPayload;
	unsigned size = dbr_size_n ( msg.m_dataType, msg.m_count );
    caStatus status = this->out.copyInHeader ( msg.m_cmmd, size,
                msg.m_dataType, msg.m_count, ECA_XXXX, 
                msg.m_available, & pPayload );
	if ( ! status ) {
	    memset ( pPayload, '\0', size );
        this->out.commitMsg ();
	}
    return status;
}

//
// casStrmClient::monitorResponse ()
//
caStatus casStrmClient::monitorResponse ( 
    epicsGuard < casClientMutex > & guard,
    casChannelI & chan, const caHdrLargeArray & msg, 
	const gdd & desc, const caStatus completionStatus )
{
    void * pPayload = 0;
    {
	    ca_uint32_t size = dbr_size_n ( msg.m_dataType, msg.m_count );
        caStatus status = out.copyInHeader ( msg.m_cmmd, size,
            msg.m_dataType, msg.m_count, ECA_NORMAL, 
            msg.m_available, & pPayload );
	    if ( status ) {
		    if ( status == S_cas_hugeRequest ) {
			    status = sendErr ( guard, & msg, chan.getCID(), ECA_TOLARGE, 
                    "unable to fit read subscription update response "
                    "into server's buffer" );
		    }
		    return status;
	    }
    }

	if ( ! chan.readAccess () ) {
        return monitorFailureResponse ( guard, msg, ECA_NORDACCESS );
	}

    gdd * pDBRDD = 0;
	if ( completionStatus == S_cas_success ) {
	    pDBRDD = createDBRDD ( msg.m_dataType, msg.m_count );
        if ( ! pDBRDD ) {
            return monitorFailureResponse ( guard, msg, ECA_ALLOCMEM );
        }
	    else {
	        gddStatus gdds = gddApplicationTypeTable::
		        app_table.smartCopy ( pDBRDD, & desc );
	        if ( gdds < 0 ) {
                pDBRDD->unreference ();
		        errPrintf ( S_cas_noConvert, __FILE__, __LINE__,
        "no conversion between event app type=%d and DBR type=%d Element count=%d",
			        desc.applicationType (), msg.m_dataType, msg.m_count);
                return monitorFailureResponse ( guard, msg, ECA_NOCONVERT );
            }
        }
	}
	else {
		errMessage ( completionStatus, "- in monitor response" );
		if ( completionStatus == S_cas_noRead ) {
            return monitorFailureResponse ( guard, msg, ECA_NORDACCESS );
		}
		else if ( completionStatus == S_cas_noMemory ) {
            return monitorFailureResponse ( guard, msg, ECA_ALLOCMEM );
		}
		else {
            return monitorFailureResponse ( guard, msg, ECA_GETFAIL );
		}
	}

	int mapDBRStatus = gddMapDbr[msg.m_dataType].conv_dbr ( pPayload, msg.m_count, 
        *pDBRDD, chan.enumStringTable() );
    if ( mapDBRStatus < 0 ) {
        pDBRDD->unreference ();
        return monitorFailureResponse ( guard, msg, ECA_NOCONVERT );
    }

#ifdef CONVERSION_REQUIRED
	/* use type as index into conversion jumptable */
	(* cac_dbr_cvrt[msg.m_dataType])
		( pPayload, pPayload, true,  msg.m_count );
#endif

	//
	// force string message size to be the true size 
	//
	if ( msg.m_dataType == DBR_STRING && msg.m_count == 1u ) {
		ca_uint32_t reducedPayloadSize = strlen ( static_cast < char * > ( pPayload ) ) + 1u;
	    this->out.commitMsg ( reducedPayloadSize );
	}
    else {
	    this->out.commitMsg ();
    }

    pDBRDD->unreference ();

	return S_cas_success;
}

/*
 * casStrmClient::writeAction()
 */
caStatus casStrmClient::writeAction ( epicsGuard < casClientMutex > & guard )
{	
	const caHdrLargeArray *mp = this->ctx.getMsg();
	caStatus status;
	casChannelI	*pChan;

	status = this->verifyRequest ( pChan );
	if (status != ECA_NORMAL) {
        if ( pChan ) {
		    return this->sendErr ( guard, mp, pChan->getCID(), 
                status, "get request" );
        }
        else {
		    return this->sendErr ( guard, mp, invalidResID, 
                status, "get request" );
        }
	}

	//
	// verify write access
	// 
	if ( ! pChan->writeAccess() ) {
		int	v41;

		v41 = CA_V41 ( this->minor_version_number );
		if (v41) {
			status = ECA_NOWTACCESS;
		}
		else{
			status = ECA_PUTFAIL;
		}

		return this->sendErr ( guard, mp, pChan->getCID(),
            status, "write access denied");
	}

	//
	// initiate the  write operation
	//
	status = this->write (); 
	if ( status == S_casApp_success || status == S_casApp_asyncCompletion ) {
		status = S_cas_success;
	}
	else if ( status == S_casApp_postponeAsyncIO ) {
		pChan->getPVI().addItemToIOBLockedList ( *this );
	}
	else {
		status = this->sendErrWithEpicsStatus ( guard, mp, pChan->getCID(),
                    status, ECA_PUTFAIL );
		//
		// I have assumed that the server tool has deleted the gdd here
		//
	}

	//
	// The gdd created above is deleted by the server tool 
	//
	return status;
}

//
// casStrmClient::writeResponse()
//
caStatus casStrmClient::writeResponse (
    epicsGuard < casClientMutex > & guard, casChannelI & chan,
	const caHdrLargeArray & msg, const caStatus completionStatus )
{
	caStatus status;

	if ( completionStatus ) {
		errMessage ( completionStatus, NULL );
		status = this->sendErrWithEpicsStatus ( guard, & msg, 
				chan.getCID(), completionStatus, ECA_PUTFAIL );
	}
	else {
		status = S_cas_success;
	}

	return status;
}

/*
 * casStrmClient::writeNotifyAction()
 */
caStatus casStrmClient::writeNotifyAction ( 
    epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray *mp = this->ctx.getMsg ();

	casChannelI	*pChan;
	int status = this->verifyRequest ( pChan );
	if ( status != ECA_NORMAL ) {
		return casStrmClient::writeNotifyResponseECA_XXX ( guard, *mp, status );
	}

	//
	// verify write access
	// 
	if ( ! pChan->writeAccess() ) {
		if ( CA_V41(this->minor_version_number) ) {
			return this->casStrmClient::writeNotifyResponseECA_XXX (
					guard, *mp, ECA_NOWTACCESS);
		}
		else {
			return this->casStrmClient::writeNotifyResponse (
					guard, *pChan, *mp, S_cas_noWrite );
		}
	}

	//
	// initiate the  write operation
	//
	status = this->write(); 
	if (status == S_casApp_asyncCompletion) {
		status = S_cas_success;
	}
	else if (status==S_casApp_postponeAsyncIO) {
		pChan->getPVI().addItemToIOBLockedList(*this);
	}
	else {
		status = casStrmClient::writeNotifyResponse ( guard, *pChan, *mp, status );
	}

	return status;
}

/* 
 * casStrmClient::writeNotifyResponse()
 */
caStatus casStrmClient::writeNotifyResponse ( epicsGuard < casClientMutex > & guard,
        casChannelI & chan, const caHdrLargeArray & msg, const caStatus completionStatus )
{
	caStatus ecaStatus;

	if ( completionStatus == S_cas_success ) {
		ecaStatus = ECA_NORMAL;
	}
	else {
		ecaStatus = ECA_PUTFAIL;	
	}

	ecaStatus = this->casStrmClient::writeNotifyResponseECA_XXX ( 
        guard, msg, ecaStatus );
	if (ecaStatus) {
		return ecaStatus;
	}

	//
	// send independent warning exception to the client so that they
	// will see the error string associated with this error code 
	// since the error string cant be sent with the put call back 
	// response (hopefully this is useful information)
	//
	// order is very important here because it determines that the put 
	// call back response is always sent, and that this warning exception
	// message will be sent at most one time. In rare instances it will
	// not be sent, but at least it will not be sent multiple times.
	// The message is logged to the console in the rare situations when
	// we are unable to send.
	//
	if ( completionStatus != S_cas_success ) {
		ecaStatus = this->sendErrWithEpicsStatus ( guard, & msg, chan.getCID(),
                        completionStatus, ECA_NOCONVERT );
		if ( ecaStatus ) {
			errMessage ( completionStatus, 
                "<= put callback failure detail not passed to client" );
		}
	}
	return S_cas_success;
}

/* 
 * casStrmClient::writeNotifyResponseECA_XXX()
 */
caStatus casStrmClient::writeNotifyResponseECA_XXX (
	epicsGuard < casClientMutex > &, 
    const caHdrLargeArray & msg, const caStatus ecaStatus )
{
    caStatus status = out.copyInHeader ( msg.m_cmmd, 0,
        msg.m_dataType, msg.m_count, ecaStatus, 
        msg.m_available, 0 );
	if ( ! status ) {
    	this->out.commitMsg ();
	}

	return status;
}

/*
 * casStrmClient::hostNameAction()
 */
caStatus casStrmClient::hostNameAction ( epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray *mp = this->ctx.getMsg();
	char 			*pName = (char *) this->ctx.getData();
	unsigned		size;
	char 			*pMalloc;
	caStatus		status;

    // currently this has to occur prior to 
    // creating channels or its not allowed
    if ( this->chanList.count () ) {
		return this->sendErr ( guard, mp, invalidResID, 
                        ECA_UNAVAILINSERV, pName );
    }

	size = strlen(pName)+1u;
	/*
	 * user name will not change if there isnt enough memory
	 */
	pMalloc = new char [size];
	if ( ! pMalloc ){
		status = this->sendErr ( guard, mp, invalidResID, 
                        ECA_ALLOCMEM, pName );
		if (status) {
			return status;
		}
		return S_cas_internal;
	}
	strncpy ( pMalloc, pName, size - 1 );
	pMalloc[ size - 1 ]='\0';

	if ( this->pHostName ) {
		delete [] this->pHostName;
	}
	this->pHostName = pMalloc;

	return S_cas_success;
}

/*
 * casStrmClient::clientNameAction()
 */
caStatus casStrmClient::clientNameAction ( 
    epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray *mp = this->ctx.getMsg();
	char 			*pName = (char *) this->ctx.getData();
	unsigned		size;
	char 			*pMalloc;
	caStatus		status;

    // currently this has to occur prior to 
    // creating channels or its not allowed
    if ( this->chanList.count () ) {
		return this->sendErr ( guard, mp, invalidResID, 
                        ECA_UNAVAILINSERV, pName );
    }

	size = strlen(pName)+1;

	/*
	 * user name will not change if there isnt enough memory
	 */
	pMalloc = new char [size];
	if(!pMalloc){
		status = this->sendErr ( guard, mp, invalidResID, 
                    ECA_ALLOCMEM, pName );
		if (status) {
			return status;
		}
		return S_cas_internal;
	}
	strncpy ( pMalloc, pName, size - 1 );
	pMalloc[size-1]='\0';

	if ( this->pUserName ) {
		delete [] this->pUserName;
	}
	this->pUserName = pMalloc;

	return S_cas_success;
}

/*
 * casStrmClientMon::claimChannelAction()
 */
caStatus casStrmClient::claimChannelAction ( 
    epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray * mp = this->ctx.getMsg();
	char *pName = (char *) this->ctx.getData();
	caServerI & cas = *this->ctx.getServer();
	caStatus status;

	/*
	 * The available field is used (abused)
	 * here to communicate the miner version number
	 * starting with CA 4.1. The field was set to zero
	 * prior to 4.1
	 */
    if ( mp->m_available < 0xffff ) {
	    this->minor_version_number = 
            static_cast < ca_uint16_t > ( mp->m_available );
    }
    else {
	    this->minor_version_number = 0;
    }

	//
	// We shouldnt be receiving a connect message from 
	// an R3.11 client because we will not respond to their
	// search requests (if so we disconnect)
	//
	if ( ! CA_V44 ( this->minor_version_number ) ) {
		//
		// old connect protocol was dropped when the
		// new API was added to the server (they must
		// now use clients at EPICS 3.12 or higher)
		//
		status = this->sendErr ( guard, mp, mp->m_cid, ECA_DEFUNCT,
				"R3.11 connect sequence from old client was ignored");
		if ( status ) {
			return status;
		}
		return S_cas_badProtocol; // disconnect client
	}

	if ( mp->m_postsize <= 1u ) {
		return S_cas_badProtocol; // disconnect client
	}

    pName[mp->m_postsize-1u] = '\0';

	if ( ( mp->m_postsize - 1u ) > unreasonablePVNameSize ) {
		return S_cas_badProtocol; // disconnect client
	}

	this->userStartedAsyncIO = false;

	//
	// attach to the PV
	//
	pvAttachReturn pvar = cas->pvAttach ( this->ctx, pName );

	//
	// prevent problems when they initiate
	// async IO but dont return status
	// indicating so (and vise versa)
	//
	if ( this->userStartedAsyncIO ) {
		if ( pvar.getStatus() != S_casApp_asyncCompletion ) {
			fprintf ( stderr, 
                "Application returned %d from cas::pvAttach()"
                " - expected S_casApp_asyncCompletion\n",  
                pvar.getStatus() );
		}
		status = S_cas_success;	
	}
	else if ( pvar.getStatus() == S_casApp_asyncCompletion ) {
		status = this->createChanResponse ( guard, *mp, S_cas_badParameter );
		errMessage ( S_cas_badParameter, 
		"- expected asynch IO creation from caServer::pvAttach()" );
	}
	else if ( pvar.getStatus() == S_casApp_postponeAsyncIO ) {
		status = S_casApp_postponeAsyncIO;
		this->ctx.getServer()->addItemToIOBLockedList ( *this );
	}
	else {
		status = this->createChanResponse ( guard, *mp, pvar );
	}
	return status;
}

//
// casStrmClient::createChanResponse()
//
caStatus casStrmClient::createChanResponse ( 
    epicsGuard < casClientMutex > & guard,
    const caHdrLargeArray & hdr, 
    const pvAttachReturn & pvar )
{
	if ( pvar.getStatus() != S_cas_success ) {
		return this->channelCreateFailedResp ( guard, 
            hdr, pvar.getStatus() );
	}

    if ( ! pvar.getPV()->pPVI ) {
        // @#$!* Tornado 2 Cygnus GNU compiler bugs
#       if ! defined (__GNUC__) || __GNUC__ > 2 || ( __GNUC__ == 2 && __GNUC_MINOR__ >= 92 )
            pvar.getPV()->pPVI = new ( std::nothrow ) // X aCC 930
                        casPVI ( *pvar.getPV() );
#       else
            try {
                pvar.getPV()->pPVI = new  
                            casPVI ( *pvar.getPV() );
            }
            catch ( ... ) {
                pvar.getPV()->pPVI = 0;
            }
#       endif

        if ( ! pvar.getPV()->pPVI ) {
            pvar.getPV()->destroyRequest ();
		    return this->channelCreateFailedResp ( guard, hdr, S_casApp_pvNotFound );
        }
    }

    unsigned nativeTypeDBR;
	caStatus status = pvar.getPV()->pPVI->bestDBRType ( nativeTypeDBR );
	if ( status ) {
		pvar.getPV()->pPVI->deleteSignal();
		errMessage ( status, "best external dbr type fetch failed" );
		return this->channelCreateFailedResp ( guard, hdr, status );
	}

    //
	// attach the PV to this server
	//
	status = pvar.getPV()->pPVI->attachToServer ( this->getCAS() );
	if ( status ) {
		pvar.getPV()->pPVI->deleteSignal();
		return this->channelCreateFailedResp ( guard, hdr, status );
	}

	//
	// create server tool XXX derived from casChannel
    // (use temp context because this can be called asynchronously)
	//
	casChannel * pChan = pvar.getPV()->pPVI->createChannel ( 
        this->ctx, this->pUserName, this->pHostName );
	if ( ! pChan ) {
		return this->channelCreateFailedResp ( 
            guard, hdr, S_cas_noMemory );
	}

    if ( ! pChan->pChanI ) {
        // @#$!* Tornado 2 Cygnus GNU compiler bugs
#       if ! defined (__GNUC__) || __GNUC__ > 2 || ( __GNUC__ == 2 && __GNUC_MINOR__ >= 92 )
            pChan->pChanI = new ( std::nothrow ) // X aCC 930
                casChannelI ( * this, *pChan, 
                    * pvar.getPV()->pPVI, hdr.m_cid );
#       else
            try {
                pChan->pChanI = new 
                    casChannelI ( * this, *pChan, 
                        * pvar.getPV()->pPVI, hdr.m_cid );
            }
            catch ( ... ) {
                pChan->pChanI = 0;
            }
#       endif

        if ( ! pChan->pChanI ) {
            pChan->destroyRequest ();
		    pChan->getPV()->pPVI->deleteSignal ();
		    return this->channelCreateFailedResp ( 
                guard, hdr, S_cas_noMemory );
        }
    }

    //
    // check to see if the enum table is empty and therefore
    // an update is needed every time that a PV attaches 
    // to the server in case the client disconnected before 
    // an asynchronous IO to get the table completed
    //
    if ( nativeTypeDBR == DBR_ENUM ) {
        this->ctx.setPV ( pvar.getPV()->pPVI );
        this->ctx.setChannel ( pChan->pChanI );
        this->userStartedAsyncIO = false;
        status = pvar.getPV()->pPVI->updateEnumStringTable ( this->ctx );
	    if ( this->userStartedAsyncIO ) {
		    if ( status != S_casApp_asyncCompletion ) {
			    fprintf ( stderr, 
                    "Application returned %d from casPV::read()"
                    " - expected S_casApp_asyncCompletion\n", status);
		    }
			status = S_cas_success;
	    }
        else if ( status == S_casApp_success ) {
            status = enumPostponedCreateChanResponse ( 
                    guard, * pChan->pChanI, hdr, nativeTypeDBR );
        }
	    else if ( status == S_casApp_asyncCompletion )  {
		    status = S_cas_badParameter;
		    errMessage ( status, 
		        "- asynch IO creation status returned, but async IO not started?");
	    }
        else if ( status == S_casApp_postponeAsyncIO ) {
		    errlogPrintf ( "The server library does not currently support postponment of " );
            errlogPrintf ( "string table cache update of casPV::read()." );
		    errlogPrintf ( "To postpone this request please postpone the PC attach IO request." );
		    errlogPrintf ( "String table cache update did not occur." );
            status = enumPostponedCreateChanResponse ( 
                guard, * pChan->pChanI, hdr, nativeTypeDBR );
        }
    }
    else {
        status = enumPostponedCreateChanResponse ( 
            guard, * pChan->pChanI, hdr, nativeTypeDBR );
    }
  
    if ( status != S_cas_success ) {
        delete ctx.getChannel();
    }

    return status;
}

//
// casStrmClient::enumPostponedCreateChanResponse()
//
// LOCK must be applied
//
caStatus casStrmClient::enumPostponedCreateChanResponse ( 
    epicsGuard < casClientMutex > & guard,
    casChannelI & chan, const caHdrLargeArray & hdr, unsigned nativeTypeDBR )
{
	//
	// We are allocating enough space for both the claim
	// response and the access rights response so that we know for
	// certain that they will both be sent together.
	//
    // Considering the possibility of large arrays we must allocate
    // an additional 2 * sizeof(ca_uint32_t)
    //
    void *pRaw;
    const outBufCtx outctx = this->out.pushCtx 
                    ( 0, 2 * sizeof ( caHdr ) + 2 * sizeof(ca_uint32_t), pRaw );
    if ( outctx.pushResult() != outBufCtx::pushCtxSuccess ) {
        return S_cas_sendBlocked;
    }

	//
	// We are certain that the request will complete
	// here because we allocated enough space for this
	// and the claim response above.
	//
	caStatus status = this->accessRightsResponse ( guard, & chan );
	if ( status ) {
        this->out.popCtx ( outctx );
		errMessage ( status, "incomplete channel create?" );
		status = this->channelCreateFailedResp ( guard, hdr, status );
        if ( status == S_cas_success ) {
		    delete & chan;
        }
        return status;
	}

    // must install into server table before using server id
    // member of channel
    this->chanTable.add ( chan );
    this->chanList.add ( chan );
    chan.installIntoPV ();

	//
	// We are allocated enough space for both the claim
	// response and the access response so that we know for
	// certain that they will both be sent together.
	// Nevertheles, some (old) clients do not receive
	// an access rights response so we allocate again
	// here to be certain that we are at the correct place in
	// the protocol buffer.
	//
	assert ( nativeTypeDBR <= 0xffff );
	aitIndex nativeCount = chan.getPVI().nativeCount();
	assert ( nativeCount <= 0xffffffff );
    status = this->out.copyInHeader ( CA_PROTO_CLAIM_CIU, 0,
        static_cast <ca_uint16_t> ( nativeTypeDBR ), 
        static_cast <ca_uint32_t> ( nativeCount ), 
        hdr.m_cid, chan.getSID(), 0 );
    if ( status != S_cas_success ) {

        this->out.popCtx ( outctx );
		errMessage ( status, "incomplete channel create?" );
		status = this->channelCreateFailedResp ( guard, hdr, status );
        if ( status == S_cas_success ) {
            this->chanTable.remove ( chan );
            this->chanList.remove ( chan );
            chan.uninstallFromPV ( this->eventSys );
		    delete & chan;
        }
        return status;
    }

    this->out.commitMsg ();

    //
    // commit the message
    //
    bufSizeT nBytes = this->out.popCtx ( outctx );
    assert ( 
        nBytes == 2 * sizeof ( caHdr ) || 
        nBytes == 2 * sizeof ( caHdr ) + 2 * sizeof ( ca_uint32_t ) );
    this->out.commitRawMsg ( nBytes );

	return status;
}

/*
 * casStrmClient::channelCreateFailed()
 */
caStatus casStrmClient::channelCreateFailedResp ( 
    epicsGuard < casClientMutex > & guard, const caHdrLargeArray & hdr, 
    const caStatus createStatus )
{
	if ( createStatus == S_casApp_asyncCompletion ) {
		errMessage( S_cas_badParameter, 
	        "- no asynchronous IO create in pvAttach() ?");
		errMessage( S_cas_badParameter, 
	        "- or S_casApp_asyncCompletion was "
            "async IO competion code ?");
	}
	else if ( createStatus != S_casApp_pvNotFound ) {
		errMessage ( createStatus, 
            "- Server unable to create a new PV");
	}
    caStatus status;
	if ( CA_V46 ( this->minor_version_number ) ) {
        status = this->out.copyInHeader ( 
            CA_PROTO_CLAIM_CIU_FAILED, 0,
            0, 0, hdr.m_cid, 0, 0 );
		if ( status == S_cas_success ) {
		    this->out.commitMsg ();
		}
	}
	else {
		status = this->sendErrWithEpicsStatus ( 
            guard, & hdr, hdr.m_cid, createStatus, ECA_ALLOCMEM );
	}
	return status;
}

//
// casStrmClient::eventsOnAction()
//
caStatus casStrmClient::eventsOnAction ( epicsGuard < casClientMutex > & )
{
    this->enableEvents ();
	return S_cas_success;
}

//
// casStrmClient::eventsOffAction()
//
caStatus casStrmClient::eventsOffAction ( epicsGuard < casClientMutex > & )
{
    this->disableEvents ();
	return S_cas_success;
}

//
// eventAddAction()
//
caStatus casStrmClient::eventAddAction ( 
    epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray *mp = this->ctx.getMsg();
	struct mon_info *pMonInfo = (struct mon_info *) 
					this->ctx.getData();

	casChannelI *pciu;
	caStatus status = casStrmClient::verifyRequest ( pciu );
	if ( status != ECA_NORMAL ) {
        if ( pciu ) {
		    return this->sendErr ( guard, mp, 
                pciu->getCID(), status, NULL);
        }
        else {
		    return this->sendErr ( guard, mp, 
                invalidResID, status, NULL );
        }
	}

	//
	// place monitor mask in correct byte order
	//
	casEventMask mask;
	ca_uint16_t caProtoMask = epicsNTOH16 (pMonInfo->m_mask);
	if (caProtoMask&DBE_VALUE) {
		mask |= this->getCAS().valueEventMask();
	}

	if (caProtoMask&DBE_LOG) {
		mask |= this->getCAS().logEventMask();
	}
	
	if (caProtoMask&DBE_ALARM) {
		mask |= this->getCAS().alarmEventMask();
	}

	if (mask.noEventsSelected()) {
		char errStr[40];
		sprintf ( errStr, "event add req with mask=0X%X\n", caProtoMask );
		return this->sendErr ( guard, mp, pciu->getCID(), 
            ECA_BADMASK, errStr );
	}

	//
	// Attempt to read the first monitored value prior to creating
	// the monitor object so that if the server tool chooses
	// to postpone asynchronous IO we can safely restart this
	// request later.
	//
	const gdd * pDD;
	status = this->read ( pDD ); 
	//
	// always send immediate monitor response at event add
	//
    if ( status == S_casApp_asyncCompletion ) {
		status = S_cas_success;
	}
	else if ( status == S_casApp_postponeAsyncIO ) {
		//
		// try again later
		//
		pciu->getPVI().addItemToIOBLockedList ( *this );
	}
	else {
		status = this->monitorResponse ( guard, *pciu, 
                    *mp, *pDD, status );
	}

	if ( status == S_cas_success ) {
        casMonitor & mon = this->monitorFactory (
            *pciu, mp->m_available, mp->m_count, 
            mp->m_dataType, mask );
        pciu->installMonitor ( mon );
	}

    if ( pDD ) {
        pDD->unreference ();
    }

	return status;
}


//
// casStrmClient::clearChannelAction()
//
caStatus casStrmClient::clearChannelAction ( 
    epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray * mp = this->ctx.getMsg ();
	const void * dp = this->ctx.getData ();
	int status;

	// send delete confirmed message
    status = this->out.copyInHeader ( mp->m_cmmd, 0,
        mp->m_dataType, mp->m_count, 
        mp->m_cid, mp->m_available, 0 );
    if ( status ) {
        return status;
    }
    this->out.commitMsg ();

	// Verify the channel
    chronIntId tmpId ( mp->m_cid );
	casChannelI * pciu = this->chanTable.remove ( tmpId );
	if ( pciu ) {
        this->chanList.remove ( *pciu );
        pciu->uninstallFromPV ( this->eventSys );
        delete pciu;
    }
    else {
		/*
		 * it is possible that the channel delete arrives just 
		 * after the server tool has deleted the PV so we will
		 * not disconnect the client in this case. Nevertheless,
		 * we send a warning message in case either the client 
		 * or server has become corrupted
		 */
  		logBadId ( guard, mp, dp, ECA_BADCHID, mp->m_cid );
	}

	return status;
}

//
// If the channel pointer is nill this indicates that 
// the existence of the channel isnt certain because 
// it is still installed and the client or the server 
// tool might have destroyed it. Therefore, we must 
// locate it using the supplied server id.
//
// If the channel pointer isnt nill this indicates 
// that the channel has already been uninstalled.
//
// In both situations we need to send a channel 
// disconnect message to the client and destroy the 
// channel.
//
caStatus casStrmClient::channelDestroyEventNotify ( 
    epicsGuard < casClientMutex > &, 
    casChannelI * const pChan, ca_uint32_t sid )
{
    casChannelI * pChanFound;
    if ( pChan ) {
        pChanFound = pChan;
    }
    else {
        chronIntId tmpId ( sid );
        pChanFound = 
            this->chanTable.lookup ( tmpId );
	    if ( ! pChanFound ) {
            return S_cas_success;
        }
    }

    if ( CA_V47 ( this->minor_version_number ) ) {
        caStatus status = this->out.copyInHeader ( 
            CA_PROTO_SERVER_DISCONN, 0,
            0, 0, pChanFound->getCID(), 0, 0 );
        if ( status == S_cas_sendBlocked ) {
            return status;
        }
		this->out.commitMsg ();
	}
    else {
        this->forceDisconnect ();
    }

    if ( ! pChan ) {
        this->chanTable.remove ( * pChanFound );
        this->chanList.remove ( * pChanFound );
        pChanFound->uninstallFromPV ( this->eventSys );
    }

    delete pChanFound;

    return S_cas_success;
}

// casStrmClient::casChannelDestroyFromInterfaceNotify()
// immediateUninstallNeeded is false when we must avoid 
// taking the lock insituations where we would compromise 
// the lock hierarchy
void casStrmClient::casChannelDestroyFromInterfaceNotify ( 
    casChannelI & chan, bool immediateUninstallNeeded )
{
    if ( immediateUninstallNeeded ) {
        epicsGuard < casClientMutex > guard ( this->mutex );

        this->chanTable.remove ( chan );
        this->chanList.remove ( chan );
        chan.uninstallFromPV ( this->eventSys );
    }

    class channelDestroyEvent * pEvent = 
        new ( std::nothrow ) class channelDestroyEvent ( // X aCC 930
            immediateUninstallNeeded ? & chan : 0,
            chan.getSID() );
    if ( pEvent ) {
        this->addToEventQueue ( *pEvent );
    }
    else {
        this->forceDisconnect ();
        if ( immediateUninstallNeeded ) {
            delete & chan;
        }
    }
}

// casStrmClient::eventCancelAction()
caStatus casStrmClient::eventCancelAction ( 
    epicsGuard < casClientMutex > & guard )
{
	const caHdrLargeArray * mp = this->ctx.getMsg ();
	const void * dp = this->ctx.getData ();

    {
        chronIntId tmpId ( mp->m_cid );
        casChannelI * pChan = this->chanTable.lookup ( tmpId );
	    if ( ! pChan ) {
		    // It is possible that the event delete arrives just 
		    // after the server tool has deleted the PV. Its probably 
            // best to just diconnect for now since some old clients
            // may still exist.
		    logBadId ( guard, mp, dp, ECA_BADCHID, mp->m_cid );
            return S_cas_badResourceId;
        }

        caStatus status = this->out.copyInHeader ( 
            CA_PROTO_EVENT_ADD, 0,
            mp->m_dataType, mp->m_count, 
            mp->m_cid, mp->m_available, 0 );
        if ( status != S_cas_success ) {
            return status;
        }
        this->out.commitMsg ();

        casMonitor * pMon = pChan->removeMonitor ( mp->m_available );
        if ( pMon ) {
            this->eventSys.prepareMonitorForDestroy ( *pMon );
        }
        else {
		    // this indicates client or server library 
            // corruption so a disconnect is probably
            // the best option
		    logBadId ( guard, mp, dp, ECA_BADMONID, mp->m_available );
            return S_cas_badResourceId;
        }
    }

	return S_cas_success;
}

#if 0
/*
 * casStrmClient::noReadAccessEvent()
 *
 * substantial complication introduced here by the need for backwards
 * compatibility
 */
caStatus casStrmClient::noReadAccessEvent ( 
    epicsGuard < casClientMutex > & guard, casClientMon * pMon )
{
	caHdr falseReply;
	unsigned size;
	caHdr * reply;
	int status;

	size = dbr_size_n ( pMon->getType(), pMon->getCount() );

	falseReply.m_cmmd = CA_PROTO_EVENT_ADD;
	falseReply.m_postsize = size;
	falseReply.m_dataType = pMon->getType();
	falseReply.m_count = pMon->getCount();
	falseReply.m_cid = pMon->getChannel().getCID();
	falseReply.m_available = pMon->getClientId();

	status = this->allocMsg ( size, &reply );
	if ( status ) {
		if( status == S_cas_hugeRequest ) {
			return this->sendErr ( &falseReply, ECA_TOLARGE, NULL );
		}
		return status;
	}
	else{
		/*
		 * New clients recv the status of the
		 * operation directly to the
		 * event/put/get callback.
		 *
		 * Fetched value is zerod in case they
		 * use it even when the status indicates 
		 * failure.
		 *
		 * The m_cid field in the protocol
		 * header is abused to carry the status
		 */
		*reply = falseReply;
		reply->m_postsize = size;
		reply->m_cid = ECA_NORDACCESS;
		memset((char *)(reply+1), 0, size);
		this->commitMsg ();
	}
	
	return S_cas_success;
}
#endif

//
// casStrmClient::readSyncAction()
//
// This message indicates that the R3.13 or before client
// timed out on a read so we must clear out any pending 
// asynchronous IO associated with a read.
//
caStatus casStrmClient::readSyncAction ( epicsGuard < casClientMutex > & )
{
    tsDLIter < casChannelI > iter = 
                this->chanList.firstIter ();
    while ( iter.valid() ) {
        iter->clearOutstandingReads ();
        iter++;
    }

	const caHdrLargeArray * mp = this->ctx.getMsg ();

    int	status = this->out.copyInHeader ( mp->m_cmmd, 0,
        mp->m_dataType, mp->m_count, 
        mp->m_cid, mp->m_available, 0 );
	if ( ! status ) {
	    this->out.commitMsg ();
	}

	return status;
}

 //
 // casStrmClient::accessRightsResponse()
 //
 // NOTE:
 // Do not change the size of this response without making
 // parallel changes in createChanResp
 //
caStatus casStrmClient::accessRightsResponse ( casChannelI * pciu )
{
    epicsGuard < casClientMutex > guard ( this->mutex );
    return this->accessRightsResponse ( guard, pciu );
}

caStatus casStrmClient::accessRightsResponse ( 
    epicsGuard < casClientMutex > &, casChannelI * pciu )
{
    unsigned ar;
    int v41;
    int status;
    
    // noop if this is an old client
    v41 = CA_V41 ( this->minor_version_number );
    if ( ! v41 ) {
        return S_cas_success;
    }
    
    ar = 0; // none 
    if ( pciu->readAccess() ) {
        ar |= CA_PROTO_ACCESS_RIGHT_READ;
    }
    if ( pciu->writeAccess() ) {
        ar |= CA_PROTO_ACCESS_RIGHT_WRITE;
    }
    
    status = this->out.copyInHeader ( CA_PROTO_ACCESS_RIGHTS, 0,
        0, 0, pciu->getCID(), ar, 0 );
    if ( ! status ) {
        this->out.commitMsg ();
    }
    
    return status;
}

//
// casStrmClient::write()
//
caStatus casStrmClient::write()
{
	const caHdrLargeArray *pHdr = this->ctx.getMsg();
	caStatus status;

	//
	// no puts via compound types (for now)
	//
	if (dbr_value_offset[pHdr->m_dataType]) {
		return S_cas_badType;
	}

#ifdef CONVERSION_REQUIRED
	/* use type as index into conversion jumptable */
	(* cac_dbr_cvrt[pHdr->m_dataType])
		( this->ctx.getData(),
		  this->ctx.getData(),
		  false,       /* net -> host format */
		  pHdr->m_count);
#endif

	//
	// clear async IO flag
	//
	this->userStartedAsyncIO = false;

	//
	// DBR_STRING is stored outside the DD so it
	// lumped in with arrays
	//
	if ( pHdr->m_count > 1u ) {
		status = this->writeArrayData ();
	}
	else {
		status = this->writeScalarData ();
	}

	//
	// prevent problems when they initiate
	// async IO but dont return status
	// indicating so (and vise versa)
	//
	if ( this->userStartedAsyncIO ) {
		if (status!=S_casApp_asyncCompletion) {
			fprintf(stderr, 
"Application returned %d from casPV::write() - expected S_casApp_asyncCompletion\n",
				status);
			status = S_casApp_asyncCompletion;
		}
	}
	else if ( status == S_casApp_asyncCompletion ) {
		status = S_cas_badParameter;
		errMessage ( status, 
		"- expected asynch IO creation from casPV::write()" );
	}

	return status;
}

//
// casStrmClient::writeScalarData()
//
caStatus casStrmClient::writeScalarData ()
{
	const caHdrLargeArray * pHdr = this->ctx.getMsg ();

	/*
	 * DBR type has already been checked, but it is possible
	 * that "gddDbrToAit" will not track with changes in
	 * the DBR_XXXX type system
	 */
	if ( pHdr->m_dataType >= NELEMENTS(gddDbrToAit) ) {
		return S_cas_badType;
	}
	aitEnum	type = gddDbrToAit[pHdr->m_dataType].type;
	if ( type == aitEnumInvalid ) {
		return S_cas_badType;
	}

    aitEnum	bestExternalType = this->ctx.getPV()->bestExternalType ();

	gdd * pDD = new gddScalar ( gddAppType_value, bestExternalType );
	if ( ! pDD ) {
		return S_cas_noMemory;
	}

    //
    // copy in, and convert to native type, the incoming data
    //
    gddStatus gddStat = aitConvert ( 
        pDD->primitiveType(), pDD->dataAddress(), type, 
        this->ctx.getData(), 1, &this->ctx.getPV()->enumStringTable() );
	caStatus status = S_cas_noConvert;
    if ( gddStat >= 0 ) { 
        //
        // set the status and severity to normal
        //
	    pDD->setStat ( epicsAlarmNone );
	    pDD->setSevr ( epicsSevNone );

        //
        // set the time stamp to the last time that
        // we added bytes to the in buf
        //
        aitTimeStamp gddts = this->lastRecvTS;
        pDD->setTimeStamp ( & gddts );

	    //
	    // call the server tool's virtual function
	    //
	    status = this->ctx.getPV()->write ( this->ctx, *pDD );
    }

	//
	// reference count is managed by smart pointer class
	// from here down
	//
	gddStat = pDD->unreference();
	assert ( ! gddStat );

	return status;
}

//
// casStrmClient::writeArrayData()
//
caStatus casStrmClient::writeArrayData()
{
	const caHdrLargeArray *pHdr = this->ctx.getMsg ();

	/*
	 * DBR type has already been checked, but it is possible
	 * that "gddDbrToAit" will not track with changes in
	 * the DBR_XXXX type system
	 */
	if ( pHdr->m_dataType >= NELEMENTS(gddDbrToAit) ) {
		return S_cas_badType;
	}
	aitEnum	type = gddDbrToAit[pHdr->m_dataType].type;
	if ( type == aitEnumInvalid ) {
		return S_cas_badType;
	}

    aitEnum	bestExternalType = this->ctx.getPV()->bestExternalType ();

	gdd * pDD = new gddAtomic(gddAppType_value, bestExternalType, 1, pHdr->m_count);
	if ( ! pDD ) {
		return S_cas_noMemory;
	}

    size_t size = aitSize[bestExternalType] * pHdr->m_count;
	char * pData = new char [size];
	if ( ! pData ) {
        pDD->unreference();
		return S_cas_noMemory;
	}

	//
	// ok to use the default gddDestructor here because
	// an array of characters was allocated above
	//
	gddDestructor * pDestructor = new gddDestructor;
	if ( ! pDestructor ) {
        pDD->unreference();
		delete [] pData;
		return S_cas_noMemory;
	}

	//
	// install allocated area into the DD
	//
	pDD->putRef ( pData, type, pDestructor );

	//
	// convert the data from the protocol buffer
	// to the allocated area so that they
	// will be allowed to ref the DD
	//
    caStatus status = S_cas_noConvert;
    gddStatus gddStat = aitConvert ( bestExternalType, 
        pData, type, this->ctx.getData(), 
        pHdr->m_count, &this->ctx.getPV()->enumStringTable() );
    if ( gddStat >= 0 ) { 
        //
        // set the status and severity to normal
        //
        pDD->setStat ( epicsAlarmNone );
	    pDD->setSevr ( epicsSevNone );

        //
        // set the time stamp to the last time that
        // we added bytes to the in buf
        //
        aitTimeStamp gddts = this->lastRecvTS;
        pDD->setTimeStamp ( & gddts );

	    //
	    // call the server tool's virtual function
	    //
	    status = this->ctx.getPV()->write ( this->ctx, *pDD );
    }

    gddStat = pDD->unreference ();
	assert ( ! gddStat );

	return status;
}

//
// casStrmClient::read()
//
caStatus casStrmClient::read ( const gdd * & pDescRet )
{
	const caHdrLargeArray * pHdr = this->ctx.getMsg();

    pDescRet = 0;

	gdd * pDD = createDBRDD ( pHdr->m_dataType, pHdr->m_count );
	if ( ! pDD ) {
		return S_cas_noMemory;
	}

	//
	// clear the async IO flag
	//
	this->userStartedAsyncIO = false;

	//
	// call the server tool's virtual function
	//
	caStatus status = this->ctx.getPV()->read ( this->ctx, * pDD );

	//
	// prevent problems when they initiate
	// async IO but dont return status
	// indicating so (and vise versa)
	//
	if ( this->userStartedAsyncIO ) {
		if ( status != S_casApp_asyncCompletion ) {
			fprintf(stderr, 
"Application returned %d from casPV::read() - expected S_casApp_asyncCompletion\n",
				status);
			status = S_casApp_asyncCompletion;
		}
	}
	else if ( status == S_casApp_asyncCompletion ) {
		status = S_cas_badParameter;
		errMessage(status, 
			"- expected asynch IO creation from casPV::read()");
	}

	if ( status == S_casApp_success ) {
        pDescRet = pDD;
	}
    else {
        pDD->unreference ();
    }

	return status;
}

//
// casStrmClient::userName() 
//
void casStrmClient::userName ( char * pBuf, unsigned bufSize ) const
{
    if ( bufSize ) {
        const char *pName = this->pUserName ? this->pUserName : "?";
        strncpy ( pBuf, pName, bufSize );
        pBuf [bufSize-1] = '\0';
    }
}

//
// caServerI::roomForNewChannel()
//
inline bool caServerI::roomForNewChannel() const
{
	return true;
}

//
//  casStrmClient::xSend()
//
outBufClient::flushCondition casStrmClient::xSend ( char * pBufIn,
                                             bufSizeT nBytesAvailableToSend,
                                             bufSizeT nBytesNeedToBeSent,
                                             bufSizeT & nActualBytes )
{
    outBufClient::flushCondition stat = outBufClient::flushDisconnect;
    bufSizeT nActualBytesDelta;
    bufSizeT totalBytes;

    assert ( nBytesAvailableToSend >= nBytesNeedToBeSent );
	
    totalBytes = 0u;
    while ( true ) {
        stat = this->osdSend ( &pBufIn[totalBytes],
                              nBytesAvailableToSend-totalBytes, nActualBytesDelta );
        if ( stat != outBufClient::flushProgress ) {
            if ( totalBytes > 0 ) {
                nActualBytes = totalBytes;
		        //
		        // !! this time fetch may be slowing things down !!
		        //
		        //this->lastSendTS = epicsTime::getCurrent();
                stat = outBufClient::flushProgress;
                break;
            }
            else {
                break;
            }
        }
		
        totalBytes += nActualBytesDelta;
		
        if ( totalBytes >= nBytesNeedToBeSent ) {
		    //
		    // !! this time fetch may be slowing things down !!
		    //
		    //this->lastSendTS = epicsTime::getCurrent();
            nActualBytes = totalBytes;
            stat = outBufClient::flushProgress;
            break;
        }
    }
	return stat;
}

//
// casStrmClient::xRecv()
//
inBufClient::fillCondition casStrmClient::xRecv ( char * pBufIn, bufSizeT nBytes,
                                 inBufClient::fillParameter, bufSizeT & nActualBytes )
{
	inBufClient::fillCondition stat;
	
	stat = this->osdRecv ( pBufIn, nBytes, nActualBytes );
    //
    // this is used to set the time stamp for write GDD's
    //
    this->lastRecvTS = epicsTime::getCurrent ();
	return stat;
}

//
// casStrmClient::getDebugLevel()
//
unsigned casStrmClient::getDebugLevel () const
{
	return this->getCAS().getDebugLevel ();
}

//
// casStrmClient::casMonitorCallBack()
//
caStatus casStrmClient::casMonitorCallBack ( 
    epicsGuard < casClientMutex > & guard,
    casMonitor & mon, const gdd & value )
{
    return mon.response ( guard, *this, value );
}

//
//	casStrmClient::sendErr()
//
caStatus casStrmClient::sendErr ( epicsGuard <casClientMutex> &,
    const caHdrLargeArray * curp, ca_uint32_t cid, 
    const int reportedStatus, const char *pformat, ... )
{
	unsigned stringSize;
	char msgBuf[1024]; /* allocate plenty of space for the message string */
	if ( pformat ) {
	    va_list args;
		va_start ( args, pformat );
		int status = vsprintf (msgBuf, pformat, args);
		if ( status < 0 ) {
			errPrintf (S_cas_internal, __FILE__, __LINE__,
				"bad sendErr(%s)", pformat);
			stringSize = 0u;
		}
		else {
			stringSize = 1u + (unsigned) status;
		}
	}
	else {
		stringSize = 0u;
	}

    unsigned hdrSize = sizeof ( caHdr );
    if ( ( curp->m_postsize >= 0xffff || curp->m_count >= 0xffff ) && 
            CA_V49( this->minor_version_number ) ) {
        hdrSize += 2 * sizeof ( ca_uint32_t );
    }

    caHdr * pReqOut;
    caStatus status = this->out.copyInHeader ( CA_PROTO_ERROR, 
        hdrSize + stringSize, 0, 0, cid, reportedStatus,
        reinterpret_cast <void **> ( & pReqOut ) );
    if ( ! status ) {
        char * pMsgString;

        /*
         * copy back the request protocol
         * (in network byte order)
         */
        if ( ( curp->m_postsize >= 0xffff || curp->m_count >= 0xffff ) && 
                CA_V49( this->minor_version_number ) ) {
            ca_uint32_t *pLW = ( ca_uint32_t * ) ( pReqOut + 1 );
            pReqOut->m_cmmd = htons ( curp->m_cmmd );
            pReqOut->m_postsize = htons ( 0xffff );
            pReqOut->m_dataType = htons ( curp->m_dataType );
            pReqOut->m_count = htons ( 0u );
            pReqOut->m_cid = htonl ( curp->m_cid );
            pReqOut->m_available = htonl ( curp->m_available );
            pLW[0] = htonl ( curp->m_postsize );
            pLW[1] = htonl ( curp->m_count );
            pMsgString = ( char * ) ( pLW + 2 );
        }
        else {
            pReqOut->m_cmmd = htons (curp->m_cmmd);
            pReqOut->m_postsize = htons ( ( (ca_uint16_t) curp->m_postsize ) );
            pReqOut->m_dataType = htons (curp->m_dataType);
            pReqOut->m_count = htons ( ( (ca_uint16_t) curp->m_count ) );
            pReqOut->m_cid = htonl (curp->m_cid);
            pReqOut->m_available = htonl (curp->m_available);
            pMsgString = ( char * ) ( pReqOut + 1 );
         }

        /*
         * add their context string into the protocol
         */
        memcpy ( pMsgString, msgBuf, stringSize );

        this->out.commitMsg ();
    }

	return S_cas_success;
}

// send minor protocol revision to the client
void casStrmClient::sendVersion ()
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    caStatus status = this->out.copyInHeader ( CA_PROTO_VERSION, 0, 
        0, CA_MINOR_PROTOCOL_REVISION, 0, 0, 0 );
    if ( ! status ) {
        this->out.commitMsg ();
    }
}

bool casStrmClient::inBufFull () const
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    return this->in.full ();
}

inBufClient::fillCondition casStrmClient::inBufFill ()
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    return this->in.fill ();
}

bufSizeT casStrmClient::inBufBytesAvailable () const
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    return this->in.bytesAvailable ();
}

bufSizeT casStrmClient::outBufBytesPresent () const
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    return this->out.bytesPresent ();
}

outBufClient::flushCondition casStrmClient::flush ()
{
    epicsGuard < epicsMutex > guard ( this->mutex );
    return this->out.flush ();
}

//
// casStrmClient::logBadIdWithFileAndLineno()
//
caStatus casStrmClient::logBadIdWithFileAndLineno ( 
    epicsGuard < casClientMutex > & guard,
    const caHdrLargeArray * mp, const void * dp, 
    const int cacStatus, const char * pFileName, 
    const unsigned lineno, const unsigned idIn )
{
	if ( pFileName ) {
        caServerI::dumpMsg ( this->pHostName, this->pUserName, mp, dp, 
            "bad resource id in \"%s\" at line %d\n",
			pFileName, lineno );
	}
    else {
	    caServerI::dumpMsg ( this->pHostName, this->pUserName, mp, dp, 
            "bad resource id\n" );
    }

	int status = this->sendErr ( guard,
			mp, invalidResID, cacStatus, 
            "Bad Resource ID=%u detected at %s.%d",
			idIn, pFileName, lineno );

	return status;
}

/*
 * casStrmClient::sendErrWithEpicsStatus()
 *
 * same as sendErr() except that we convert epicsStatus
 * to a string and send that additional detail
 */
caStatus casStrmClient::sendErrWithEpicsStatus ( 
    epicsGuard < casClientMutex > & guard, const caHdrLargeArray * pMsg, 
	ca_uint32_t cid, caStatus epicsStatus, caStatus clientStatus )
{
	char buf[0x1ff];
	errSymLookup ( epicsStatus, buf, sizeof(buf) );
	return this->sendErr ( guard, pMsg, cid, clientStatus, buf );
}

