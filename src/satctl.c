/*
 * Copyright (c) 2014 Satlab ApS <satlab@satlab.com>
 * Proprietary software - All rights reserved.
 *
 * Satellite and subsystem control program
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <slash/slash.h>

#include <param/param.h>
#include <vmem/vmem_server.h>
#include <vmem/vmem_ram.h>

#include <csp/csp.h>
#include <csp/arch/csp_thread.h>
#include <csp/interfaces/csp_if_can.h>
#include <csp/interfaces/csp_if_kiss.h>
#include <csp/drivers/usart.h>
#include <csp/drivers/can_socketcan.h>
#include <param/param_list.h>
#include <param/param_group.h>
#include <param/param_server.h>

#define SATCTL_PROMPT_GOOD		"\033[96msatctl \033[90m%\033[0m "
#define SATCTL_PROMPT_BAD		"\033[96msatctl \033[31m!\033[0m "
#define SATCTL_DEFAULT_INTERFACE	"can0"
#define SATCTL_DEFAULT_ADDRESS		0

#define SATCTL_LINE_SIZE		128
#define SATCTL_HISTORY_SIZE		2048

void usage(void)
{
	printf("usage: satctl [command]\n");
	printf("\n");
	printf("Copyright (c) 2018 Space Inventor ApS <info@spaceinventor.com>\n");
	printf("Copyright (c) 2014 Satlab ApS <satlab@satlab.com>\n");
	printf("\n");
	printf("Options:\n");
	printf(" -i INTERFACE,\tUse INTERFACE as CAN interface\n");
	printf(" -n NODE\tUse NODE as own CSP address\n");
	printf(" -r REMOTE NODE\tUse NODE as remote CSP address for rparam\n");
	printf(" -h,\t\tPrint this help and exit\n");
}

int configure_csp(uint8_t addr, char *ifc)
{

	if (csp_buffer_init(100, 320) < 0)
		return -1;

	csp_conf_t csp_config;
	csp_conf_get_defaults(&csp_config);
	csp_config.address = addr;
	csp_config.hostname = "satctl";
	csp_config.model = "linux";
	if (csp_init(&csp_config) < 0)
		return -1;

	//csp_debug_set_level(4, 1);
	//csp_debug_set_level(5, 1);

#if 0
	struct usart_conf usart_conf = {
			.device = "/dev/ttyUSB0",
			.baudrate = 38400,
	};
	usart_init(&usart_conf);

	static csp_iface_t kiss_if;
	static csp_kiss_handle_t kiss_handle;
	void kiss_usart_putchar(char c) {
		usleep(4000);
		usart_putc(c);
	}
	void kiss_usart_callback(uint8_t *buf, int len, void *pxTaskWoken) {
		csp_kiss_rx(&kiss_if, buf, len, pxTaskWoken);
	}
	usart_set_callback(kiss_usart_callback);
	csp_kiss_init(&kiss_if, &kiss_handle, kiss_usart_putchar, NULL, "KISS");
#endif

	csp_iface_t *can0 = csp_can_socketcan_init(ifc, 1000000, 0);

	if (can0) {
		if (csp_route_set(CSP_DEFAULT_ROUTE, can0, CSP_NODE_MAC) < 0)
			return -1;
	}

	if (csp_route_start_task(0, 0) < 0)
		return -1;


	csp_rdp_set_opt(2, 10000, 2000, 1, 1000, 2);
	//csp_rdp_set_opt(10, 20000, 8000, 1, 5000, 9);

	csp_socket_t *sock_csh = csp_socket(CSP_SO_NONE);
	csp_socket_set_callback(sock_csh, csp_service_handler);
	csp_bind(sock_csh, CSP_ANY);

	csp_socket_t *sock_param = csp_socket(CSP_SO_NONE);
	csp_socket_set_callback(sock_param, param_serve);
	csp_bind(sock_param, PARAM_PORT_SERVER);

	csp_thread_handle_t vmem_handle;
	csp_thread_create(vmem_server_task, "vmem", 2000, NULL, 1, &vmem_handle);

	return 0;
}

int main(int argc, char **argv)
{
	static struct slash *slash;
	int remain, index, i, c, p = 0;
	char *ex;

	uint8_t addr = SATCTL_DEFAULT_ADDRESS;
	char *ifc = SATCTL_DEFAULT_INTERFACE;

	while ((c = getopt(argc, argv, "+hr:i:n:")) != -1) {
		switch (c) {
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		case 'i':
			ifc = optarg;
			break;
		case 'n':
			addr = atoi(optarg);
			break;
		default:
			exit(EXIT_FAILURE);
		}
	}

	remain = argc - optind;
	index = optind;

	param_list_store_file_load("param.cfg");
	param_group_store_file_load("group.cfg");

	if (configure_csp(addr, ifc) < 0) {
		fprintf(stderr, "Failed to init CSP\n");
		exit(EXIT_FAILURE);
	}

	slash = slash_create(SATCTL_LINE_SIZE, SATCTL_HISTORY_SIZE);
	if (!slash) {
		fprintf(stderr, "Failed to init slash\n");
		exit(EXIT_FAILURE);
	}

	/* Interactive or one-shot mode */
	if (remain > 0) {
		ex = malloc(SATCTL_LINE_SIZE);
		if (!ex) {
			fprintf(stderr, "Failed to allocate command memory");
			exit(EXIT_FAILURE);
		}

		/* Build command string */
		for (i = 0; i < remain; i++) {
			if (i > 0)
				p = ex - strncat(ex, " ", SATCTL_LINE_SIZE - p);
			p = ex - strncat(ex, argv[index + i], SATCTL_LINE_SIZE - p);
		}
		slash_execute(slash, ex);
		free(ex);
	} else {
		printf("\n\n");
		printf(" *******************************\n");
		printf(" **   SatCtl - Space Command  **\n");
		printf(" *******************************\n\n");

		printf(" Copyright (c) 2018 Space Inventor ApS <info@spaceinventor.com>\n");
		printf(" Copyright (c) 2014 Satlab ApS <satlab@satlab.com>\n\n");

		slash_loop(slash, SATCTL_PROMPT_GOOD, SATCTL_PROMPT_BAD);
	}

	slash_destroy(slash);

	return 0;
}
