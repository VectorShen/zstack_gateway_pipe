#ifndef _CONFIG_H_
#define _CONFIG_H_

#define NPI_IPC_LISTEN_PIPE_SERVER2CLIENT   "/home/centos/fifos/npi_ipc_server2client"
#define NPI_IPC_LISTEN_PIPE_CLIENT2SERVER   "/home/centos/fifos/npi_ipc_client2server"

#define NPI_IPC_LISTEN_PIPE_CHECK_STRING    "connect_to_npi_ipc"

#define ZLSZNP_LISTEN_PIPE_SERVER2CLIENT    "/home/centos/fifos/zlsznp_server2client"
#define ZLSZNP_LISTEN_PIPE_CLIENT2SERVER    "/home/centos/fifos/zlsznp_client2server"

#define ZLSZNP_LISTEN_PIPE_CHECK_STRING     "connect_to_zlsznp"

#define NWKMGR_LISTEN_PIPE_SERVER2CLIENT    "/home/centos/fifos/nwkmgr_server2client"
#define NWKMGR_LISTEN_PIPE_CLIENT2SERVER    "/home/centos/fifos/nwkmgr_client2server"

#define NWKMGR_LISTEN_PIPE_CHECK_STRING     "connect_to_nwkmgr"

#define OTASERVER_LISTEN_PIPE_SERVER2CLIENT "/home/centos/fifos/otaserver_server2client"
#define OTASERVER_LISTEN_PIPE_CLIENT2SERVER "/home/centos/fifos/otaserver_client2server"

#define OTASERVER_LISTEN_PIPE_CHECK_STRING  "connect_to_otaserver"

#define GATEWAY_LISTEN_PIPE_SERVER2CLIENT   "/home/centos/fifos/gateway_server2client"
#define GATEWAY_LISTEN_PIPE_CLIENT2SERVER   "/home/centos/fifos/gateway_client2server"

#define GATEWAY_LISTEN_PIPE_CHECK_STRING    "connect_to_gateway"

#define SERVER_NPI_IPC                      "srv_npi_ipc"
#define SERVER_ZLSZNP                       "srv_zlsznp"
#define SERVER_NWKMGR                       "srv_nwkmgr"
#define SERVER_OTASERVER                    "srv_otaserver"
#define SERVER_GATEWAY                      "srv_gateway"

#define TMP_PIPE_NAME_SIZE				        50
#define TMP_ASSIGNED_ID_STRING_LEN              3
#define TMP_PIPE_CHECK_STRING_LEN               30
#define SERVER_LISTEN_BUF_SIZE			        30

typedef enum
{
    SERVER_NPI_IPC_ID,
    SERVER_ZLSZNP_ID,
    SERVER_NWKMGR_ID,
    SERVER_OTASERVER_ID,
    SERVER_GATEWAY_ID
}emServerId;

#endif /* _CONFIG_H_ */
