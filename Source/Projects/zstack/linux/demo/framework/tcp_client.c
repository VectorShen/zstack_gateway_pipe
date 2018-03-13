/*******************************************************************************
 Filename:       tcp_client.h
 Revised:        $Date$
 Revision:       $Revision$

 Description:   Client communication via TCP ports

 Copyright 2013 Texas Instruments Incorporated. All rights reserved.

 IMPORTANT: Your use of this Software is limited to those specific rights
 granted under the terms of a software license agreement between the user
 who downloaded the software, his/her employer (which must be your employer)
 and Texas Instruments Incorporated (the "License").  You may not use this
 Software unless you agree to abide by the terms of the License. The License
 limits your use, and you acknowledge, that the Software may not be modified,
 copied or distributed unless used solely and exclusively in conjunction with
 a Texas Instruments radio frequency device, which is integrated into
 your product.  Other than for the foregoing purpose, you may not use,
 reproduce, copy, prepare derivative works of, modify, distribute, perform,
 display or sell this Software and/or its documentation for any purpose.

 YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
 PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED,l
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
*******************************************************************************/

/******************************************************************************
 * Includes
 *****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "tcp_client.h"
#include "polling.h"
#include "user_interface.h"
#include "types.h"

/******************************************************************************
 * Constants
 *****************************************************************************/
#define SERVER_RECONNECTION_RETRY_TIME 2000
#define MAX_TCP_PACKET_SIZE 2048

poll_pipe_write_t pollPipeWriteArray[POLL_SERVER_NUMS];

/******************************************************************************
 * Function Prototypes
 *****************************************************************************/
void tcp_socket_event_handler (server_details_t * server_details);
void tcp_socket_reconnect_to_server (server_details_t * server_details);

/******************************************************************************
 * Functions
 *****************************************************************************/

int searchPipeWirteFromRead (int readPipe)
{
	int i;
	for (i = 0; i < POLL_SERVER_NUMS; i++)
	{
		if (pollPipeWriteArray[i].pollReadFd == readPipe)
        {
            return pollPipeWriteArray[i].pipeWriteFd;
        }
	}
	return -1;
}

void clearArrayFromReadWritePipe (int pipe)
{
	int i;
	for (i = 0; i < POLL_SERVER_NUMS; i++)
	{
		if (pollPipeWriteArray[i].pollReadFd == pipe ||
			pollPipeWriteArray[i].pipeWriteFd == pipe)
        {
            pollPipeWriteArray[i].pollReadFd = -1;
            pollPipeWriteArray[i].pipeWriteFd = -1;
        }
	}
}

int tcp_send_packet (server_details_t * server_details, uint8_t * buf, int len)
{
	if (write
		(searchPipeWirteFromRead (polling_fds[server_details->fd_index].fd),
		 buf, len) != len)
	{
		return -1;
	}

	return 0;
}

int tcp_new_server_connection (server_details_t * server_details,
							 char *hostname,
							 server_incoming_data_handler_t
							 server_incoming_data_handler, char *name,
							 server_connected_disconnected_handler_t
							 server_connected_disconnected_handler)
{
	server_details->hostname = hostname;
	server_details->server_incoming_data_handler = server_incoming_data_handler;
	server_details->server_reconnection_timer.in_use = false;
	server_details->name = name;
	server_details->server_connected_disconnected_handler =
		server_connected_disconnected_handler;
	server_details->connected = false;

	tcp_socket_reconnect_to_server (server_details);

	return 0;
}

int tcp_disconnect_from_server (server_details_t * server)
{
	//TBD
	return 0;
}

int tcp_connect_to_server (server_details_t * server_details)
{
	int n;
	int i;
	int tmpReadPipe;
	int tmpWritePipe;
	char tmpReadPipePath[TMP_PIPE_NAME_SIZE];
	char tmpWritePipePath[TMP_PIPE_NAME_SIZE];
	char tmpCheckString[TMP_PIPE_CHECK_STRING_LEN];
	char assignedIdBuf[TMP_ASSIGNED_ID_STRING_LEN];
	int readWriteNum;

	memset (tmpReadPipePath, '\0', TMP_PIPE_NAME_SIZE);
	memset (tmpWritePipePath, '\0', TMP_PIPE_NAME_SIZE);
	memset (tmpCheckString, '\0', TMP_PIPE_CHECK_STRING_LEN);
	memset (assignedIdBuf, '\0', TMP_ASSIGNED_ID_STRING_LEN);

	//比较host名，打开监听管道
	if (!strncmp (server_details->hostname, SERVER_NPI_IPC,
				strlen (SERVER_NPI_IPC)))
	{
		strncpy (tmpReadPipePath, NPI_IPC_LISTEN_PIPE_SERVER2CLIENT,
				 strlen (NPI_IPC_LISTEN_PIPE_SERVER2CLIENT));
		strncpy (tmpWritePipePath, NPI_IPC_LISTEN_PIPE_CLIENT2SERVER,
				 strlen (NPI_IPC_LISTEN_PIPE_CLIENT2SERVER));
		strncpy (tmpCheckString, NPI_IPC_LISTEN_PIPE_CHECK_STRING,
				 strlen (NPI_IPC_LISTEN_PIPE_CHECK_STRING));
	}
	else if (!strncmp (server_details->hostname, SERVER_ZLSZNP,
					 strlen (SERVER_ZLSZNP)))
	{
		strncpy (tmpReadPipePath, ZLSZNP_LISTEN_PIPE_SERVER2CLIENT,
				 strlen (ZLSZNP_LISTEN_PIPE_SERVER2CLIENT));
		strncpy (tmpWritePipePath, ZLSZNP_LISTEN_PIPE_CLIENT2SERVER,
				 strlen (ZLSZNP_LISTEN_PIPE_CLIENT2SERVER));
		strncpy (tmpCheckString, ZLSZNP_LISTEN_PIPE_CHECK_STRING,
				 strlen (ZLSZNP_LISTEN_PIPE_CHECK_STRING));
	}
	else if (!strncmp (server_details->hostname, SERVER_NWKMGR,
					 strlen (SERVER_NWKMGR)))
	{
		strncpy (tmpReadPipePath, NWKMGR_LISTEN_PIPE_SERVER2CLIENT,
				 strlen (NWKMGR_LISTEN_PIPE_SERVER2CLIENT));
		strncpy (tmpWritePipePath, NWKMGR_LISTEN_PIPE_CLIENT2SERVER,
				 strlen (NWKMGR_LISTEN_PIPE_CLIENT2SERVER));
		strncpy (tmpCheckString, NWKMGR_LISTEN_PIPE_CHECK_STRING,
				 strlen (NWKMGR_LISTEN_PIPE_CHECK_STRING));
	}
	else if (!strncmp (server_details->hostname, SERVER_OTASERVER,
					 strlen (SERVER_OTASERVER)))
	{
		strncpy (tmpReadPipePath, OTASERVER_LISTEN_PIPE_SERVER2CLIENT,
				 strlen (OTASERVER_LISTEN_PIPE_SERVER2CLIENT));
		strncpy (tmpWritePipePath, OTASERVER_LISTEN_PIPE_CLIENT2SERVER,
				 strlen (OTASERVER_LISTEN_PIPE_CLIENT2SERVER));
		strncpy (tmpCheckString, OTASERVER_LISTEN_PIPE_CHECK_STRING,
				 strlen (OTASERVER_LISTEN_PIPE_CHECK_STRING));
	}
	else if (!strncmp (server_details->hostname, SERVER_GATEWAY,
					 strlen (SERVER_GATEWAY)))
	{
		strncpy (tmpReadPipePath, GATEWAY_LISTEN_PIPE_SERVER2CLIENT,
				 strlen (GATEWAY_LISTEN_PIPE_SERVER2CLIENT));
		strncpy (tmpWritePipePath, GATEWAY_LISTEN_PIPE_CLIENT2SERVER,
				 strlen (GATEWAY_LISTEN_PIPE_CLIENT2SERVER));
		strncpy (tmpCheckString, GATEWAY_LISTEN_PIPE_CHECK_STRING,
				 strlen (GATEWAY_LISTEN_PIPE_CHECK_STRING));
	}

	UI_PRINT_LOG ("readPipePathName is %s\n", tmpReadPipePath);
	UI_PRINT_LOG ("writePipePathName is %s\n", tmpWritePipePath);

	//mkfifo
	if ((mkfifo (tmpReadPipePath, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		UI_PRINT_LOG ("cannot create fifo %s\n", tmpReadPipePath);
	}
	if ((mkfifo (tmpWritePipePath, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		UI_PRINT_LOG ("cannot create fifo %s\n", tmpWritePipePath);
	}
	//阻塞打开写监听管道
	tmpWritePipe = open (tmpWritePipePath, O_WRONLY, 0);
	if (tmpWritePipe == -1)
	{
		//error
		UI_PRINT_LOG ("open tmpWritePipe failed.\n");
	}

	//写入管道
	n = write (tmpWritePipe, tmpCheckString, strlen (tmpCheckString));
	UI_PRINT_LOG ("write to tmpWritePipe checkString %d.\n", n);

	//阻塞打开读监听管道
	tmpReadPipe = open (tmpReadPipePath, O_RDONLY, 0);
	if (tmpReadPipe == -1)
	{
		//error
		UI_PRINT_LOG ("open readPipePathName failed\n");
	}

	UI_PRINT_LOG ("successfully open tmpReadPipePath.\n");

	//读取分配的id
	readWriteNum =
		read (tmpReadPipe, assignedIdBuf, TMP_ASSIGNED_ID_STRING_LEN);
	UI_PRINT_LOG ("readWriteNum is %d.\n", readWriteNum);
	if (readWriteNum <= 0)
	{
		//error
	}
	//附属到默认管道名后面作为一个临时名字
	strcat (tmpReadPipePath, assignedIdBuf);
	strcat (tmpWritePipePath, assignedIdBuf);
	UI_PRINT_LOG ("readPipePathName is %s\n", tmpReadPipePath);
	UI_PRINT_LOG ("writePipePathName is %s\n", tmpWritePipePath);

	UI_PRINT_LOG ("Trying to open regular pipes...\n");
	if ((mkfifo (tmpReadPipePath, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		UI_PRINT_LOG ("cannot create fifo %s\n", tmpReadPipePath);
	}
	if ((mkfifo (tmpWritePipePath, O_CREAT | O_EXCL) < 0) && (errno != EEXIST))
	{
		UI_PRINT_LOG ("cannot create fifo %s\n", tmpWritePipePath);
	}

	close (tmpReadPipe);
	close (tmpWritePipe);

	tmpReadPipe = open (tmpReadPipePath, O_RDONLY, 0);
	if (tmpReadPipe == -1)
	{
		//error
		UI_PRINT_LOG ("open readPipePathName failed\n");
	}

	tmpWritePipe = open (tmpWritePipePath, O_WRONLY, 0);
	if (tmpWritePipe == -1)
	{
		//error
		UI_PRINT_LOG ("open tmpWritePipe failed.\n");
	}

	//加入全局描述符记录数组
	for (i = 0; i < POLL_SERVER_NUMS; i++)
	{
		if (pollPipeWriteArray[i].pollReadFd == -1 &&
			pollPipeWriteArray[i].pipeWriteFd == -1)
        {
            //加入数组
            pollPipeWriteArray[i].pollReadFd = tmpReadPipe;
            pollPipeWriteArray[i].pipeWriteFd = tmpWritePipe;
            break;
        }
	}

	if (i == POLL_SERVER_NUMS)
	{
		//error happens
	}

	if ((server_details->fd_index =
		 polling_define_poll_fd (pollPipeWriteArray[i].pollReadFd, POLLIN,
								 (event_handler_cb_t) tcp_socket_event_handler,
								 server_details)) != -1)
	{
		UI_PRINT_LOG ("Successfully connected to %s server",
						server_details->name);

		server_details->connected = true;

		if (server_details->server_connected_disconnected_handler != NULL)
        {
            server_details->server_connected_disconnected_handler ();
        }
		return 0;
	}

	return -1;
}

void tcp_socket_reconnect_to_server (server_details_t * server_details)
{
	if (tcp_connect_to_server (server_details) == -1)
	{
		tu_set_timer (&server_details->server_reconnection_timer,
						SERVER_RECONNECTION_RETRY_TIME, false,
						(timer_handler_cb_t) tcp_socket_reconnect_to_server,
						server_details);
	}
	else
	{
		ui_redraw_server_state ();
	}
}

void tcp_socket_event_handler (server_details_t * server_details)
{
	char buf[MAX_TCP_PACKET_SIZE];
	pkt_buf_t *pkt_ptr = (pkt_buf_t *) buf;
	int remaining_len;
	char tmp_string[80];

	bzero (buf, MAX_TCP_PACKET_SIZE);
	remaining_len =
		read (polling_fds[server_details->fd_index].fd, buf,
			MAX_TCP_PACKET_SIZE - 1);

	if (remaining_len < 0)
	{
		UI_PRINT_LOG ("ERROR reading from socket (server %s)",
						server_details->name);
	}
	else if (remaining_len == 0)
	{
		UI_PRINT_LOG ("Server %s disconnected", server_details->name);
		close (polling_fds[server_details->fd_index].fd);
		close (searchPipeWirteFromRead
				 (polling_fds[server_details->fd_index].fd));
		clearArrayFromReadWritePipe (polling_fds[server_details->fd_index].
									 fd);
		polling_undefine_poll_fd (server_details->fd_index);
		server_details->connected = false;

		if (server_details->server_connected_disconnected_handler != NULL)
        {
            server_details->server_connected_disconnected_handler ();
        }

		ui_redraw_server_state ();
		tcp_socket_reconnect_to_server (server_details);
	}
	else
	{
		while (remaining_len > 0)
        {
            if (remaining_len < sizeof (pkt_ptr->header))
            {
                UI_PRINT_LOG
                    ("%sERROR: Packet header incomplete. expected_len=%d, actual_len=%d",
                        DARK_RED, sizeof (pkt_ptr->header), remaining_len);
            }
            else if (remaining_len < (pkt_ptr->header.len + 4))
            {
                UI_PRINT_LOG
                    ("%sERROR: Packet truncated. expected_len=%d, actual_len=%d",
                        DARK_RED, (pkt_ptr->header.len + 4), remaining_len);
            }
            else
            {
                STRING_START (tmp_string, "received from %s: ",
                                server_details->name);
                ui_print_packet_to_log (pkt_ptr, tmp_string,
                                        BOLD UNDERSCORE);

                server_details->server_incoming_data_handler (pkt_ptr,
                                                                remaining_len);

                remaining_len -= (pkt_ptr->header.len + 4);
                pkt_ptr =
                    ((pkt_buf_t *) (((uint8_t *) pkt_ptr) +
                                    (pkt_ptr->header.len + 4)));

                if (remaining_len > 0)
                {
                    UI_PRINT_LOG
                        ("Additional API command in the same TCP packet");
                }
            }
        }
	}
}
