#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../STM32_NaMi_protocol/Inc/nami_hal_init.h"
#include "../../STM32_NaMi_protocol/Inc/nami_protocol.h"
#include "../../STM32_NaMi_protocol/Vendor/WIZnet/socket.h"
#include "../../STM32_NaMi_protocol/Vendor/WIZnet/wizchip_conf.h"

#include "main.h"

#define DISCOVERY_UDP_PORT 5005
#define SOCK_UDP_DISCOVERY 0
#define SOCK_TCP_CONTROL   1

static uint8_t data_buffer[2048];
static const NaMi_Device_Info* g_device_info = NULL;
static NaMi_CommandHandler g_command_handler = NULL;

typedef enum { STATE_LISTENING, STATE_CONNECTED, STATE_SESSION_ACTIVE } TcpServerState;
static TcpServerState tcp_state = STATE_LISTENING;
static char active_session_id[30];

static void process_nami_discovery(void)
{
    int32_t len = 0;
    uint8_t peer_ip[4];
    uint16_t peer_port;

    if ((len = getSn_RX_RSR(SOCK_UDP_DISCOVERY)) > 0)
    {
        len = recvfrom(SOCK_UDP_DISCOVERY, data_buffer, len, peer_ip, &peer_port);
        if (len <= 0) {
            return;
        }

        data_buffer[len] = '\0';
        if (strcmp((char*)data_buffer, "DISCOVER_NAMI_DEVICES") == 0)
        {
            cJSON *root = cJSON_CreateObject();
            if (!root) {
                return;
            }

            cJSON_AddStringToObject(root, "device_id", g_device_info->device_id);
            cJSON_AddStringToObject(root, "type", g_device_info->type);
            cJSON_AddStringToObject(root, "version", g_device_info->version);
            cJSON_AddStringToObject(root, "software_version", g_device_info->software_version);

            cJSON *protocols = cJSON_CreateArray();
            if (protocols) {
                cJSON_AddItemToArray(protocols, cJSON_CreateString("tcp_binary"));
                cJSON_AddItemToArray(protocols, cJSON_CreateString("uart"));
                cJSON_AddItemToObject(root, "protocols", protocols);
            }

            char* json_string = cJSON_PrintUnformatted(root);
            if (json_string) {
                sendto(SOCK_UDP_DISCOVERY, (uint8_t*)json_string, strlen(json_string), peer_ip, peer_port);
                free(json_string);
            }

            cJSON_Delete(root);
        }
    }
}


static void process_tcp_server(void)
{
    int32_t len = 0;
    switch(getSn_SR(SOCK_TCP_CONTROL))
    {
        case SOCK_ESTABLISHED:
            if(tcp_state == STATE_LISTENING) {
                if(getSn_IR(SOCK_TCP_CONTROL) & Sn_IR_CON) {
                    setSn_IR(SOCK_TCP_CONTROL, Sn_IR_CON);
                    tcp_state = STATE_CONNECTED;
                }
            }

            if((len = getSn_RX_RSR(SOCK_TCP_CONTROL)) > 0)
            {
                len = recv(SOCK_TCP_CONTROL, data_buffer, len);
                if (len > 0)
                {
                    data_buffer[len] = '\0';

                    cJSON *root = cJSON_Parse((char*)data_buffer);
                    if (root != NULL) {
                        cJSON *cmd_item = cJSON_GetObjectItem(root, "cmd");
                        if (cJSON_IsString(cmd_item))
                        {
                            if (strcmp(cmd_item->valuestring, "INIT") == 0 && tcp_state == STATE_CONNECTED)
                            {
                                snprintf(active_session_id, sizeof(active_session_id),
                                         "%s_sess_%ld", g_device_info->device_id, HAL_GetTick());

                                cJSON *resp_json = cJSON_CreateObject();
                                if (resp_json) {
                                    cJSON_AddStringToObject(resp_json, "status", "OK");
                                    cJSON_AddStringToObject(resp_json, "session_id", active_session_id);

                                    char* response_string = cJSON_PrintUnformatted(resp_json);
                                    if (response_string) {
                                        send(SOCK_TCP_CONTROL, (uint8_t*)response_string, strlen(response_string));
                                        free(response_string);
                                    }
                                    cJSON_Delete(resp_json);
                                }

                                tcp_state = STATE_SESSION_ACTIVE;
                            }
                            else if (tcp_state == STATE_SESSION_ACTIVE && g_command_handler != NULL)
                            {
                                g_command_handler(root, active_session_id);
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
            }
            break;

        case SOCK_CLOSE_WAIT:
            disconnect(SOCK_TCP_CONTROL);
            tcp_state = STATE_LISTENING;
            break;

        case SOCK_CLOSED:
            socket(SOCK_TCP_CONTROL, Sn_MR_TCP, g_device_info->tcp_port, 0);
            listen(SOCK_TCP_CONTROL);
            tcp_state = STATE_LISTENING;
            break;

        default:
        	break;
    }
}

int8_t NaMi_Init(const NaMi_Hardware_Config* hw_config, const NaMi_Network_Config* net_config, const NaMi_Device_Info* dev_info, NaMi_CommandHandler cmd_handler)
{
    g_device_info = dev_info;
    g_command_handler = cmd_handler;

    nami_hal_reset_w5500(hw_config);
    nami_hal_register_spi_callbacks(hw_config);
    
    uint8_t tx_size[] = {2, 2, 2, 2, 2, 2, 2, 2};
    uint8_t rx_size[] = {2, 2, 2, 2, 2, 2, 2, 2};
    wizchip_init(tx_size, rx_size);

    wiz_NetInfo net_info = {
        .mac = {0}, .ip = {0}, .sn = {0}, .gw = {0}, .dns = {0}, .dhcp = NETINFO_STATIC
    };
    memcpy(net_info.mac, net_config->mac, 6);
    memcpy(net_info.ip,  net_config->ip, 4);
    memcpy(net_info.sn,  net_config->subnet_mask, 4);
    memcpy(net_info.gw,  net_config->gateway, 4);
    wizchip_setnetinfo(&net_info);

    if (getVERSIONR() != 0x04) return -1;

    if (socket(SOCK_UDP_DISCOVERY, Sn_MR_UDP, DISCOVERY_UDP_PORT, 0) != SOCK_UDP_DISCOVERY) return -1;
    if (socket(SOCK_TCP_CONTROL, Sn_MR_TCP, g_device_info->tcp_port, 0) != SOCK_TCP_CONTROL) return -1;
    if (listen(SOCK_TCP_CONTROL) != SOCK_OK) {
        close(SOCK_TCP_CONTROL);
        return -1;
    }
    
    return 0;
}

void NaMi_Process(void)
{
    process_nami_discovery();
    process_tcp_server();
}
