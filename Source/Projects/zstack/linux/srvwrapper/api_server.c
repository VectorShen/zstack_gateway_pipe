/*********************************************************************
 Filename:			 api_server.c
 Revised:				$Date$
 Revision:			 $Revision$

 Description:		API Server module

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
 *********************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <pthread.h>
#include <bits/local_lim.h>
#include <errno.h>

#include "hal_rpc.h"
#include "api_server.h"
#include "trace.h"

/*********************************************************************
 * MACROS
 */
#if !defined( MAX )
#define MAX(a,b)		((a > b) ? a : b)
#endif

#define NPI_LNX_ERROR_MODULE(a)		 ((uint8)((a & 0xFF00) >> 8))
#define NPI_LNX_ERROR_THREAD(a)		 ((uint8)(a & 0x00FF))

/*********************************************************************
 * CONSTANTS
 */

#if !defined ( APIS_PIPE_QUEUE_SIZE )
#define APIS_PIPE_QUEUE_SIZE 4
#endif

#define APIS_LNX_SUCCESS 0
#define APIS_LNX_FAILURE 1

#define APIS_ERROR_PIPE_GET_ADDRESS_INFO                                                -1
#define APIS_ERROR_IPC_PIPE_SET_REUSE_ADDRESS                                           -2
#define APIS_ERROR_IPC_PIPE_BIND                                                        -3
#define APIS_ERROR_IPC_PIPE_LISTEN                                                      -4
#define APIS_ERROR_IPC_PIPE_SELECT_CHECK_ERRNO                                          -5
#define APIS_ERROR_IPC_PIPE_ACCEPT                                                      -6

#define APIS_ERROR_IPC_ADD_TO_ACTIVE_LIST_NO_ROOM						 				-7
#define APIS_ERROR_IPC_REMOVE_FROM_ACTIVE_LIST_NOT_FOUND								-8
#define APIS_ERROR_IPC_RECV_DATA_DISCONNECT									 			-9
#define APIS_ERROR_IPC_RECV_DATA_CHECK_ERRNO											-10
#define APIS_ERROR_IPC_RECV_DATA_TOO_FEW_BYTES											-11
#define APIS_ERROR_HAL_GPIO_WAIT_SRDY_CLEAR_POLL_TIMEDOUT		 						-12
#define APIS_ERROR_IPC_RECV_DATA_INCOMPATIBLE_CMD_TYPE									-13
#define APIS_ERROR_IPC_SEND_DATA_ALL													-14
#define APIS_ERROR_IPC_SEND_DATA_SPECIFIC_PIPE_REMOVED									-15
#define APIS_ERROR_IPC_SEND_DATA_SPECIFIC										 		-16
#define APIS_ERROR_MALLOC_FAILURE														-17

/*********************************************************************
 * TYPEDEFS
 */

/*********************************************************************
 * GLOBAL VARIABLES
 */
bool APIS_initialized = FALSE;
size_t APIS_threadStackSize = (PTHREAD_STACK_MIN * 3);	 // 16k*3

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */

int listenPipeReadHndl;
int listenPipeWriteHndl;

static int apisErrorCode = 0;

char *toAPISLnxLog = NULL;

fd_set activePipesFDs;
fd_set activePipesFDsSafeCopy;
int fdmax;

typedef struct _pipeinfo_t
{
	struct _pipeinfo_t *next;
	bool garbage;
	pthread_mutex_t mutex;
	int serverReadPipe;
	int serverWritePipe;
} Pipe_t;

int clientsNum = 1;

Pipe_t *activePipeList = NULL;
size_t activePipeCount = 0;
pthread_mutex_t aclMutex = PTHREAD_MUTEX_INITIALIZER;
size_t aclNoDeleteCount = 0;
pthread_cond_t aclDeleteAllowCond = PTHREAD_COND_INITIALIZER;

struct
{
	int list[APIS_PIPE_QUEUE_SIZE];
	int size;
} activePipes;

static pthread_t listeningThreadId;

static char* logPath = NULL;

static pfnAPISMsgCB apisMsgCB = NULL;

static apic16BitLenMsgHdr_t apisHdrBuf;
static char listenReadPipePathName[APIC_READWRITE_PIPE_NAME_LEN];
/*********************************************************************
 * LOCAL PROTOTYPES
 */

void *apislisteningThreadFunc( void *ptr );

static int apisSendData( uint16 len, uint8 *pData, int writePipe );
static int createReadWritePipes( emServerId serverId );
//static void apiServerExit( int exitCode );
static void startupInfo( void );
static void writetoAPISLnxLog( const char* str );
static int addToActiveList( int readPipe, int writePipe );
static int apisPipeHandle( int readPipe );
static int searchWritePipeFromReadPipe( int readPipe);

emServerId id;

/*********************************************************************
 * Public Functions
 */

/*********************************************************************
 * APIS_Init - Start and maintain an API Server
 *
 * public API is defined in api_server.h.
 *********************************************************************/
bool APIS_Init( emServerId serverId, bool verbose, pfnAPISMsgCB pfCB )
{
	id = serverId;
	pthread_attr_t attr;

	apisMsgCB = pfCB;

    printf(" serverId is %d.\n",id);

	if ( verbose )
	{
		startupInfo();
	}

	// prepare thread creation
	if ( pthread_attr_init( &attr ) )
	{
		perror( "pthread_attr_init" );
		return (FALSE);
	}

	if ( pthread_attr_setstacksize( &attr, APIS_threadStackSize ) )
	{
		perror( "pthread_attr_setstacksize" );
		return (FALSE);
	}

	// Create a listening socket
	if ( createReadWritePipes(serverId) < 0 )
	{
		return (FALSE);
	}

	if ( verbose )
	{
		uiPrintf( "waiting for connect pipe on #%d...\n", listenPipeReadHndl );
	}

    printf(" serverId is %d.\n",id);

	// iCreate thread for listening
	if ( pthread_create( &listeningThreadId, &attr, apislisteningThreadFunc,
			NULL ) )
	{
		// thread creation failed
		uiPrintf( "Failed to create an API Server Listening thread\n" );
		return (FALSE);
	}

	APIS_initialized = TRUE;

	return (TRUE);
}

/*********************************************************************
 * APIS_SendData - Send a packet to a pipe
 *
 * public API is defined in api_server.h.
 *********************************************************************/
void APIS_SendData( int writePipe, uint8 sysID, bool areq, uint8 cmdId,
										uint16 len, uint8 *pData )
{
	size_t msglen = len;
	apic16BitLenMsgHdr_t *pMsg;

	if ( len > (uint16)( len + sizeof(apic16BitLenMsgHdr_t) ) )
	{
		uiPrintf( "[ERR] APIS_SendData() called with length exceeding max allowed" );
		return;
	}
	msglen += sizeof(apic16BitLenMsgHdr_t);

	pMsg = malloc( msglen );
	if ( !pMsg )
	{
		// TODO: abort
		uiPrintf( "[ERR] APIS_SendData() failed to allocate memory block" );
		return;
	}

	pMsg->lenL = (uint8) len;
	pMsg->lenH = (uint8)( len >> 8 );
	if ( areq )
	{
		pMsg->subSys = (uint8)( RPC_CMD_AREQ | sysID );
	}
	else
	{
		pMsg->subSys = (uint8)( RPC_CMD_SRSP | sysID );
	}
	pMsg->cmdId = cmdId;

	memcpy( pMsg + 1, pData, len );

	apisSendData( (len + sizeof(apic16BitLenMsgHdr_t)), (uint8 *) pMsg,
			writePipe );

	free( pMsg );
}

/*********************************************************************
 * Local Functions
 */

/*********************************************************************
 * @fn			reserveActivePipe
 *
 * @brief	 Reserve an active pipe so that active pipe
 *					list does not change and also the send() to the active
 *					pipe is performed in sequence.
 *
 * @param	 pipe - pipe
 *
 * @return	0 when successful. -1 when failed.
 *
 *********************************************************************/
static int reserveActivePipe( int writePipe )
{
	Pipe_t *entry;

	if ( pthread_mutex_lock( &aclMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}
	entry = activePipeList;
	while ( entry )
	{
		if ( entry->serverWritePipe == writePipe )
		{
			aclNoDeleteCount++;
			pthread_mutex_unlock( &aclMutex );
			if ( pthread_mutex_lock( &entry->mutex ) != 0 )
			{
				perror( "pthread_mutex_lock" );
				exit( 1 );
			}
			return 0;
		}
		entry = entry->next;
	}
	pthread_mutex_unlock( &aclMutex );
	return -1;
}

/*********************************************************************
 * @fn			unreserveActivePipe
 *
 * @brief	 Free a reserved active pipe.
 *
 * @param	 pipe - pipe
 *
 * @return	None
 *
 *********************************************************************/
static void unreserveActivePipe( int writePipe )
{
	Pipe_t *entry;

	if ( pthread_mutex_lock( &aclMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}
	entry = activePipeList;
	while ( entry )
	{
		if ( entry->serverWritePipe == writePipe )
		{
			pthread_mutex_unlock( &entry->mutex );
			if ( --aclNoDeleteCount == 0 )
			{
				pthread_cond_signal( &aclDeleteAllowCond );
			}
			break;
		}
		entry = entry->next;
	}
	pthread_mutex_unlock( &aclMutex );
}

/*********************************************************************
 * @fn			dropActivePipe
 *
 * @brief	 Mark an active pipe as a garbage
 *
 * @param	 pipe - pipe
 *
 * @return	None
 *
 *********************************************************************/
static void dropActivePipe( int pipe )
{
	Pipe_t *entry;

	if ( pthread_mutex_lock( &aclMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}
	entry = activePipeList;
	while ( entry )
	{
		if ( entry->serverReadPipe == pipe || entry->serverWritePipe == pipe)
		{
			entry->garbage = TRUE;
            close( entry->serverReadPipe );
			close( entry->serverWritePipe );
			break;
		}
		entry = entry->next;
	}
	pthread_mutex_unlock( &aclMutex );
}

/*********************************************************************
 * @fn			aclGarbageCollect
 *
 * @brief	 Garbage collect connections from active pipe list
 *
 * @param	 None
 *
 * @return	None
 *
 *********************************************************************/
static void aclGarbageCollect( void )
{
	Pipe_t **entryPtr;
	Pipe_t *deletedlist = NULL;

	if ( pthread_mutex_lock( &aclMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}

	for ( ;; )
	{
		if ( !aclNoDeleteCount )
		{
			break;
		}
		pthread_cond_wait( &aclDeleteAllowCond, &aclMutex );
	}

	entryPtr = &activePipeList;
	while ( *entryPtr )
	{
		if ( (*entryPtr)->garbage )
		{
			Pipe_t *deleted = *entryPtr;
			*entryPtr = deleted->next;
			close( deleted->serverReadPipe );
            close( deleted->serverWritePipe );
			FD_CLR( deleted->serverReadPipe, &activePipesFDs );
			pthread_mutex_destroy( &deleted->mutex );
			deleted->next = deletedlist;
			deletedlist = deleted;
			continue;
		}
		entryPtr = &(*entryPtr)->next;
	}
	pthread_mutex_unlock( &aclMutex );

	// Indicate pipe is done
	while ( deletedlist )
	{
		Pipe_t *deleted = deletedlist;
		deletedlist = deletedlist->next;

		if ( apisMsgCB )
		{
			apisMsgCB( deleted->serverWritePipe, 0, 0, 0xFFFFu, NULL, SERVER_DISCONNECT );
		}
		free( deleted );
		activePipeCount--;
	}
}

/*********************************************************************
 * @fn			apisSendData
 *
 * @brief	 Send Packet
 *
 * @param	 len - length to send
 * @param	 pData - pointer to buffer to send
 * @param	 pipe - pipe to send message (for synchronous
 *											 response) otherwise -1 for all connections
 *
 * @return	status
 *
 *********************************************************************/
static int apisSendData( uint16 len, uint8 *pData, int writePipe )
{
	int bytesSent = 0;
	int ret = APIS_LNX_SUCCESS;

	printf("\napisSendData to pipeNum %d.\n", writePipe);

	// Send to all connections?
	if ( writePipe < 0 )
	{
		// retain the list
		Pipe_t *entry;

		if ( pthread_mutex_lock( &aclMutex ) != 0 )
		{
			perror( "pthread_mutex_lock" );
			exit( 1 );
		}

		entry = activePipeList;

		// Send data to all connections, except listener
		while ( entry )
		{
            // Repeat till all data is sent out
            if ( pthread_mutex_lock( &entry->mutex ) != 0 )
            {
                perror( "pthread_mutex_lock" );
                exit( 1 );
            }

            for ( ;; )
            {
				bytesSent = write( entry->serverWritePipe, pData, len );
				if ( bytesSent < 0 )
				{
					char *errorStr = (char *) malloc( 30 );
					sprintf( errorStr, "send %d, %d", entry->serverWritePipe, errno );
					perror( errorStr );
					// Remove from list if detected bad file descriptor
					if ( errno == EBADF )
					{
						uiPrintf( "Removing pipe #%d\n", entry->serverWritePipe );
						entry->garbage = TRUE;
					}
					else
					{
						apisErrorCode = APIS_ERROR_IPC_SEND_DATA_ALL;
						ret = APIS_LNX_FAILURE;
					}
				}
				else if ( bytesSent < len )
				{
					pData += bytesSent;
					len -= bytesSent;
					continue;
				}
				break;
            }
            pthread_mutex_unlock( &entry->mutex );
			entry = entry->next;
		}
		pthread_mutex_unlock( &aclMutex );
	}
	else
	{
		// Protect thread against asynchronous deletion
		if ( reserveActivePipe( writePipe ) == 0 )
		{

			// Repeat till all data is sent
			for ( ;; )
			{
				// Send to specific pipe only
				bytesSent = write( writePipe, pData, len );

				uiPrintfEx(trINFO, "...sent %d bytes to Client\n", bytesSent );

				if ( bytesSent < 0 )
				{
					perror( "send" );
					// Remove from list if detected bad file descriptor
					if ( errno == EBADF )
					{
						uiPrintf( "Removing pipe #%d\n", writePipe );
						dropActivePipe( writePipe );
						// Pipe closed. Remove from set
						apisErrorCode =
								APIS_ERROR_IPC_SEND_DATA_SPECIFIC_PIPE_REMOVED;
						ret = APIS_LNX_FAILURE;
					}
				}
				else if ( bytesSent < len )
				{
					pData += bytesSent;
					len -= bytesSent;
					continue;
				}
				break;
			}
			unreserveActivePipe( writePipe );
		}
		else
		{
			printf("reserveActivePipe failed for writePipe is %d.\n", writePipe);
		}
	}

	return (ret);
}

/*********************************************************************
 * @fn			createReadWritePipes
 *
 * @brief	 Create an API Server Listening socket
 *
 * @param	 ptr -
 *
 * @return	none
 *
 *********************************************************************/
static int createReadWritePipes( emServerId serverId )
{
	apisErrorCode = 0;

	//mkfifo and open pipes
	switch(serverId)
    {
        case SERVER_ZLSZNP_ID:
            strncpy(listenReadPipePathName,ZLSZNP_LISTEN_PIPE_CLIENT2SERVER,strlen(ZLSZNP_LISTEN_PIPE_CLIENT2SERVER));
            break;
        case SERVER_NWKMGR_ID:
            strncpy(listenReadPipePathName,NWKMGR_LISTEN_PIPE_CLIENT2SERVER,strlen(NWKMGR_LISTEN_PIPE_CLIENT2SERVER));
            break;
        case SERVER_OTASERVER_ID:
            strncpy(listenReadPipePathName,OTASERVER_LISTEN_PIPE_CLIENT2SERVER,strlen(OTASERVER_LISTEN_PIPE_CLIENT2SERVER));
            break;
        case SERVER_GATEWAY_ID:
            strncpy(listenReadPipePathName,GATEWAY_LISTEN_PIPE_CLIENT2SERVER,strlen(GATEWAY_LISTEN_PIPE_CLIENT2SERVER));
            break;
        default:
            //error
            break;
    }
	if ((mkfifo (listenReadPipePathName, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		printf ("cannot create fifo %s\n", listenReadPipePathName);
	}
	//非阻塞打开读管道，写管道的打开要等读管道接收到数据再操作
	listenPipeReadHndl = open (listenReadPipePathName, O_RDONLY | O_NONBLOCK, 0);
	if (listenPipeReadHndl == -1)
	{
		printf ("open %s for read error\n", listenReadPipePathName);
		exit (-1);
	}

	// Clear file descriptor sets
	FD_ZERO( &activePipesFDs );
	FD_ZERO( &activePipesFDsSafeCopy );

	// Add the listener to the set
	FD_SET( listenPipeReadHndl, &activePipesFDs );
	fdmax = listenPipeReadHndl;

	toAPISLnxLog = (char *) malloc( AP_MAX_BUF_LEN );

	return (apisErrorCode);
}

/*********************************************************************
 * @fn			apislisteningThreadFunc
 *
 * @brief	 API Server listening thread
 *
 * @param	 ptr -
 *
 * @return	none
 *
 *********************************************************************/
void *apislisteningThreadFunc( void *ptr )
{
	bool done = FALSE;
	int ret;
	int c;
    int n;
	struct timeval *pTimeout = NULL;
	char listen_buf[SERVER_LISTEN_BUF_SIZE];

	printf("serverId is %d.\n", id);

	char tmpReadPipeName[TMP_PIPE_NAME_SIZE];
	char tmpWritePipeName[TMP_PIPE_NAME_SIZE];
    char readPipePathName[APIS_READWRITE_PIPE_NAME_LEN];
	char writePipePathName[APIS_READWRITE_PIPE_NAME_LEN];
    char assignedId[APIS_ASSIGNED_ID_BUF_LEN];
	int tmpReadPipe;
	int tmpWritePipe;

    memset(writePipePathName,'\0',APIS_READWRITE_PIPE_NAME_LEN);
    memset(readPipePathName,'\0',APIS_READWRITE_PIPE_NAME_LEN);
    switch(id)
    {
        case SERVER_NPI_IPC_ID:
            strncpy(writePipePathName,NPI_IPC_LISTEN_PIPE_SERVER2CLIENT,strlen(NPI_IPC_LISTEN_PIPE_SERVER2CLIENT));
            strncpy(readPipePathName,NPI_IPC_LISTEN_PIPE_CLIENT2SERVER,strlen(NPI_IPC_LISTEN_PIPE_CLIENT2SERVER));
			break;
        case SERVER_ZLSZNP_ID:
            strncpy(writePipePathName,ZLSZNP_LISTEN_PIPE_SERVER2CLIENT,strlen(ZLSZNP_LISTEN_PIPE_SERVER2CLIENT));
            strncpy(readPipePathName,ZLSZNP_LISTEN_PIPE_CLIENT2SERVER,strlen(ZLSZNP_LISTEN_PIPE_CLIENT2SERVER));
            break;
        case SERVER_NWKMGR_ID:
            strncpy(writePipePathName,NWKMGR_LISTEN_PIPE_SERVER2CLIENT,strlen(NWKMGR_LISTEN_PIPE_SERVER2CLIENT));
            strncpy(readPipePathName,NWKMGR_LISTEN_PIPE_CLIENT2SERVER,strlen(NWKMGR_LISTEN_PIPE_CLIENT2SERVER));
            break;
        case SERVER_OTASERVER_ID:
            strncpy(writePipePathName,OTASERVER_LISTEN_PIPE_SERVER2CLIENT,strlen(OTASERVER_LISTEN_PIPE_SERVER2CLIENT));
            strncpy(readPipePathName,OTASERVER_LISTEN_PIPE_CLIENT2SERVER,strlen(OTASERVER_LISTEN_PIPE_CLIENT2SERVER));
            break;
        case SERVER_GATEWAY_ID:
            strncpy(writePipePathName,GATEWAY_LISTEN_PIPE_SERVER2CLIENT,strlen(GATEWAY_LISTEN_PIPE_SERVER2CLIENT));
            strncpy(readPipePathName,GATEWAY_LISTEN_PIPE_CLIENT2SERVER,strlen(GATEWAY_LISTEN_PIPE_CLIENT2SERVER));   
            break;
    }

	printf("server id is %d.\n", id);

	trace_init_thread("LSTN");
	//
	do
	{
		activePipesFDsSafeCopy = activePipesFDs;

		// First use select to find activity on the sockets
		if ( select( fdmax + 1, &activePipesFDsSafeCopy, NULL, NULL,
				pTimeout ) == -1 )
		{
			if ( errno != EINTR )
			{
				perror( "select" );
				apisErrorCode = APIS_ERROR_IPC_PIPE_SELECT_CHECK_ERRNO;
				ret = APIS_LNX_FAILURE;
				break;
			}
			continue;
		}

		// Then process this activity
		for ( c = 0; c <= fdmax; c++ )
		{
			if ( FD_ISSET( c, &activePipesFDsSafeCopy ) )
			{
                if( c == listenPipeReadHndl )
                {
					//接收客户端管道的数据
					memset(listen_buf,'\0',SERVER_LISTEN_BUF_SIZE);
					n = read( listenPipeReadHndl, listen_buf, SERVER_LISTEN_BUF_SIZE );
					if ( n <= 0 )
					{
						if ( n < 0 )
						{
							perror( "read" );
                            apisErrorCode = APIS_ERROR_IPC_RECV_DATA_CHECK_ERRNO;
                            ret = APIS_LNX_FAILURE;
						}
						else
						{
							uiPrintfEx(trINFO, "Will disconnect #%d\n", listenPipeReadHndl );
							printf("srvwapper server will reconnect\n");
							apisErrorCode = APIS_ERROR_IPC_RECV_DATA_DISCONNECT;
							//ret = APIS_LNX_FAILURE;
							close(listenPipeReadHndl);
							FD_CLR(listenPipeReadHndl, &activePipesFDs);
    						listenPipeReadHndl = open (listenReadPipePathName, O_RDONLY | O_NONBLOCK, 0);
    						if (listenPipeReadHndl == -1)
    						{
        						printf ("open %s for read error\n", listenReadPipePathName);
        						exit (-1);
    						}
                           	FD_SET( listenPipeReadHndl, &activePipesFDs );
                            if ( listenPipeReadHndl > fdmax )
                            {
                            	fdmax = listenPipeReadHndl;
                         	}
						}
					}
					else
					{
                        ret = APIS_LNX_SUCCESS;
						listen_buf[n]='\0';
						printf("read %d bytes from listenPipeReadHndl of string is %s.\n",n,listen_buf);
                        switch(id)
                        {
                            case SERVER_ZLSZNP_ID:
                                if(strncmp(listen_buf,ZLSZNP_LISTEN_PIPE_CHECK_STRING,strlen(ZLSZNP_LISTEN_PIPE_CHECK_STRING)))
                                {
                                    ret = APIS_LNX_FAILURE;
                                }
                                break;
                            case SERVER_NWKMGR_ID:
                                if(strncmp(listen_buf,NWKMGR_LISTEN_PIPE_CHECK_STRING,strlen(NWKMGR_LISTEN_PIPE_CHECK_STRING)))
                                {
                                    ret = APIS_LNX_FAILURE;
                                }
                                break;
                            case SERVER_OTASERVER_ID:
                                if(strncmp(listen_buf,OTASERVER_LISTEN_PIPE_CHECK_STRING,strlen(OTASERVER_LISTEN_PIPE_CHECK_STRING)))
                                {
                                    ret = APIS_LNX_FAILURE;
                                }
                                break;
                            case SERVER_GATEWAY_ID:
                                if(strncmp(listen_buf,GATEWAY_LISTEN_PIPE_CHECK_STRING,strlen(GATEWAY_LISTEN_PIPE_CHECK_STRING)))
                                {
                                    ret = APIS_LNX_FAILURE;
                                }
                                break;
                            case SERVER_NPI_IPC_ID:
                                if(strncmp(listen_buf,NPI_IPC_LISTEN_PIPE_CHECK_STRING,strlen(NPI_IPC_LISTEN_PIPE_CHECK_STRING)))
                                {
                                    ret = APIS_LNX_FAILURE;
                                }
                                break;
                        }
						if( ret == APIS_LNX_SUCCESS )
						{
							//是正确的客户端管道连接数据
							//打开写管道，写入数据，并打开相应编号管道的读写描述符，加入fd select的控制里
                            //阻塞打开，防止不同进程运行快慢问题
							printf("writePipePathName is %s.\n", writePipePathName);
							listenPipeWriteHndl=open(writePipePathName, O_WRONLY, 0);
							if(listenPipeWriteHndl==-1)
							{
								if(errno==ENXIO)
								{
									printf("open error; no reading process\n");	
									ret = APIS_LNX_FAILURE;
									break;
								}
							}
                            sprintf(assignedId,"%d",clientsNum);

							//更新管道文件名
							memset(tmpReadPipeName, '\0', TMP_PIPE_NAME_SIZE);
							memset(tmpWritePipeName, '\0', TMP_PIPE_NAME_SIZE);
							sprintf(tmpReadPipeName, "%s%d", readPipePathName, clientsNum);
							sprintf(tmpWritePipeName, "%s%d", writePipePathName, clientsNum);

                            clientsNum++;
                            
							//非阻塞创建管道
							if((mkfifo(tmpReadPipeName,O_CREAT|O_EXCL)<0)&&(errno!=EEXIST))
							{
								printf("cannot create fifo %s\n", tmpReadPipeName);
							}		
							if((mkfifo(tmpWritePipeName,O_CREAT|O_EXCL)<0)&&(errno!=EEXIST))
							{
								printf("cannot create fifo %s\n", tmpWritePipeName);
							}	
							//非阻塞打开读管道
							tmpReadPipe = open(tmpReadPipeName, O_RDONLY|O_NONBLOCK, 0);
							if(tmpReadPipe == -1)
							{
								printf("open %s for read error\n", tmpReadPipeName);
								ret = APIS_LNX_FAILURE;
								break;
							}
							
                            //写入监听管道
							write(listenPipeWriteHndl, assignedId, strlen(assignedId));

							//阻塞打开写管道
							tmpWritePipe = open(tmpWritePipeName, O_WRONLY, 0);
							if(tmpWritePipe == -1)
							{
								printf("open %s for write error\n", tmpWritePipeName);
								ret = APIS_LNX_FAILURE;
								break;
							}
							//读写管道描述符加入到activelist中
							ret = addToActiveList(tmpReadPipe, tmpWritePipe);
							if ( ret != APIS_LNX_SUCCESS )
							{
								// Adding to the active connection list failed.
								// Close the accepted connection.
								close( tmpReadPipe );
								close( tmpWritePipe );
							}
							else
							{
								FD_SET( tmpReadPipe, &activePipesFDs );
								if ( tmpReadPipe > fdmax )
								{
									fdmax = tmpReadPipe;
								}

								apisMsgCB( searchWritePipeFromReadPipe(tmpReadPipe), 0, 0, 0, NULL, SERVER_CONNECT );
							}
							//关闭监听时的写管道
							close(listenPipeWriteHndl);
						}
                        else
                        {
                            //error handle
                            printf("ret is failure\n");
                        }
					}
                }
                else
                {
                    ret = apisPipeHandle( c );
                    if ( ret == APIS_LNX_SUCCESS )
                    {
                        // Everything is ok
                    }
                    else
                    {
                        uint8 childThread;
                        switch ( apisErrorCode )
                        {
                            case APIS_ERROR_IPC_RECV_DATA_DISCONNECT:
                                uiPrintf( "Removing pipe #%d\n", c );

                                // Pipe closed. Remove from set
                                FD_CLR( c, &activePipesFDs );

                                // We should now set ret to APIS_LNX_SUCCESS, but there is still one fatal error
                                // possibility so simply set ret = to return value from removeFromActiveList().
                                dropActivePipe( c );

                                sprintf( toAPISLnxLog, "\t\tRemoved pipe #%d", c );
                                writetoAPISLnxLog( toAPISLnxLog );
                                break;

                            default:
                                if ( apisErrorCode == APIS_LNX_SUCCESS )
                                {
                                    // Do not report and abort if there is no real error.
                                    ret = APIS_LNX_SUCCESS;
                                }
                                else
                                {
                                    //							debug_
                                    uiPrintf( "[ERR] apisErrorCode 0x%.8X\n", apisErrorCode );

                                    // Everything about the error can be found in the message, and in npi_ipc_errno:
                                    childThread = apisHdrBuf.cmdId;

                                    sprintf( toAPISLnxLog, "Child thread with ID %d"
                                            " in module %d reported error",
                                            NPI_LNX_ERROR_THREAD( childThread ),
                                            NPI_LNX_ERROR_MODULE( childThread ));
                                    writetoAPISLnxLog( toAPISLnxLog );
                                }
                                break;
                        }
                    }
                }
			}
		}

		// Collect garbages
		// Note that for thread-safety garbage connections are collected
		// only from this single thread
		aclGarbageCollect();

	} while ( !done );

	return (ptr);
}

/*********************************************************************
 * @fn			startupInfo
 *
 * @brief	 Print/Display the pipe information
 *
 * @param	 none
 *
 * @return	none
 *
 *********************************************************************/
static void startupInfo( void )
{

}

/*********************************************************************
 * @fn			apiServerExit
 *
 * @brief	 Exit the API Server
 *
 * @param	 exitCode - reason for exit
 *
 * @return	none
 *
 *********************************************************************/
 /*
static void apiServerExit( int exitCode )
{
	// doesn't work yet, TBD
	//exit ( apisErrorCode );
}
*/
/*********************************************************************
 * @fn			writetoAPISLnxLog
 *
 * @brief	 Write a log file entry
 *
 * @param	 str - String to write
 *
 * @return	none
 *
 *********************************************************************/
static void writetoAPISLnxLog( const char* str )
{
	int APISLnxLogFd, i = 0;
	char *fullStr;
	char *inStr;

	time_t timeNow;
	struct tm * timeNowinfo;

	if ( logPath )
	{
		fullStr = (char *) malloc( 255 );
		inStr = (char *) malloc( 255 );

		time( &timeNow );
		timeNowinfo = localtime( &timeNow );

		sprintf( fullStr, "[%s", asctime( timeNowinfo ) );
		sprintf( inStr, "%s", str );

		// Remove \n characters
		fullStr[strlen( fullStr ) - 2] = 0;
		for ( i = strlen( str ) - 1; i > MAX( strlen( str ), 4 ); i-- )
		{
			if ( inStr[i] == '\n' )
			{
				inStr[i] = 0;
			}
		}
		sprintf( fullStr, "%s] %s", fullStr, inStr );

		// Add global error code
		sprintf( fullStr, "%s. Error: %.8X\n", fullStr, apisErrorCode );

		// Write error message to /dev/SSLnxLog
		APISLnxLogFd = open( logPath, (O_WRONLY | O_APPEND | O_CREAT), S_IRWXU );
		if ( APISLnxLogFd > 0 )
		{
			write( APISLnxLogFd, fullStr, strlen( fullStr ) + 1 );
		}
		else
		{
			uiPrintf( "Could not write \n%s\n to %s. Error: %.8X\n", str, logPath,
					errno );
			perror( "open" );
		}

		close( APISLnxLogFd );
		free( fullStr );
		free( inStr );
	}
}

static int searchWritePipeFromReadPipe( int readPipe)
{
    Pipe_t *entry;
    entry = activePipeList;
    while ( entry )
    {
        if ( entry->serverReadPipe == readPipe )
        {
			return entry->serverWritePipe;
        }
        entry = entry->next;
    }
    return -1;	
}

/*********************************************************************
 * @fn			addToActiveList
 *
 * @brief	Add a pipe to the active list
 *
 * @param	 c - pipe handle
 *
 * @return	APIS_LNX_SUCCESS or APIS_LNX_FAILURE
 *
 *********************************************************************/
static int addToActiveList( int readPipe, int writePipe )
{
	printf("addToActiveList read && write %d,%d.\n", readPipe, writePipe);
	if ( pthread_mutex_lock( &aclMutex ) != 0 )
	{
		perror( "pthread_mutex_lock" );
		exit( 1 );
	}
	if ( activePipeCount <= APIS_PIPE_QUEUE_SIZE )
	{
		// Entry at position activePipes.size is always the last available entry
		Pipe_t *newinfo = malloc( sizeof(Pipe_t) );
		newinfo->next = activePipeList;
		newinfo->garbage = FALSE;
		pthread_mutex_init( &newinfo->mutex, NULL );
		newinfo->serverReadPipe = readPipe;
		newinfo->serverWritePipe = writePipe;
		activePipeList = newinfo;

		// Increment size
		activePipeCount++;

		pthread_mutex_unlock( &aclMutex );

		return APIS_LNX_SUCCESS;
	}
	else
	{
		pthread_mutex_unlock( &aclMutex );

		// There's no more room in the list
		apisErrorCode = APIS_ERROR_IPC_ADD_TO_ACTIVE_LIST_NO_ROOM;

		return (APIS_LNX_FAILURE);
	}
}

/*********************************************************************
 * @fn			apisPipeHandle
 *
 * @brief	 Handle receiving a message
 *
 * @param	 c - pipe handle to remove
 *
 * @return	APIS_LNX_SUCCESS or APIS_LNX_FAILURE
 *
 *********************************************************************/
static int apisPipeHandle( int readPipe )
{
	int n;
	int ret = APIS_LNX_SUCCESS;

// Handle the pipe
	uiPrintfEx(trINFO, "Receive message...\n" );

// Receive only NPI header first. Then then number of bytes indicated by length.
	n = read( readPipe, &apisHdrBuf, sizeof(apisHdrBuf) );
	if ( n <= 0 )
	{
		if ( n < 0 )
		{
			perror( "read" );
            apisErrorCode = APIS_ERROR_IPC_RECV_DATA_CHECK_ERRNO;
            ret = APIS_LNX_FAILURE;
		}
		else
		{
			uiPrintfEx(trINFO, "Will disconnect #%d\n", readPipe );
			apisErrorCode = APIS_ERROR_IPC_RECV_DATA_DISCONNECT;
			ret = APIS_LNX_FAILURE;
		}
	}
	else if ( n == sizeof(apisHdrBuf) )
	{
		uint8 *buf = NULL;
		uint16 len = apisHdrBuf.lenL;
		len |= (uint16) apisHdrBuf.lenH << 8;

		// Now read out the payload of the NPI message, if it exists
		if ( len > 0 )
		{
			buf = malloc( len );

			if ( !buf )
			{
				uiPrintf( "[ERR] APIS receive thread memory allocation failure" );
				apisErrorCode = APIS_ERROR_MALLOC_FAILURE;
				return APIS_LNX_FAILURE;
			}

			n = read( readPipe, buf, len );
			if ( n != len )
			{
				uiPrintf( "[ERR] Could not read out the NPI payload."
						" Requested %d, but read %d!\n", len, n );

				apisErrorCode = APIS_ERROR_IPC_RECV_DATA_TOO_FEW_BYTES;
				ret = APIS_LNX_FAILURE;

				if ( n < 0 )
				{
					perror( "read" );
					// Disconnect this
					apisErrorCode = APIS_ERROR_IPC_RECV_DATA_DISCONNECT;
					ret = APIS_LNX_FAILURE;
				}
			}
			else
			{
				ret = APIS_LNX_SUCCESS;
			}
		}

		/*
		 * Take the message from the client and pass it to be processed
		 */
		if ( ret == APIS_LNX_SUCCESS )
		{
			if ( len != 0xFFFF )
			{
				if ( apisMsgCB )
				{
					apisMsgCB( searchWritePipeFromReadPipe(readPipe), apisHdrBuf.subSys, apisHdrBuf.cmdId, len, buf,
							SERVER_DATA );
				}
			}
			else
			{
				// len == 0xFFFF is used for pipe removal
				// hence is not supported.
				uiPrintf( "[WARN] APIS received message"
						" with size 0xFFFFu was discarded silently\n" );
			}
		}

		if ( buf )
		{
			free( buf );
		}
	}

	if ( (ret == APIS_LNX_FAILURE)
			&& (apisErrorCode == APIS_ERROR_IPC_RECV_DATA_DISCONNECT) )
	{
		uiPrintfEx(trINFO, "Done with %d\n", readPipe );
	}
	else
	{
		uiPrintfEx(trINFO, "!Done\n" );
	}

	return ret;
}

/*********************************************************************
 *********************************************************************/
