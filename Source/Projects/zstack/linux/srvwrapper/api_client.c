/**************************************************************************************************
 Filename:			 api_client.c
 Revised:				$Date: 2014-06-06 00:21:27 -0700 (Fri, 06 Jun 2014) $
 Revision:			 $Revision: 38849 $

 Description:		This file contains the API Server Wrapper client APIs.


 Copyright 2013 - 2014 Texas Instruments Incorporated. All rights reserved.

 IMPORTANT: Your use of this Software is limited to those specific rights
 granted under the terms of a software license agreement between the user
 who downloaded the software, his/her employer (which must be your employer)
 and Texas Instruments Incorporated (the "License").	You may not use this
 Software unless you agree to abide by the terms of the License. The License
 limits your use, and you acknowledge, that the Software may not be modified,
 copied or distributed unless used solely and exclusively in conjunction with
 a Texas Instruments radio frequency device, which is integrated into
 your product.	Other than for the foregoing purpose, you may not use,
 reproduce, copy, prepare derivative works of, modify, distribute, perform,
 display or sell this Software and/or its documentation for any purpose.

 YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
 PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, TITLE,
 NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
 TEXAS INSTRUMENTS OR ITS LICENSORS BE LIABLE OR OBLIGATED UNDER CONTRACT,
 NEGLIGENCE, STRICT LIABILITY, CONTRIBUTION, BREACH OF WARRANTY, OR OTHER
 LEGAL EQUITABLE THEORY ANY DIRECT OR INDIRECT DAMAGES OR EXPENSES
 INCLUDING BUT NOT LIMITED TO ANY INCIDENTAL, SPECIAL, INDIRECT, PUNITIVE
 OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, COST OF PROCUREMENT
 OF SUBSTITUTE GOODS, TECHNOLOGY, SERVICES, OR ANY CLAIMS BY THIRD PARTIES
 (INCLUDING BUT NOT LIMITED TO ANY DEFENSE THEREOF), OR OTHER SIMILAR COSTS.

 Should you have any questions regarding your right to use this Software,
 contact Texas Instruments Incorporated at www.TI.com.
 **************************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <pthread.h>
#include <semaphore.h>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <signal.h>
#include <bits/local_lim.h>
#include <fcntl.h>

#include "hal_types.h"
#include "hal_rpc.h"
#include "api_lnx_ipc_rpc.h"
#include "api_client.h"
#include "trace.h"

#include "config.h"

/**************************************************************************************************
 * Constant
 **************************************************************************************************/
#define MSG_AREQ_READY		                0x0000
#define MSG_AREQ_BUSY			            0x0001

// These came from api_lnx_ipc_rpc.h
#define API_LNX_PARAM_NB_CONNECTIONS		1
#define API_LNX_PARAM_DEVICE_USED		    2

/**************************************************************************************************
 * Typedefs
 **************************************************************************************************/

#ifdef API_CLIENT_8BIT_LEN
typedef apic8BitLenMsgHdr_t apicMsgHdr_t;
#else // API_CLIENT_8BIT_LEN
typedef apic16BitLenMsgHdr_t apicMsgHdr_t;
#endif // API_CLIENT_8BIT_LEN
struct linkedAreqMsg
{
	struct linkedAreqMsg *nextMessage;
	uint8 subSys;
	uint8 cmdId;
	uint16 len;
};

typedef struct linkedAreqMsg areqMsg;

typedef struct _apicInstance_t
{
	// Client pipe handle
	int sAPIreadPipe;
	int sAPIwritePipe;

	// Mutex to handle synchronous response
	// i.e., to protect the synchronous response queue
	pthread_mutex_t clientSREQmutex;

	// Mutex to protect asynchronous message receive queue
	pthread_mutex_t clientAREQmutex;

	// Mutex to protect socket write
	pthread_mutex_t sendMutex;

	// conditional variable to notify Synchronous response
	pthread_cond_t clientSREQcond;

	// conditional variable to notify that the AREQ is handled
	sem_t clientAREQsem;

	pthread_t SISRThreadId;
	pthread_t SISHThreadId;

	// Client data AREQ received buffer
	areqMsg *areq_rec_buf;

	// SRSP received message payload
	areqMsg *srsp_msg;

	// variable to store the number of received synchronous response bytes
	long numOfReceivedSRSPbytes;

	// Message count to keep track of incoming and processed messages
	int areqRxMsgCount;
	int areqProcMsgCount;

	// Notification from receive thread to callback thread
	// that the connection is closed.
	bool closed;

	// Application triggered close
	bool appClosed;

	// Freeing this memory block is postponed because apicClose()
	// is called from a callback thread
	bool freePending;

	// Asynchronous message callback function
	pfnAsyncMsgCb pfnAsyncMsgHandler;
} apicInstance_t;

/**************************************************************************************************
 * Globals
 **************************************************************************************************/

size_t apicThreadStackSize = (PTHREAD_STACK_MIN * 3); // 16K*3

/**************************************************************************************************
 * Locals
 **************************************************************************************************/

/**************************************************************************************************
 * Function Prototypes
 **************************************************************************************************/
static void initSyncRes( apicInstance_t *pInstance );
static void delSyncRes( apicInstance_t *pInstance );
static void *SISreadThreadFunc( void *ptr );
static void *SIShandleThreadFunc( void *ptr );
static int asynchMsgCback( apicInstance_t *pInstance, areqMsg *pMsg );

/**************************************************************************************************
 *
 * @fn					apicIgnoreSigPipe
 *
 * @brief			 This function sets SIGPIPE signal handling action to ignore
 *							the signal.
 *							This function, if to be called, must be called prior to
 *							any apicInit() call because apicInit() call itself may cause
 *							a SIGPIPE signal.
 *							If this function is not called, there is a chance that
 *							the process may be terminated when server connected to the
 *							process drops the connection and hence application must
 *							handle SIGPIPE with its own handler in such a case in order
 *							to gracefully handle disconnection by peer.
 *
 * @param			 None
 *
 * @return			None
 *
 **************************************************************************************************/
void apicIgnoreSigPipe( void )
{
	struct sigaction newact;

	/* Set up a new action to ignore the signal. */
	newact.sa_handler = SIG_IGN;
	sigemptyset( &newact.sa_mask );
	newact.sa_flags = 0;
	sigaction( SIGPIPE, &newact, NULL );
}

/**************************************************************************************************
 *
 * @fn					apicInit
 *
 * @brief			 This function initializes API Client Socket
 *
 * @param			 srvName - path to the serial interface server
 * @param			 getVer - TRUE to get and display the server information after connection
 * @param			 pFn - function pointer to async message handler
 *										Note that if len argument is passed as 0xffffu to this
 *										handler function, the API client is notifying that
 *										the connection is dropped by the peer (server)
 *										and other parameters passed to the handler function
 *										are not valid.
 *
 * @return			API client handle if successful.
 *							NULL, otherwise.
 *
 **************************************************************************************************/
apicHandle_t apicInit( const char *srvName, bool getVer, pfnAsyncMsgCb pFn )
{
	apicInstance_t *pInstance;
	int n;
	char readPipePathName[APIC_READWRITE_PIPE_NAME_LEN];
	char writePipePathName[APIC_READWRITE_PIPE_NAME_LEN];
    char checkString[APIC_SEND_SERVER_PIPE_CHECK_STRING_LEN];
    char assignedIdBuf[APIC_READ_ASSIGNED_ID_BUF_LEN];
    int readWriteNum;

	pthread_attr_t attr;
    int tmpReadPipe;
    int tmpWritePipe;

    memset(assignedIdBuf,'\0',APIC_READ_ASSIGNED_ID_BUF_LEN);
    memset(checkString,'\0',APIC_SEND_SERVER_PIPE_CHECK_STRING_LEN);
	memset(readPipePathName,'\0',APIC_READWRITE_PIPE_NAME_LEN);
	memset(writePipePathName,'\0',APIC_READWRITE_PIPE_NAME_LEN);

	// prepare thread creation
	if ( pthread_attr_init( &attr ) )
	{
		perror( "pthread_attr_init" );
		return NULL;
	}

	if ( pthread_attr_setstacksize( &attr, apicThreadStackSize ) )
	{
		perror( "pthread_attr_setstacksize" );
		return NULL;
	}

	pInstance = malloc( sizeof(apicInstance_t) );

	if ( !pInstance )
	{
		uiPrintf( "[ERR] apicInit malloc failed\n" );
		return pInstance;
	}

	// Clear the instance
	memset( pInstance, 0, sizeof(*pInstance) );

	/**********************************************************************
	 * Initiate synchronization resources
	 */
	initSyncRes( pInstance );

    //Server pipe select
	if(!strncmp(srvName,SERVER_NPI_IPC,strlen(SERVER_NPI_IPC)))
	{
        strncpy(checkString,NPI_IPC_LISTEN_PIPE_CHECK_STRING,strlen(NPI_IPC_LISTEN_PIPE_CHECK_STRING));
		strncpy(readPipePathName,NPI_IPC_LISTEN_PIPE_SERVER2CLIENT,strlen(NPI_IPC_LISTEN_PIPE_SERVER2CLIENT));
		strncpy(writePipePathName,NPI_IPC_LISTEN_PIPE_CLIENT2SERVER,strlen(NPI_IPC_LISTEN_PIPE_CLIENT2SERVER));
	}
	else if(!strncmp(srvName,SERVER_ZLSZNP,strlen(SERVER_ZLSZNP)))
	{
        strncpy(checkString,ZLSZNP_LISTEN_PIPE_CHECK_STRING,strlen(ZLSZNP_LISTEN_PIPE_CHECK_STRING));
		strncpy(readPipePathName,ZLSZNP_LISTEN_PIPE_SERVER2CLIENT,strlen(ZLSZNP_LISTEN_PIPE_SERVER2CLIENT));
		strncpy(writePipePathName,ZLSZNP_LISTEN_PIPE_CLIENT2SERVER,strlen(ZLSZNP_LISTEN_PIPE_CLIENT2SERVER));
	}
	else if(!strncmp(srvName,SERVER_NWKMGR,strlen(SERVER_NWKMGR)))
	{
        strncpy(checkString,NWKMGR_LISTEN_PIPE_CHECK_STRING,strlen(NWKMGR_LISTEN_PIPE_CHECK_STRING));
		strncpy(readPipePathName,NWKMGR_LISTEN_PIPE_SERVER2CLIENT,strlen(NWKMGR_LISTEN_PIPE_SERVER2CLIENT));
		strncpy(writePipePathName,NWKMGR_LISTEN_PIPE_CLIENT2SERVER,strlen(NWKMGR_LISTEN_PIPE_CLIENT2SERVER));
	}
	else if(!strncmp(srvName,SERVER_OTASERVER,strlen(SERVER_OTASERVER)))
	{
        strncpy(checkString,OTASERVER_LISTEN_PIPE_CHECK_STRING,strlen(OTASERVER_LISTEN_PIPE_CHECK_STRING));
		strncpy(readPipePathName,OTASERVER_LISTEN_PIPE_SERVER2CLIENT,strlen(OTASERVER_LISTEN_PIPE_SERVER2CLIENT));
		strncpy(writePipePathName,OTASERVER_LISTEN_PIPE_CLIENT2SERVER,strlen(OTASERVER_LISTEN_PIPE_CLIENT2SERVER));
	}
	else if(!strncmp(srvName,SERVER_GATEWAY,strlen(SERVER_GATEWAY)))
	{
        strncpy(checkString,GATEWAY_LISTEN_PIPE_CHECK_STRING,strlen(GATEWAY_LISTEN_PIPE_CHECK_STRING));
		strncpy(readPipePathName,GATEWAY_LISTEN_PIPE_SERVER2CLIENT,strlen(GATEWAY_LISTEN_PIPE_SERVER2CLIENT));
		strncpy(writePipePathName,GATEWAY_LISTEN_PIPE_CLIENT2SERVER,strlen(GATEWAY_LISTEN_PIPE_CLIENT2SERVER));
	}
    else 
    {
		perror( "wrong server name provided" );
		return NULL;
    }
	uiPrintf("readPipePathName is %s\n",readPipePathName);
	uiPrintf("writePipePathName is %s\n",writePipePathName);

	/**********************************************************************
	 * Open listen pipe for getting a unique pipe id
	 */
	uiPrintf( "Trying to open listen pipes...\n" );
	if ((mkfifo (readPipePathName, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		uiPrintf ("cannot create fifo %s\n", readPipePathName);
	}
	if ((mkfifo (writePipePathName, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		uiPrintf ("cannot create fifo %s\n", writePipePathName);
	}
    //阻塞打开写监听管道
    tmpWritePipe = open(writePipePathName, O_WRONLY, 0);
    if(tmpWritePipe == -1)
    {
        //error
        uiPrintf("apicInit open tmpWritePipe failed.\n");
    }
	//pause();
    //写入管道
    n = write (tmpWritePipe,checkString,strlen(checkString));
	uiPrintf("write to tmpWritePipe checkString %d.\n", n);
    //阻塞打开读监听管道
    tmpReadPipe = open(readPipePathName, O_RDONLY, 0);
    if(tmpReadPipe == -1)
    {
        //error
        uiPrintf("open readPipePathName failed\n");
    }
    //读取分配的id
    readWriteNum = read(tmpReadPipe, assignedIdBuf, APIC_READ_ASSIGNED_ID_BUF_LEN);
	uiPrintf("readWriteNum is %d.\n",readWriteNum);
    if(readWriteNum<=0)
    {
        //error
    }
    //附属到默认管道名后面作为一个临时名字
    strcat(readPipePathName,assignedIdBuf);
    strcat(writePipePathName,assignedIdBuf); 

	uiPrintf("readPipePathName is %s\n",readPipePathName);
	uiPrintf("writePipePathName is %s\n",writePipePathName);

	//pause();
    //关闭监听管道的读写
	/**********************************************************************
	 * Open to the API server pipes
	 **********************************************************************/

	uiPrintf( "Trying to open regular pipes...\n" );
	if ((mkfifo (readPipePathName, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		uiPrintf ("cannot create fifo %s\n", readPipePathName);
	}
	if ((mkfifo (writePipePathName, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		uiPrintf ("cannot create fifo %s\n", writePipePathName);
	}

	pInstance->sAPIreadPipe = open(readPipePathName, O_RDONLY, 0);
	if(pInstance->sAPIreadPipe == -1)
	{
		perror( "read pipe open" );

		delSyncRes( pInstance );
		free( pInstance );
		return NULL;
	}
	pInstance->sAPIwritePipe = open(writePipePathName, O_WRONLY, 0);
	if(pInstance->sAPIwritePipe == -1)
	{
		perror( "write pipe open" );

		delSyncRes( pInstance );
		free( pInstance );
		return NULL;
	}

	uiPrintf( "Both read and wirte pipes opened.\n" );

    close(tmpReadPipe);
    close(tmpWritePipe);

	// Set up asynchronous message handler before creating callback thread.
	pInstance->pfnAsyncMsgHandler = pFn;

	/****************************************************************************
	 * Create thread which can read new messages from the serial interface server
	 ****************************************************************************/

	if ( pthread_create( &pInstance->SISRThreadId, &attr, SISreadThreadFunc,
			pInstance ) )
	{
		// thread creation failed
		uiPrintf( "Failed to create RTIS LNX IPC Client read thread\n" );
        close( pInstance->sAPIreadPipe);
        close( pInstance->sAPIwritePipe);
		delSyncRes( pInstance );
		free( pInstance );
		return NULL;
	}

	/******************************************************************************
	 * Create thread which can handle new messages from the serial interface server
	 ******************************************************************************/

	if ( pthread_create( &pInstance->SISHThreadId, &attr, SIShandleThreadFunc,
			pInstance ) )
	{
		// thread creation failed
		uiPrintf( "Failed to create RTIS LNX IPC Client handle thread\n" );
        close( pInstance->sAPIreadPipe);
        close( pInstance->sAPIwritePipe);
		pthread_join( pInstance->SISRThreadId, NULL );
		delSyncRes( pInstance );
		free( pInstance );
		return NULL;
	}

	if ( getVer )
	{
		uint8 version[3];
		uint8 param[2];

		//Read Software Version.
		apicReadVersionReq( pInstance, version );
		uiPrintf( "Connected to Server v%d.%d.%d\n", version[0], version[1],\
				version[2] );

		//Read Number of Active Connection Version.
		apicReadParamReq( pInstance, API_LNX_PARAM_NB_CONNECTIONS, 2, param );
		uiPrintf( "%d active connection , out of %d maximum connections\n", param[0],
				param[1] );

		//Check Which interface is used.
		apicReadParamReq( pInstance, API_LNX_PARAM_DEVICE_USED, 1, param );
		uiPrintf( "Interface used y server: %d (0 = UART, 1 = SPI, 2 = I2C)\n",
				param[0] );
	}

	return pInstance;
}

/**************************************************************************************************
 *
 * @fn					apicClose
 *
 * @brief			 This function stops API client
 *
 * @param			 handle - API client handle
 *
 * @return			None.
 *
 **************************************************************************************************/
void apicClose( apicHandle_t handle )
{
	apicInstance_t *pInstance = handle;

	if ( pInstance->appClosed )
	{
		// This function was called again.
		return;
	}

	// Not to notify application of connection close when application triggered
	// connection close, indicate that the application triggered close.
	pInstance->appClosed = TRUE;

	// Close the API client socket connection
	// For some reason, close() without shutdown() does not unblock
	// read() in the receive thread when attached to a debugger or
	// when run from valgrind, though close() supposedly encompass
	// shutdown().
	close( pInstance->sAPIreadPipe );
    close( pInstance->sAPIwritePipe );

	// Join receive thread and callback thread.
	// It is important to wait till those threads are closed
	// since, otherwise, the threads will access released resources.
	pthread_join( pInstance->SISRThreadId, NULL );

	if ( pthread_self() == pInstance->SISHThreadId )
	{
		// To prevent deadlock, join() is not called from the same
		// context, but we know that SIShandleThreadFunc()
		// must not be calling and pInstance variable any longer
		pInstance->freePending = TRUE;
	}
	else
	{
		pthread_join( pInstance->SISHThreadId, NULL );
	}

	// Delete synchronization resources
	delSyncRes( pInstance );

	if ( !pInstance->freePending )
	{
		// Free the memory block for the instance
		free( pInstance );
	}
}

/**************************************************************************************************
 *
 * @fn					apicSendSynchData
 *
 * @brief			 This function sends a message synchronously over the socket
 *
 * @param			 handle - API client handle returned from apicInit()
 *
 * @param			 subSys - Subsystem ID
 *
 * @param			 cmdId - Command ID
 *
 * @param			 len- length in bytes of the message payload to write
 *
 * @param			 pData - payload message to write
 *
 * @param			 pRxSubSys - pointer to a buffer to store the subsystem ID
 *											of the received response message.
 *											This parameter may be NULL.
 *
 * @param			 pRxCmdId - pointer to a buffer to store the command ID
 *											of the received response message.
 *											This parameter may be NULL.
 *
 * @param			 pRxLen - pointer to a buffer to store the received
 *											response message payload length.
 *											This parameter may be NULL.
 *
 * @return			Pointer to response message payload or NULL when failed.
 *							The caller has to call apicFreeSynchData() to free
 *							the returned memory block once it is done with the received
 *							response message unless this function returned NULL.
 *							Note that even if the pRxLen dereferences value of zero,
 *							this function shall return a valid address that needs to
 *							be freed as far as the function succeeded.
 *
 **************************************************************************************************/
uint8 *apicSendSynchData( apicHandle_t handle, uint8 subSys, uint8 cmdId,
													uint16 len, const uint8 *pData, uint8 *pRxSubSys,
													uint8 *pRxCmdId, uint16 *pRxLen )
{
	int result = 0;
	struct timespec expirytime;
	struct timeval curtime;
	int mutexRet = 0, writeOnce = 0;
	size_t i;
	ssize_t n;
	uint8 *ptr;
	areqMsg *rspMsg = NULL;
	apicMsgHdr_t *hdr;
	apicInstance_t *pInstance = handle;

#ifdef API_CLIENT_8BIT_LEN
	if (len > 255)
#else // API_CLIENT_8BIT_LEN
	if ( len == 0xFFFFu )
#endif // API_CLIENT_8BIT_LEN
	{
		uiPrintf( "[ERR] apicSendSynchData failed due to excessive length\n" );
		return NULL;
	}

	hdr = malloc( sizeof(apicMsgHdr_t) + len );
	if ( !hdr )
	{
		uiPrintf( "[ERR] apicSendSynchData failed due to malloc() failure\n" );
		return NULL;
	}
	hdr->subSys = subSys & RPC_SUBSYSTEM_MASK;
	if ( (subSys & RPC_CMD_TYPE_MASK) == 0 )
	{
		hdr->subSys |= RPC_CMD_SREQ;
	}
	hdr->cmdId = cmdId;
#ifdef API_CLIENT_8BIT_LEN
	hdr->len = (uint8) len;
#else // API_CLIENT_8BIT_LEN
	// Length field endianness conversion to little endian
	hdr->lenL = (uint8) len;
	hdr->lenH = (uint8)( len >> 8 );
#endif // API_CLIENT_8BIT_LEN
	memcpy( hdr + 1, pData, len );

	uiPrintfEx(trINFO, "preparing to write %d bytes, subSys 0x%.2X, cmdId 0x%.2X, pData:\n",\
			len,\
			subSys,\
			cmdId );

	for ( i = 0; i < len; i++ )
	{
		uiPrintfEx(trINFO, " 0x%.2X\n", pData[i] );
	}

	// Lock mutex
	uiPrintfEx(trINFO, "[MUTEX] Lock SRSP Mutex" );
	while ( (mutexRet = pthread_mutex_trylock( &pInstance->clientSREQmutex ))
			== EBUSY )
	{
		if ( writeOnce == 0 )
		{
			uiPrintfEx(trINFO, "\n[MUTEX] SRSP Mutex busy" );
			fflush( stdout );
			writeOnce++;
		}
		else
		{
			writeOnce++;
			if ( (writeOnce % 1000) == 0 )
			{
				uiPrintfEx(trINFO, "." );
			}

			if ( writeOnce > 0xFFFFFFF0 )
			{
				writeOnce = 1;
			}

			fflush( stdout );
		}
	}

	uiPrintfEx(trINFO, "\n[MUTEX] SRSP Lock status: %d\n", mutexRet );

	len += sizeof(*hdr);
	ptr = (uint8 *) hdr;
	if ( pthread_mutex_lock( &pInstance->sendMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}
	for ( ;; )
	{
		n = write( pInstance->sAPIwritePipe, ptr, len );
		if ( n == -1 )
		{
			perror( "write" );
			pthread_mutex_unlock( &pInstance->sendMutex );
			free( hdr );
			pthread_mutex_unlock( &pInstance->clientSREQmutex );
			return NULL;
		}
		if ( n < len )
		{
			ptr += n;
			len -= n;
			// Repeat till entire frame is sent out.
			continue;
		}
		break;
	}
	pthread_mutex_unlock( &pInstance->sendMutex );
	free( hdr );

	uiPrintfEx(trINFO, "Waiting for synchronous response...\n" );

	// Conditional wait for the response handled in the receiving thread,
	// wait maximum 2 seconds
	gettimeofday( &curtime, NULL );
	expirytime.tv_sec = curtime.tv_sec + 2;
	expirytime.tv_nsec = curtime.tv_usec * 1000;

	uiPrintfEx(trINFO, "[MUTEX] Wait for SRSP Cond signal...\n" );

	result = pthread_cond_timedwait( &pInstance->clientSREQcond,
			&pInstance->clientSREQmutex, &expirytime );

	if ( result == ETIMEDOUT )
	{
		// TODO: Indicate synchronous transaction error
		uiPrintfEx(trINFO, "[MUTEX] SRSP Cond Wait timed out!\n" );
		uiPrintfEx(trINFO, "[ERR] SRSP Cond Wait timed out!\n" );
	}
	// Wait for response
	else if ( pInstance->numOfReceivedSRSPbytes > 0 )
	{
		for ( i = 0; i < pInstance->numOfReceivedSRSPbytes - sizeof(apicMsgHdr_t);
				i++ )
		{
			//GUNCOM Need to fix this issue, cannot comment out line belo
			//uiPrintfEx(trINFO, "0x%.2X\n",(unsigned int)((uint8 *)(pInstance->srsp_msg + 1)[i]) );
			uiPrintfEx(trINFO, "0x%.2X\n", ((uint8 *)(&(pInstance->srsp_msg[1])))[i]);
		}

		// Copy response back in transmission buffer for processing
		rspMsg = pInstance->srsp_msg;

		// Clear the response queue
		pInstance->srsp_msg = NULL;
	}
	else
	{
		// TODO: The code shouldn't reach here.
		//			 Replace the following with assert
		if ( pInstance->numOfReceivedSRSPbytes < 0 )
		{
			perror( "read" );
		}
		else
		{
			uiPrintfEx(trINFO, "Server closed connection\n" );
		}
	}

	pInstance->numOfReceivedSRSPbytes = 0;

	// Now unlock the mutex before returning
	//uiPrintf( "[MUTEX] Unlock SRSP Mutex\n" );
	pthread_mutex_unlock( &pInstance->clientSREQmutex );

	if ( rspMsg )
	{
		if ( pRxSubSys )
		{
			*pRxSubSys = rspMsg->subSys;
		}
		if ( pRxCmdId )
		{
			*pRxCmdId = rspMsg->cmdId;
		}
		if ( pRxLen )
		{
			*pRxLen = rspMsg->len;
		}
		return (uint8 *) (rspMsg + 1);
	}
	return NULL;
}

/**************************************************************************************************
 *
 * @fn					apicFreeSynchData
 *
 * @brief			 This function returns the received synchronous response data
 *							back to the memory pool.
 *
 * @param			 pData - pointer to the memory block that were returned
 *											from apicSendSynchData.
 *
 * @return			None
 *
 **************************************************************************************************/
void apicFreeSynchData( uint8 *pData )
{
	free( ((areqMsg *) pData) - 1 );
}

/**************************************************************************************************
 *
 * @fn					apicSendAsynchData
 *
 * @brief			 This function sends a message asynchronously over the socket
 *
 * @param			 handle - API client handle
 *
 * @param			 subSys - Subsystem ID
 *
 * @param			 cmdId - Command ID
 *
 * @param			 len- length in bytes of the message payload to write
 *
 * @param			 pData - payload message to write
 *
 * @return			None.
 *
 **************************************************************************************************/
void apicSendAsynchData( apicHandle_t handle, uint8 subSys, uint8 cmdId,
												 uint16 len, uint8 *pData )
{
	int i;
	size_t n;
	uint8 *ptr;
	apicMsgHdr_t *hdr;
	apicInstance_t *pInstance = handle;

#ifdef API_CLIENT_8BIT_LEN
	if (len > 255)
	{
		uiPrintf( "[ERR] apicSendAsynchData called with excessive length %d\n",\
				len );
		return;
	}
#endif // API_CLIENT_8BIT_LEN
	hdr = malloc( sizeof(apicMsgHdr_t) + len );
	if ( !hdr )
	{
		uiPrintf( "[ERR] apicSendAsynchData failed malloc()\n" );
		return;
	}

	// Add Proper RPC type to header
	hdr->subSys = subSys & RPC_SUBSYSTEM_MASK;

	if ( (subSys & RPC_CMD_TYPE_MASK) == 0 )
	{
		hdr->subSys |= RPC_CMD_AREQ;
	}
	hdr->cmdId = cmdId;
#ifdef API_CLIENT_8BIT_LEN
	hdr->len = len;
#else // API_CLIENT_8BIT_LEN
	// Endianness conversion
	hdr->lenL = (uint8) len;
	hdr->lenH = (uint8)( len >> 8 );
#endif // API_CLIENT_8BIT_LEN
	memcpy( hdr + 1, pData, len );
	uiPrintfEx(trINFO, "trying to write %d bytes,\t subSys 0x%.2X,"\
			" cmdId 0x%.2X, pData:\t",\
			len,\
			subSys,\
			cmdId );
	for ( i = 0; i < len; i++ )
	{
		uiPrintfEx(trINFO," 0x%.2X", pData[i] );
	}

	ptr = (uint8 *) hdr;
	len += sizeof(apicMsgHdr_t);
	if ( pthread_mutex_lock( &pInstance->sendMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}
	for ( ;; )
	{
		n = write( pInstance->sAPIwritePipe, ptr, len );
		if ( n == -1 )
		{
			perror( "write" );
		}
		else if ( n < len )
		{
			len -= n;
			ptr += n;
			// Repeat till all bytes of the message is sent.
			continue;
		}
		break;
	}
	pthread_mutex_unlock( &pInstance->sendMutex );
	free( hdr );
}

/**************************************************************************************************
 *
 * @fn					apicReadVersionReq
 *
 * @brief			 This API is used to read the serial interface server version.
 *
 * @param			 handle - API client handle
 *
 * @param			 pValue -
 *
 * @return			None.
 *
 **************************************************************************************************/
void apicReadVersionReq( apicHandle_t handle, uint8 *pValue )
{
	uint8 *pRsp;

	// Send Read Version Request
	pRsp = apicSendSynchData( handle, RPC_SYS_SRV_CTRL,
			API_LNX_CMD_ID_VERSION_REQ, 0, NULL, NULL, NULL, NULL );

	if ( pRsp )
	{
		// copy the reply data to the client's buffer
		// Note: the first byte of the payload is reserved for the status
		memcpy( pValue, &pRsp[1], 3 );
		apicFreeSynchData( pRsp );
	}
}

/**************************************************************************************************
 *
 * @fn					apicReadParamReq
 *
 * @brief			 This API is used to read serial interface server parameters.
 *
 * @param			 handle - API client handle
 * @param			 paramId - The parameter item identifier.
 * @param			 len - The length in bytes of the item identifier's data.
 * @param			 *pValue - Pointer to buffer where read data is placed.
 *
 * @return			None.
 *
 **************************************************************************************************/
void apicReadParamReq( apicHandle_t handle, uint8 paramId, uint8 len,
											 uint8 *pValue )
{
	uint8 *pRsp, reqdata[] =
	{ paramId, len };

	// Send param request
	pRsp = apicSendSynchData( handle, RPC_SYS_SRV_CTRL,
			API_LNX_CMD_ID_GET_PARAM_REQ, 2, reqdata, NULL, NULL, NULL );

	if ( pRsp )
	{
		// copy the reply data to the client's buffer
		// Note: the first byte of the payload is reserved for the status
		memcpy( pValue, &pRsp[1], len );
		apicFreeSynchData( pRsp );
	}
}

/* Initialize thread synchronization resources */
static void initSyncRes( apicInstance_t *pInstance )
{
	// initialize all mutexes
	pthread_mutex_init( &pInstance->clientSREQmutex, NULL );
	pthread_mutex_init( &pInstance->clientAREQmutex, NULL );
	pthread_mutex_init( &pInstance->sendMutex, NULL );

	// initialize all conditional variables
	pthread_cond_init( &pInstance->clientSREQcond, NULL );
	if ( sem_init( &pInstance->clientAREQsem, 0, 0 ) != 0 )
	{
		//uiPrintf( "[ERR] sem_init() failed\n" );
		exit( 1 );
	}
}

/* Destroy thread synchronization resources */
static void delSyncRes( apicInstance_t *pInstance )
{
	// destroy all conditional variables
	pthread_cond_destroy( &pInstance->clientSREQcond );
	sem_destroy( &pInstance->clientAREQsem );

	// destroy all mutexes
	pthread_mutex_destroy( &pInstance->clientSREQmutex );
	pthread_mutex_destroy( &pInstance->clientAREQmutex );

}

static void *SISreadThreadFunc( void *ptr )
{
	int done = 0, n;
	apicMsgHdr_t hdrbuf;
	apicInstance_t *pInstance = ptr;

	trace_init_thread("READ");
	/* thread loop */

	// Read from socket
	do
	{
		// Normal data
		n = read( pInstance->sAPIreadPipe, &hdrbuf, sizeof(hdrbuf) );

		if ( n <= 0 )
		{
			if ( n < 0 )
			{
				perror( "read" );
			}
			else
			{
				//uiPrintf("Peer closed connection\n" );
			}
			done = 1;
		}
		else if ( n == sizeof(hdrbuf) )
		{
			size_t len;
			areqMsg *pMsg;

			// We have received the header,
			// now read out length bytes and process it,
			// if there are bytes to receive.
#ifdef API_CLIENT_8BIT_LEN
			len = hdrbuf.len;
#else // API_CLIENT_8BIT_LEN
			// Convert little endian length to host endianness
			len = hdrbuf.lenL;
			len |= (uint16) hdrbuf.lenH << 8;
#endif // API_CLIENT_8BIT_LEN
			pMsg = malloc( sizeof(areqMsg) + len );
			if ( pMsg )
			{
				pMsg->nextMessage = NULL;
				pMsg->len = len;
				pMsg->subSys = hdrbuf.subSys;
				pMsg->cmdId = hdrbuf.cmdId;

				if ( len > 0 )
				{
					n = read( pInstance->sAPIreadPipe, pMsg + 1, len );
				}
				else
				{
					// There are no payload bytes; which is also valid.
					n = 0;
				}

				if ( n == pMsg->len )
				{
					int i;
					uiPrintfEx(trINFO,"Received %d bytes,\t subSys 0x%.2X,"\
							" cmdId 0x%.2X, pData:\n",\
							pMsg->len, pMsg->subSys, pMsg->cmdId );
					for ( i = 0; i < n; i++ )
					{
						//GUNCOM Need to fix this print statement, gives error
						//uiPrintfEx(trINFO, " 0x%.2X\n", (uint8)((uint8 *)(pMsg+1)[i]) );
						//uiPrintfEx(trINFO, " 0x%X\n", (uint8)((uint8 *)(pMsg+1)[i]) );
						uiPrintfEx(trINFO, " 0x%X\n",	((uint8 *)(&pMsg[1]))[i]	); 
					}

					if ( (pMsg->subSys & RPC_CMD_TYPE_MASK) == RPC_CMD_SRSP )
					{
						// and signal the synchronous reception
						uiPrintfEx(trINFO, "[MUTEX] SRSP Cond signal set\n" );
						uiPrintfEx(trINFO, "Client Read: (len %ld): ", pInstance->numOfReceivedSRSPbytes );
						fflush( stdout );

						if ( pthread_mutex_lock( &pInstance->clientSREQmutex ) != 0 )
						{
							uiPrintf( "[ERR] Mutex lock failed while handling SRSP\n" );
							exit( 1 );
						}

						if ( pInstance->srsp_msg )
						{
							// Unhandled SRSP message must be freed
							uiPrintf( "[ERR] Unhandled SRSP cleared\n" );
							free( pInstance->srsp_msg );
						}
						pInstance->srsp_msg = pMsg;
						pInstance->numOfReceivedSRSPbytes = pMsg->len + sizeof(hdrbuf);
						pthread_cond_signal( &pInstance->clientSREQcond );
						pthread_mutex_unlock( &pInstance->clientSREQmutex );
					}
					else if ( (pMsg->subSys & RPC_CMD_TYPE_MASK) == RPC_CMD_AREQ )
					{
						uiPrintfEx(trINFO, "RPC_CMD_AREQ cmdId: 0x%.2X\n", pMsg->cmdId );

						pInstance->areqRxMsgCount++;
						uiPrintfEx(trINFO, "\n[DBG] Allocated \t@ 0x%.16X"\
								" (received\040 %d messages)...\n",\
								(unsigned int)pMsg,\
								pInstance->areqRxMsgCount );

						uiPrintfEx(trINFO, "Filling new message (@ 0x%.16X)...\n",\
								(unsigned int)pMsg );
						if ( pthread_mutex_lock( &pInstance->clientAREQmutex ) != 0 )
						{
							uiPrintf( "[ERR] pthread_mutex_lock() failed"\
									" while processing AREQ\n" );
							exit( 1 );
						}

						// Place message in read list
						if ( pInstance->areq_rec_buf == NULL )
						{
							// First message in list
							pInstance->areq_rec_buf = pMsg;
						}
						else
						{
							areqMsg *searchList = pInstance->areq_rec_buf;

							// Find last entry and place it here
							while ( searchList->nextMessage != NULL )
							{
								searchList = searchList->nextMessage;
							}
							searchList->nextMessage = pMsg;
						}
						pthread_mutex_unlock( &pInstance->clientAREQmutex );
						// Flag semaphore
						sem_post( &pInstance->clientAREQsem );
					}
					else
					{
						// Cannot handle synchronous requests from RNP
						uiPrintf( "ERR: Received SREQ\n" );
						free( pMsg );
					}
				}
				else
				{
					// Possible if the socket connection is gone in the middle
					uiPrintf( "[ERR] Connection lost in the middle of reception\n"\
							"- n:%d, len:%d\n", n, pMsg->len );
					free( pMsg );
				}
			}
		}
		else
		{
			// Possible if the socket connection is gone in the middle
			uiPrintf( "[ERR] Connection lost in the middle of header reception"\
					" - n: %d\n", n );
			done = 1;
		}

	} while ( !done );

	// Flag semaphore to notify the callback thread that the receive
	// thread is terminated.
	pInstance->closed = TRUE;
	sem_post( &pInstance->clientAREQsem );

	return (ptr);
}

static void *SIShandleThreadFunc( void *ptr )
{
	int done = 0;
	apicInstance_t *pInstance = ptr;
	trace_init_thread("HNDL");

	// Handle message from socket
	do
	{
		int semresult;

		uiPrintfEx(trINFO, "[MUTEX] Wait for AREQ semaphore\n" );

		do
		{
			semresult = sem_wait( &pInstance->clientAREQsem );
			/* Repeat while interrupt by signal */
		} while ( semresult != 0 && errno == EINTR );

		if ( semresult != 0 )
		{
			uiPrintf( "[ERR] sem_wait() for AREQ receive failed\n" );
			exit( 1 );
		}

		if ( pthread_mutex_lock( &pInstance->clientAREQmutex ) != 0 )
		{
			uiPrintf( "[ERR] pthread_mutex_lock() for AREQ receive failed\n" );
			exit( 1 );
		}

		// Walk through all received AREQ messages before releasing MUTEX
		areqMsg *searchList = pInstance->areq_rec_buf;

		// Note that some may think of the following statement
		//	 areq_rec_buf = NULL;
		// here and process searchList as entire linked list
		// without waiting for semaphore to reduce the sem_wait() calls
		// during callbacks.
		// However, it increases the risk of semaphore counter overflow
		// and it does not actually reduce the number of sem_wait() calls.
		if ( searchList )
		{
			pInstance->areq_rec_buf = searchList->nextMessage;
		}

		// Note that the callback calls must not hold the receive thread
		// hostage and hence, the thread is freed here.
		pthread_mutex_unlock( &pInstance->clientAREQmutex );

		uiPrintfEx(trINFO, "[MUTEX] Mutex for AREQ unlocked\n" );

		if ( searchList != NULL )
		{
			uiPrintfEx(trINFO, "\n\n[DBG] Processing \t@ 0x%.16X\n",\
					(unsigned int)searchList );
			// Must remove command type before calling callback function
			searchList->subSys &= ~(RPC_CMD_TYPE_MASK);

			uiPrintfEx(trINFO, "[MUTEX] AREQ Calling asynchMsgCback (Handle)...\n" );

			asynchMsgCback( pInstance, searchList );

			if ( pInstance->freePending )
			{
				// application must have called apicClose() from within the callback
				free( pInstance );
				return NULL;
			}
			uiPrintfEx(trINFO, "[MUTEX] AREQ (Handle) (message @ 0x%.16X)...\n",\
					(unsigned int)searchList );
			pInstance->areqProcMsgCount++;
			uiPrintfEx(trINFO, "[DBG] Clearing \t\t@ 0x%.16X"\
					" (processed %d messages)...\n",\
					(unsigned int) searchList,\
					pInstance->areqProcMsgCount );
			free( searchList );
		}
		else if ( pInstance->closed )
		{
			if ( !pInstance->appClosed && pInstance->pfnAsyncMsgHandler )
			{
				// Notify that the connection was closed when application did not
				// trigger close.
				pInstance->pfnAsyncMsgHandler( pInstance, 0, 0, 0xFFFFu, NULL );
				if ( pInstance->freePending )
				{
					// apicClose() must have been called from within the callback.
					// free the instance.
					free( pInstance );
					return NULL;
				}
			}
			done = TRUE;
		}

	} while ( !done );

	return (ptr);
}

/**************************************************************************************************
 * @fn					asynchMsgCback
 *
 * @brief			 This function is a API callback to the client that inidcates an
 *							asynchronous message has been received. The client software is
 *							expected to complete this call.
 *
 *							Note: The client must copy this message if it requires it
 *										beyond the context of this call.
 *
 * input parameters
 *
 * @param			 pInstance - API client instance
 * @param			 *pMsg - A pointer to an asychronously received message.
 *
 * output parameters
 *
 * None.
 *
 * @return			None.
 **************************************************************************************************
 */
static int asynchMsgCback( apicInstance_t *pInstance, areqMsg *pMsg )
{
	if ( pMsg )
	{
		if ( pInstance->pfnAsyncMsgHandler )
		{
			uiPrintfEx(trINFO, "\n\n[DBG] asyncCB: subSys:0x%.16X, cmdId:0x%.16X, len:0x%.16X, pData:0x%.16X\n",\
					(unsigned int)pMsg->subSys,\
					(unsigned int)pMsg->cmdId,\
					(unsigned int)pMsg->len,\
					(unsigned int)(pMsg + 1) );
			pInstance->pfnAsyncMsgHandler( pInstance, pMsg->subSys, pMsg->cmdId,
					pMsg->len, (uint8 *) (pMsg + 1) );
		}
	}

	return (0);
}
