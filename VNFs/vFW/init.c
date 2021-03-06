/*
// Copyright (c) 2017 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_eal.h>
#include <rte_malloc.h>
#include <rte_version.h>

#include "app.h"
#include "pipeline.h"
#include "pipeline_common_fe.h"
#include "pipeline_master.h"
#include "pipeline_passthrough.h"
#include "thread_fe.h"
#include "pipeline_vfw.h"
#include "pipeline_loadb.h"
#include "pipeline_txrx.h"
#include "pipeline_arpicmp.h"
#include "interface.h"
#include "l3fwd_common.h"
#include "l3fwd_lpm4.h"
#include "l3fwd_lpm6.h"
#include "lib_arp.h"
#include "vnf_define.h"
#define APP_NAME_SIZE	32
port_config_t *port_config;

static void
app_init_core_map(struct app_params *app)
{
	APP_LOG(app, HIGH, "Initializing CPU core map ...");
	app->core_map = cpu_core_map_init(4, 32, 4, 0);

	if (app->core_map == NULL)
		rte_panic("Cannot create CPU core map\n");

	if (app->log_level >= APP_LOG_LEVEL_LOW)
		cpu_core_map_print(app->core_map);
}

/* Core Mask String in Hex Representation */
#define APP_CORE_MASK_STRING_SIZE ((64 * APP_CORE_MASK_SIZE) / 8 * 2 + 1)

static void
app_init_core_mask(struct app_params *app)
{
	char core_mask_str[APP_CORE_MASK_STRING_SIZE];
	uint32_t i;

	for (i = 0; i < app->n_pipelines; i++) {
		struct app_pipeline_params *p = &app->pipeline_params[i];
		int lcore_id;

		lcore_id = cpu_core_map_get_lcore_id(app->core_map,
			p->socket_id,
			p->core_id,
			p->hyper_th_id);

		if (lcore_id < 0)
			rte_panic("Cannot create CPU core mask\n");

		app_core_enable_in_core_mask(app, lcore_id);
	}

       app_core_build_core_mask_string(app, core_mask_str);
       APP_LOG(app, HIGH, "CPU core mask = 0x%s", core_mask_str);

}

static void
app_init_eal(struct app_params *app)
{
	char buffer[256];
	char core_mask_str[APP_CORE_MASK_STRING_SIZE];
	struct app_eal_params *p = &app->eal_params;
	uint8_t n_args = 0;
	uint32_t i;
	int status;

	app->eal_argv[n_args++] = strdup(app->app_name);

	app_core_build_core_mask_string(app, core_mask_str);
	snprintf(buffer, sizeof(buffer), "-c%s", core_mask_str);
	app->eal_argv[n_args++] = strdup(buffer);

	if (p->coremap) {
		snprintf(buffer, sizeof(buffer), "--lcores=%s", p->coremap);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->master_lcore_present) {
		snprintf(buffer,
			sizeof(buffer),
			"--master-lcore=%" PRIu32,
			p->master_lcore);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	snprintf(buffer, sizeof(buffer), "-n%" PRIu32, p->channels);
	app->eal_argv[n_args++] = strdup(buffer);

	if (p->memory_present) {
		snprintf(buffer, sizeof(buffer), "-m%" PRIu32, p->memory);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->ranks_present) {
		snprintf(buffer, sizeof(buffer), "-r%" PRIu32, p->ranks);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	for (i = 0; i < APP_MAX_LINKS; i++) {
		if (p->pci_blacklist[i] == NULL)
			break;

		snprintf(buffer,
			sizeof(buffer),
			"--pci-blacklist=%s",
			p->pci_blacklist[i]);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (app->port_mask != 0)
		for (i = 0; i < APP_MAX_LINKS; i++) {
			if (p->pci_whitelist[i] == NULL)
				break;

			snprintf(buffer,
				sizeof(buffer),
				"--pci-whitelist=%s",
				p->pci_whitelist[i]);
			app->eal_argv[n_args++] = strdup(buffer);
		}
	else
		for (i = 0; i < app->n_links; i++) {
			char *pci_bdf = app->link_params[i].pci_bdf;

			snprintf(buffer,
				sizeof(buffer),
				"--pci-whitelist=%s",
				pci_bdf);
			app->eal_argv[n_args++] = strdup(buffer);
		}

	for (i = 0; i < APP_MAX_LINKS; i++) {
		if (p->vdev[i] == NULL)
			break;

		snprintf(buffer,
			sizeof(buffer),
			"--vdev=%s",
			p->vdev[i]);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->vmware_tsc_map_present) && p->vmware_tsc_map) {
		snprintf(buffer, sizeof(buffer), "--vmware-tsc-map");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->proc_type) {
		snprintf(buffer,
			sizeof(buffer),
			"--proc-type=%s",
			p->proc_type);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->syslog) {
		snprintf(buffer, sizeof(buffer), "--syslog=%s", p->syslog);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->log_level_present) {
		snprintf(buffer,
			sizeof(buffer),
			"--log-level=%" PRIu32,
			p->log_level);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->version_present) && p->version) {
		snprintf(buffer, sizeof(buffer), "-v");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->help_present) && p->help) {
		snprintf(buffer, sizeof(buffer), "--help");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->no_huge_present) && p->no_huge) {
		snprintf(buffer, sizeof(buffer), "--no-huge");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->no_pci_present) && p->no_pci) {
		snprintf(buffer, sizeof(buffer), "--no-pci");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->no_hpet_present) && p->no_hpet) {
		snprintf(buffer, sizeof(buffer), "--no-hpet");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->no_shconf_present) && p->no_shconf) {
		snprintf(buffer, sizeof(buffer), "--no-shconf");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->add_driver) {
		snprintf(buffer, sizeof(buffer), "-d=%s", p->add_driver);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->socket_mem) {
		snprintf(buffer,
			sizeof(buffer),
			"--socket-mem=%s",
			p->socket_mem);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->huge_dir) {
		snprintf(buffer, sizeof(buffer), "--huge-dir=%s", p->huge_dir);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->file_prefix) {
		snprintf(buffer,
			sizeof(buffer),
			"--file-prefix=%s",
			p->file_prefix);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->base_virtaddr) {
		snprintf(buffer,
			sizeof(buffer),
			"--base-virtaddr=%s",
			p->base_virtaddr);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->create_uio_dev_present) && p->create_uio_dev) {
		snprintf(buffer, sizeof(buffer), "--create-uio-dev");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if (p->vfio_intr) {
		snprintf(buffer,
			sizeof(buffer),
			"--vfio-intr=%s",
			p->vfio_intr);
		app->eal_argv[n_args++] = strdup(buffer);
	}

	if ((p->xen_dom0_present) && (p->xen_dom0)) {
		snprintf(buffer, sizeof(buffer), "--xen-dom0");
		app->eal_argv[n_args++] = strdup(buffer);
	}

	snprintf(buffer, sizeof(buffer), "--");
	app->eal_argv[n_args++] = strdup(buffer);

	app->eal_argc = n_args;

	APP_LOG(app, HIGH, "Initializing EAL ...");
	if (app->log_level >= APP_LOG_LEVEL_LOW) {
		int i;

		fprintf(stdout, "[APP] EAL arguments: \"");
		for (i = 1; i < app->eal_argc; i++)
			fprintf(stdout, "%s ", app->eal_argv[i]);
		fprintf(stdout, "\"\n");
	}

	status = rte_eal_init(app->eal_argc, app->eal_argv);
	if (status < 0)
		rte_panic("EAL init error\n");
}
static inline int
app_link_filter_arp_add(struct app_link_params *link)
{
	struct rte_eth_ethertype_filter filter = {
		.ether_type = ETHER_TYPE_ARP,
		.flags = 0,
		.queue = link->arp_q,
	};

	return rte_eth_dev_filter_ctrl(link->pmd_id,
		RTE_ETH_FILTER_ETHERTYPE,
		RTE_ETH_FILTER_ADD,
		&filter);
}

static inline int
app_link_filter_tcp_syn_add(struct app_link_params *link)
{
	struct rte_eth_syn_filter filter = {
		.hig_pri = 1,
		.queue = link->tcp_syn_q,
	};

	return rte_eth_dev_filter_ctrl(link->pmd_id,
		RTE_ETH_FILTER_SYN,
		RTE_ETH_FILTER_ADD,
		&filter);
}

static inline int
app_link_filter_ip_add(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = 0,
		.proto_mask = 0, /* Disable */
		.tcp_flags = 0,
		.priority = 1, /* Lowest */
		.queue = l1->ip_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_ADD,
		&filter);
}

static inline int
app_link_filter_ip_del(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = 0,
		.proto_mask = 0, /* Disable */
		.tcp_flags = 0,
		.priority = 1, /* Lowest */
		.queue = l1->ip_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_DELETE,
		&filter);
}

static inline int
app_link_filter_tcp_add(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = IPPROTO_TCP,
		.proto_mask = UINT8_MAX, /* Enable */
		.tcp_flags = 0,
		.priority = 2, /* Higher priority than IP */
		.queue = l1->tcp_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_ADD,
		&filter);
}

static inline int
app_link_filter_tcp_del(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = IPPROTO_TCP,
		.proto_mask = UINT8_MAX, /* Enable */
		.tcp_flags = 0,
		.priority = 2, /* Higher priority than IP */
		.queue = l1->tcp_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_DELETE,
		&filter);
}

static inline int
app_link_filter_udp_add(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = IPPROTO_UDP,
		.proto_mask = UINT8_MAX, /* Enable */
		.tcp_flags = 0,
		.priority = 2, /* Higher priority than IP */
		.queue = l1->udp_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_ADD,
		&filter);
}

static inline int
app_link_filter_udp_del(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = IPPROTO_UDP,
		.proto_mask = UINT8_MAX, /* Enable */
		.tcp_flags = 0,
		.priority = 2, /* Higher priority than IP */
		.queue = l1->udp_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_DELETE,
		&filter);
}

static inline int
app_link_filter_sctp_add(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = IPPROTO_SCTP,
		.proto_mask = UINT8_MAX, /* Enable */
		.tcp_flags = 0,
		.priority = 2, /* Higher priority than IP */
		.queue = l1->sctp_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_ADD,
		&filter);
}

static inline int
app_link_filter_sctp_del(struct app_link_params *l1, struct app_link_params *l2)
{
	struct rte_eth_ntuple_filter filter = {
		.flags = RTE_5TUPLE_FLAGS,
		.dst_ip = rte_bswap32(l2->ip),
		.dst_ip_mask = UINT32_MAX, /* Enable */
		.src_ip = 0,
		.src_ip_mask = 0, /* Disable */
		.dst_port = 0,
		.dst_port_mask = 0, /* Disable */
		.src_port = 0,
		.src_port_mask = 0, /* Disable */
		.proto = IPPROTO_SCTP,
		.proto_mask = UINT8_MAX, /* Enable */
		.tcp_flags = 0,
		.priority = 2, /* Higher priority than IP */
		.queue = l1->sctp_local_q,
	};

	return rte_eth_dev_filter_ctrl(l1->pmd_id,
		RTE_ETH_FILTER_NTUPLE,
		RTE_ETH_FILTER_DELETE,
		&filter);
}

/* rte_eth_dev is removed in DPDK version 16.11 and onwards */
#if RTE_VERSION < 0x100b0000
static int
app_link_is_virtual(struct app_link_params *p)
{
        uint32_t pmd_id = p->pmd_id;
        struct rte_eth_dev *dev = &rte_eth_devices[pmd_id];

        if (dev->dev_type == RTE_ETH_DEV_VIRTUAL)
                return 1;

        return 0;
}
#endif

void
app_link_up_internal(__rte_unused struct app_params *app,
		struct app_link_params *cp)
{
	if(app == NULL || cp == NULL)
		printf("NULL Pointers");

#if RTE_VERSION < 0x100b0000
        if (app_link_is_virtual(cp)) {
                cp->state = 1;
                return;
        }
#endif
	ifm_update_linkstatus(cp->pmd_id, IFM_ETH_LINK_UP);

	/* Mark link as UP */
	cp->state = 1;
}

void
app_link_down_internal(__rte_unused struct app_params *app,
		struct app_link_params *cp)
{
	if(app == NULL || cp == NULL)
		printf("NULL Pointers");

#if RTE_VERSION < 0x100b0000
        if (app_link_is_virtual(cp)) {
                cp->state = 0;
                return;
        }
#endif
	ifm_update_linkstatus(cp->pmd_id, IFM_ETH_LINK_DOWN);
	/* Mark link as DOWN */
	cp->state = 0;

}

static void
app_check_link(struct app_params *app)
{
	uint32_t all_links_up, i;

	all_links_up = 1;

	for (i = 0; i < app->n_links; i++) {
		struct app_link_params *p = &app->link_params[i];
		struct rte_eth_link link_params;

		memset(&link_params, 0, sizeof(link_params));
		rte_eth_link_get(p->pmd_id, &link_params);

		APP_LOG(app, HIGH, "%s (%" PRIu32 ") (%" PRIu32 " Gbps) %s",
			p->name,
			p->pmd_id,
			link_params.link_speed / 1000,
			link_params.link_status ? "UP" : "DOWN");

		if (link_params.link_status == ETH_LINK_DOWN)
			all_links_up = 0;
	}

	if (all_links_up == 0)
		rte_panic("Some links are DOWN\n");
}

static uint32_t
is_any_swq_frag_or_ras(struct app_params *app)
{
	uint32_t i;

	for (i = 0; i < app->n_pktq_swq; i++) {
		struct app_pktq_swq_params *p = &app->swq_params[i];

		if ((p->ipv4_frag == 1) || (p->ipv6_frag == 1) ||
			(p->ipv4_ras == 1) || (p->ipv6_ras == 1))
			return 1;
	}

	return 0;
}

static void
app_init_link_frag_ras(struct app_params *app)
{
	uint32_t i;

	if (is_any_swq_frag_or_ras(app)) {
		for (i = 0; i < app->n_pktq_hwq_out; i++) {
			struct app_pktq_hwq_out_params *p_txq =
				&app->hwq_out_params[i];

			p_txq->conf.txq_flags &= ~ETH_TXQ_FLAGS_NOMULTSEGS;
		}
	}
}

static inline int
app_get_cpu_socket_id(uint32_t pmd_id)
{
	int status = rte_eth_dev_socket_id(pmd_id);

	return (status != SOCKET_ID_ANY) ? status : 0;
}

struct rte_eth_rxmode rx_mode = {
	.max_rx_pkt_len = ETHER_MAX_LEN, /**< Default maximum frame length. */
	.split_hdr_size = 0,
	.header_split   = 0, /**< Header Split disabled. */
	.hw_ip_checksum = 0, /**< IP checksum offload disabled. */
	.hw_vlan_filter = 1, /**< VLAN filtering enabled. */
	.hw_vlan_strip  = 1, /**< VLAN strip enabled. */
	.hw_vlan_extend = 0, /**< Extended VLAN disabled. */
	.jumbo_frame    = 0, /**< Jumbo Frame Support disabled. */
	.hw_strip_crc   = 0, /**< CRC stripping by hardware disabled. */
};
struct rte_fdir_conf fdir_conf = {
	.mode = RTE_FDIR_MODE_NONE,
	.pballoc = RTE_FDIR_PBALLOC_64K,
	.status = RTE_FDIR_REPORT_STATUS,
	.mask = {
		.vlan_tci_mask = 0x0,
		.ipv4_mask     = {
			.src_ip = 0xFFFFFFFF,
			.dst_ip = 0xFFFFFFFF,
		},
		.ipv6_mask     = {
		.src_ip = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
		.dst_ip = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF},
		},
		.src_port_mask = 0xFFFF,
		.dst_port_mask = 0xFFFF,
		.mac_addr_byte_mask = 0xFF,
		.tunnel_type_mask = 1,
		.tunnel_id_mask = 0xFFFFFFFF,
	},
	.drop_queue = 127,
};

	static void
app_init_link(struct app_params *app)
{
	uint32_t i, size;

	app_init_link_frag_ras(app);

	/* Configuring port_config_t structure for interface
	 * manager initialization
	 */
	size = RTE_CACHE_LINE_ROUNDUP(sizeof(port_config_t));
	port_config = rte_zmalloc(NULL, (app->n_links * size),
			RTE_CACHE_LINE_SIZE);
	if (port_config == NULL)
		rte_panic("port_config is NULL: Memory Allocation failure\n");

	for (i = 0; i < app->n_links; i++) {
		struct app_link_params *p_link = &app->link_params[i];
		uint32_t link_id, n_hwq_in, n_hwq_out;
		int status;

		status = sscanf(p_link->name, "LINK%" PRIu32, &link_id);
		if (status < 0)
			rte_panic("%s (%" PRId32 "): "
					"init error (%" PRId32 ")\n",
					p_link->name, link_id, status);

		n_hwq_in = app_link_get_n_rxq(app, p_link);
		n_hwq_out = app_link_get_n_txq(app, p_link);

		printf("\n\nn_hwq_in %d\n", n_hwq_in);
		struct rte_eth_conf *My_local_conf = &p_link->conf;
		if (enable_hwlb) {
			My_local_conf->rxmode = rx_mode;
			My_local_conf->fdir_conf = fdir_conf;
			My_local_conf->rxmode.mq_mode = ETH_MQ_RX_RSS;
			My_local_conf->rx_adv_conf.rss_conf.rss_key = NULL;
			My_local_conf->rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP
				| ETH_RSS_UDP | ETH_RSS_TCP;
			/* pkt-filter-mode is perfect */
			My_local_conf->fdir_conf.mode = RTE_FDIR_MODE_PERFECT;
		} else {
			/* disable-rss */
			My_local_conf->rx_adv_conf.rss_conf.rss_hf = 0;
		}

		/* Set the hardware CRC stripping to avoid double stripping
		 * of FCS in VM */
		p_link->conf.rxmode.hw_strip_crc = 1;

		APP_LOG(app, HIGH, "Initializing %s (%" PRIu32") "
				"(%" PRIu32 " RXQ, %" PRIu32 " TXQ) ...",
				p_link->name,
				p_link->pmd_id,
				n_hwq_in,
				n_hwq_out);

		port_config[i].port_id = p_link->pmd_id;
		port_config[i].nrx_queue = n_hwq_in;
		port_config[i].ntx_queue = n_hwq_out;
		port_config[i].state = 1;
		port_config[i].promisc = p_link->promisc;
		port_config[i].mempool.pool_size =
			app->mempool_params[0].pool_size;
		port_config[i].mempool.buffer_size =
			app->mempool_params[0].buffer_size;
		port_config[i].mempool.cache_size =
			app->mempool_params[0].cache_size;
		port_config[i].mempool.cpu_socket_id =
			app->mempool_params[0].cpu_socket_id;
		memcpy(&port_config[i].port_conf, &p_link->conf,
				sizeof(struct rte_eth_conf));
		memcpy(&port_config[i].rx_conf, &app->hwq_in_params[0].conf,
				sizeof(struct rte_eth_rxconf));
		memcpy(&port_config[i].tx_conf, &app->hwq_out_params[0].conf,
				sizeof(struct rte_eth_txconf));

		if (app->header_csum_req) {
			/* Enable TCP and UDP HW Checksum */
			port_config[i].tx_conf.txq_flags &=
				~(ETH_TXQ_FLAGS_NOXSUMTCP |
						ETH_TXQ_FLAGS_NOXSUMUDP);
		}

		if (ifm_port_setup(p_link->pmd_id, &port_config[i])) {
                        printf("Failed to configure port %s - %"PRIu32
                               ".\n", p_link->name, p_link->pmd_id);
                        printf("Try again with offload disabled....\n");
                        port_config[i].tx_conf.txq_flags |= ETH_TXQ_FLAGS_NOOFFLOADS;
                        if (ifm_port_setup (p_link->pmd_id, &port_config[i]))
			    rte_panic("Port Setup Failed: %s - %" PRIu32
					"\n", p_link->name, p_link->pmd_id);
                }

		app_link_up_internal(app, p_link);
	}

	app_check_link(app);
}

static void
app_init_swq(struct app_params *app)
{
	uint32_t i;

	for (i = 0; i < app->n_pktq_swq; i++) {
		struct app_pktq_swq_params *p = &app->swq_params[i];
		unsigned int flags = 0;

		if (app_swq_get_readers(app, p) == 1)
			flags |= RING_F_SC_DEQ;
		if (app_swq_get_writers(app, p) == 1)
			flags |= RING_F_SP_ENQ;

		APP_LOG(app, HIGH, "Initializing %s...", p->name);
		app->swq[i] = rte_ring_create(
				p->name,
				p->size,
				p->cpu_socket_id,
				flags);

		if (app->swq[i] == NULL)
			rte_panic("%s init error\n", p->name);
	}
}

static void
app_init_tm(struct app_params *app)
{
	uint32_t i;

	for (i = 0; i < app->n_pktq_tm; i++) {
		struct app_pktq_tm_params *p_tm = &app->tm_params[i];
		struct app_link_params *p_link;
		struct rte_eth_link link_eth_params;
		struct rte_sched_port *sched;
		uint32_t n_subports, subport_id;
		int status;

		p_link = app_get_link_for_tm(app, p_tm);
		/* LINK */
		rte_eth_link_get(p_link->pmd_id, &link_eth_params);

		/* TM */
		p_tm->sched_port_params.name = p_tm->name;
		p_tm->sched_port_params.socket =
			app_get_cpu_socket_id(p_link->pmd_id);
		p_tm->sched_port_params.rate =
			(uint64_t) link_eth_params.link_speed * 1000 * 1000 / 8;

		APP_LOG(app, HIGH, "Initializing %s ...", p_tm->name);
		sched = rte_sched_port_config(&p_tm->sched_port_params);
		if (sched == NULL)
			rte_panic("%s init error\n", p_tm->name);
		app->tm[i] = sched;

		/* Subport */
		n_subports = p_tm->sched_port_params.n_subports_per_port;
		for (subport_id = 0; subport_id < n_subports; subport_id++) {
			uint32_t n_pipes_per_subport, pipe_id;

			status = rte_sched_subport_config(sched,
				subport_id,
				&p_tm->sched_subport_params[subport_id]);
			if (status)
				rte_panic("%s subport %" PRIu32
					" init error (%" PRId32 ")\n",
					p_tm->name, subport_id, status);

			/* Pipe */
			n_pipes_per_subport =
				p_tm->sched_port_params.n_pipes_per_subport;
			for (pipe_id = 0;
				pipe_id < n_pipes_per_subport;
				pipe_id++) {
				int profile_id = p_tm->sched_pipe_to_profile[
					subport_id * APP_MAX_SCHED_PIPES +
					pipe_id];

				if (profile_id == -1)
					continue;

				status = rte_sched_pipe_config(sched,
					subport_id,
					pipe_id,
					profile_id);
				if (status)
					rte_panic("%s subport %" PRIu32
						" pipe %" PRIu32
						" (profile %" PRId32 ") "
						"init error (% " PRId32 ")\n",
						p_tm->name, subport_id, pipe_id,
						profile_id, status);
			}
		}
	}
}

static void
app_init_msgq(struct app_params *app)
{
	uint32_t i;

	for (i = 0; i < app->n_msgq; i++) {
		struct app_msgq_params *p = &app->msgq_params[i];

		APP_LOG(app, HIGH, "Initializing %s ...", p->name);
		app->msgq[i] = rte_ring_create(
				p->name,
				p->size,
				p->cpu_socket_id,
				RING_F_SP_ENQ | RING_F_SC_DEQ);

		if (app->msgq[i] == NULL)
			rte_panic("%s init error\n", p->name);
	}
}

static void app_pipeline_params_get(struct app_params *app,
	struct app_pipeline_params *p_in,
	struct pipeline_params *p_out)
{
	uint32_t i;
	uint32_t mempool_id;

	snprintf(p_out->name, PIPELINE_NAME_SIZE, "%s", p_in->name);

	p_out->socket_id = (int) p_in->socket_id;

	p_out->log_level = app->log_level;

	/* pktq_in */
	p_out->n_ports_in = p_in->n_pktq_in;
	for (i = 0; i < p_in->n_pktq_in; i++) {
		struct app_pktq_in_params *in = &p_in->pktq_in[i];
		struct pipeline_port_in_params *out = &p_out->port_in[i];

		switch (in->type) {
		case APP_PKTQ_IN_HWQ:
		{
			struct app_pktq_hwq_in_params *p_hwq_in =
				&app->hwq_in_params[in->id];
			struct app_link_params *p_link =
				app_get_link_for_rxq(app, p_hwq_in);
			uint32_t rxq_link_id, rxq_queue_id;

			int status =
			sscanf(p_hwq_in->name, "RXQ%" SCNu32 ".%" SCNu32,
				&rxq_link_id,
				&rxq_queue_id);
			if (status < 0)
				rte_panic("%s (%" PRId32 "): "
				"init error (%" PRId32 ")\n",
				p_hwq_in->name, rxq_link_id, status);

			out->type = PIPELINE_PORT_IN_ETHDEV_READER;
			out->params.ethdev.port_id = p_link->pmd_id;
			out->params.ethdev.queue_id = rxq_queue_id;
			out->burst_size = p_hwq_in->burst;
			break;
		}
		case APP_PKTQ_IN_SWQ:
		{
			struct app_pktq_swq_params *swq_params =
				&app->swq_params[in->id];

			if ((swq_params->ipv4_frag == 0) &&
				(swq_params->ipv6_frag == 0)) {
				if (app_swq_get_readers(app,
					swq_params) == 1) {
					out->type =
						PIPELINE_PORT_IN_RING_READER;
					out->params.ring.ring =
						app->swq[in->id];
					out->burst_size =
						app->swq_params[in->id].
							burst_read;
				} else {
				out->type = PIPELINE_PORT_IN_RING_MULTI_READER;
				out->params.ring_multi.ring = app->swq[in->id];
				out->burst_size = swq_params->burst_read;
				}
			} else {
				if (swq_params->ipv4_frag == 1) {
				struct rte_port_ring_reader_ipv4_frag_params
					*params =
						&out->params.ring_ipv4_frag;

				out->type =
					PIPELINE_PORT_IN_RING_READER_IPV4_FRAG;
				params->ring = app->swq[in->id];
				params->mtu = swq_params->mtu;
				params->metadata_size =
					swq_params->metadata_size;
				params->pool_direct =
					app->mempool
					[swq_params->mempool_direct_id];
				params->pool_indirect =
					app->mempool
					[swq_params->mempool_indirect_id];
				out->burst_size = swq_params->burst_read;
				} else {
				struct rte_port_ring_reader_ipv6_frag_params
					*params =
						&out->params.ring_ipv6_frag;

				out->type =
					PIPELINE_PORT_IN_RING_READER_IPV6_FRAG;
				params->ring = app->swq[in->id];
				params->mtu = swq_params->mtu;
				params->metadata_size =
					swq_params->metadata_size;
				params->pool_direct =
					app->mempool
					[swq_params->mempool_direct_id];
				params->pool_indirect =
					app->mempool
					[swq_params->mempool_indirect_id];
				out->burst_size = swq_params->burst_read;
				}
			}
			break;
		}
		case APP_PKTQ_IN_TM:
			out->type = PIPELINE_PORT_IN_SCHED_READER;
			out->params.sched.sched = app->tm[in->id];
			out->burst_size = app->tm_params[in->id].burst_read;
			break;
		case APP_PKTQ_IN_SOURCE:
			mempool_id = app->source_params[in->id].mempool_id;
			out->type = PIPELINE_PORT_IN_SOURCE;
			out->params.source.mempool = app->mempool[mempool_id];
			out->burst_size = app->source_params[in->id].burst;

#ifdef RTE_NEXT_ABI
			if (app->source_params[in->id].file_name
				!= NULL) {
				out->params.source.file_name = strdup(
					app->source_params[in->id].
					file_name);
				if (out->params.source.file_name == NULL) {
					out->params.source.
						n_bytes_per_pkt = 0;
					break;
				}
				out->params.source.n_bytes_per_pkt =
					app->source_params[in->id].
					n_bytes_per_pkt;
			}
#endif

			break;
		default:
			break;
		}
	}

	/* pktq_out */
	p_out->n_ports_out = p_in->n_pktq_out;
	for (i = 0; i < p_in->n_pktq_out; i++) {
		struct app_pktq_out_params *in = &p_in->pktq_out[i];
		struct pipeline_port_out_params *out = &p_out->port_out[i];

		switch (in->type) {
		case APP_PKTQ_OUT_HWQ:
		{
			struct app_pktq_hwq_out_params *p_hwq_out =
				&app->hwq_out_params[in->id];
			struct app_link_params *p_link =
				app_get_link_for_txq(app, p_hwq_out);
			uint32_t txq_link_id, txq_queue_id;

			int status =
			sscanf(p_hwq_out->name,
				"TXQ%" SCNu32 ".%" SCNu32,
				&txq_link_id,
				&txq_queue_id);
			if (status < 0)
				rte_panic("%s (%" PRId32 "): "
				"init error (%" PRId32 ")\n",
				p_hwq_out->name, txq_link_id, status);

			if (p_hwq_out->dropless == 0) {
				struct rte_port_ethdev_writer_params *params =
					&out->params.ethdev;

				out->type = PIPELINE_PORT_OUT_ETHDEV_WRITER;
				params->port_id = p_link->pmd_id;
				params->queue_id = txq_queue_id;
				params->tx_burst_sz =
					app->hwq_out_params[in->id].burst;
			} else {
				struct rte_port_ethdev_writer_nodrop_params
					*params = &out->params.ethdev_nodrop;

				out->type =
					PIPELINE_PORT_OUT_ETHDEV_WRITER_NODROP;
				params->port_id = p_link->pmd_id;
				params->queue_id = txq_queue_id;
				params->tx_burst_sz = p_hwq_out->burst;
				params->n_retries = p_hwq_out->n_retries;
			}
			break;
		}
		case APP_PKTQ_OUT_SWQ:
		{
		struct app_pktq_swq_params *swq_params =
			&app->swq_params[in->id];

		if ((swq_params->ipv4_ras == 0) &&
			(swq_params->ipv6_ras == 0)) {
			if (app_swq_get_writers(app, swq_params) == 1) {
				if (app->swq_params[in->id].dropless == 0) {
					struct rte_port_ring_writer_params
						*params = &out->params.ring;

				out->type = PIPELINE_PORT_OUT_RING_WRITER;
				params->ring = app->swq[in->id];
				params->tx_burst_sz =
					app->swq_params[in->id].burst_write;
				} else {
				struct rte_port_ring_writer_nodrop_params
					*params = &out->params.ring_nodrop;

				out->type =
					PIPELINE_PORT_OUT_RING_WRITER_NODROP;
				params->ring = app->swq[in->id];
				params->tx_burst_sz =
					app->swq_params[in->id].burst_write;
				params->n_retries =
				app->swq_params[in->id].n_retries;
				}
			} else {
				if (swq_params->dropless == 0) {
					struct rte_port_ring_multi_writer_params
						*params =
						&out->params.ring_multi;

				out->type =
					PIPELINE_PORT_OUT_RING_MULTI_WRITER;
				params->ring = app->swq[in->id];
				params->tx_burst_sz = swq_params->burst_write;
				} else {
				struct rte_port_ring_multi_writer_nodrop_params
					*params =
						&out->params.ring_multi_nodrop;

				out->type =
				PIPELINE_PORT_OUT_RING_MULTI_WRITER_NODROP;

				params->ring = app->swq[in->id];
				params->tx_burst_sz = swq_params->burst_write;
				params->n_retries = swq_params->n_retries;
				}
				}
			} else {
			if (swq_params->ipv4_ras == 1) {
				struct rte_port_ring_writer_ipv4_ras_params
					*params =
						&out->params.ring_ipv4_ras;

				out->type =
					PIPELINE_PORT_OUT_RING_WRITER_IPV4_RAS;
				params->ring = app->swq[in->id];
				params->tx_burst_sz = swq_params->burst_write;
			} else {
				struct rte_port_ring_writer_ipv6_ras_params
					*params =
						&out->params.ring_ipv6_ras;

				out->type =
					PIPELINE_PORT_OUT_RING_WRITER_IPV6_RAS;
				params->ring = app->swq[in->id];
				params->tx_burst_sz = swq_params->burst_write;
			}
			}
			break;
		}
		case APP_PKTQ_OUT_TM: {
			struct rte_port_sched_writer_params *params =
				&out->params.sched;

			out->type = PIPELINE_PORT_OUT_SCHED_WRITER;
			params->sched = app->tm[in->id];
			params->tx_burst_sz =
				app->tm_params[in->id].burst_write;
			break;
		}
		case APP_PKTQ_OUT_SINK:
			out->type = PIPELINE_PORT_OUT_SINK;
			if (app->sink_params[in->id].file_name != NULL) {
				out->params.sink.file_name = strdup(
					app->sink_params[in->id].
					file_name);
				if (out->params.sink.file_name == NULL) {
					out->params.sink.max_n_pkts = 0;
					break;
				}
				out->params.sink.max_n_pkts =
					app->sink_params[in->id].
					n_pkts_to_dump;
			} else {
				out->params.sink.file_name = NULL;
				out->params.sink.max_n_pkts = 0;
			}
			break;
		default:
			break;
		}
	}

	/* msgq */
	p_out->n_msgq = p_in->n_msgq_in;

	for (i = 0; i < p_in->n_msgq_in; i++)
		p_out->msgq_in[i] = app->msgq[p_in->msgq_in[i]];

	for (i = 0; i < p_in->n_msgq_out; i++)
		p_out->msgq_out[i] = app->msgq[p_in->msgq_out[i]];

	/* args */
	p_out->n_args = p_in->n_args;
	for (i = 0; i < p_in->n_args; i++) {
		p_out->args_name[i] = p_in->args_name[i];
		p_out->args_value[i] = p_in->args_value[i];
	}
}

static void
app_init_pipelines(struct app_params *app)
{
	uint32_t p_id;

	for (p_id = 0; p_id < app->n_pipelines; p_id++) {
		struct app_pipeline_params *params =
			&app->pipeline_params[p_id];
		struct app_pipeline_data *data = &app->pipeline_data[p_id];
		struct pipeline_type *ptype;
		struct pipeline_params pp;

		APP_LOG(app, HIGH, "Initializing %s ...", params->name);

		ptype = app_pipeline_type_find(app, params->type);
		if (ptype == NULL)
			rte_panic("Init error: Unknown pipeline type \"%s\"\n",
				params->type);

		app_pipeline_params_get(app, params, &pp);

		/* Back-end */
		data->be = NULL;
		if (ptype->be_ops->f_init) {
			data->be = ptype->be_ops->f_init(&pp, (void *) app);

			if (data->be == NULL)
				rte_panic("Pipeline instance \"%s\" back-end "
					"init error\n", params->name);
		}

		/* Front-end */
		data->fe = NULL;
		if (ptype->fe_ops->f_init) {
			data->fe = ptype->fe_ops->f_init(&pp, (void *) app);

			if (data->fe == NULL)
				rte_panic("Pipeline instance \"%s\" front-end "
				"init error\n", params->name);
		}

		data->ptype = ptype;

		data->timer_period = (rte_get_tsc_hz() *
			params->timer_period) / 100;
	}
}

static void
app_init_threads(struct app_params *app)
{
	uint64_t time = rte_get_tsc_cycles();
	uint32_t p_id;

	for (p_id = 0; p_id < app->n_pipelines; p_id++) {
		struct app_pipeline_params *params =
			&app->pipeline_params[p_id];
		struct app_pipeline_data *data = &app->pipeline_data[p_id];
		struct pipeline_type *ptype;
		struct app_thread_data *t;
		struct app_thread_pipeline_data *p;
		int lcore_id;

		lcore_id = cpu_core_map_get_lcore_id(app->core_map,
			params->socket_id,
			params->core_id,
			params->hyper_th_id);

		if (lcore_id < 0)
			rte_panic("Invalid core s%" PRIu32 "c%" PRIu32 "%s\n",
				params->socket_id,
				params->core_id,
				(params->hyper_th_id) ? "h" : "");

		t = &app->thread_data[lcore_id];

		t->timer_period = (rte_get_tsc_hz() *
			APP_THREAD_TIMER_PERIOD) / DIV_CONV_HZ_SEC;
		t->thread_req_deadline = time + t->timer_period;

		t->headroom_cycles = 0;
		t->headroom_time = rte_get_tsc_cycles();
		t->headroom_ratio = 0.0;

		t->msgq_in = app_thread_msgq_in_get(app,
				params->socket_id,
				params->core_id,
				params->hyper_th_id);
		if (t->msgq_in == NULL)
			rte_panic("Init error: Cannot find MSGQ_IN "
				"for thread %" PRId32, lcore_id);

		t->msgq_out = app_thread_msgq_out_get(app,
				params->socket_id,
				params->core_id,
				params->hyper_th_id);
		if (t->msgq_out == NULL)
			rte_panic("Init error: Cannot find MSGQ_OUT "
				"for thread %" PRId32, lcore_id);

		ptype = app_pipeline_type_find(app, params->type);
		if (ptype == NULL)
			rte_panic("Init error: Unknown pipeline "
				"type \"%s\"\n", params->type);

		p = (ptype->be_ops->f_run == NULL) ?
			&t->regular[t->n_regular] :
			&t->custom[t->n_custom];

		p->pipeline_id = p_id;
		p->be = data->be;
		p->f_run = ptype->be_ops->f_run;
		p->f_timer = ptype->be_ops->f_timer;
		p->timer_period = data->timer_period;
		p->deadline = time + data->timer_period;

		data->enabled = 1;

		if (ptype->be_ops->f_run == NULL)
			t->n_regular++;
		else
			t->n_custom++;
	}
}

int app_init(struct app_params *app)
{
	app_init_core_map(app);
	app_init_core_mask(app);

	app_init_eal(app);
	ifm_init();
	/*app_init_mempool(app);*/
	app_init_link(app);
	app_init_swq(app);
	app_init_tm(app);
	app_init_msgq(app);

	app_pipeline_common_cmd_push(app);
	app_pipeline_thread_cmd_push(app);
	app_pipeline_type_register(app, &pipeline_master);
	app_pipeline_type_register(app, &pipeline_passthrough);
	app_pipeline_type_register(app, &pipeline_vfw);
	app_pipeline_type_register(app, &pipeline_loadb);
	app_pipeline_type_register(app, &pipeline_txrx);
	app_pipeline_type_register(app, &pipeline_arpicmp);

	app_init_pipelines(app);
	app_init_threads(app);

	#ifdef L3_STACK_SUPPORT
	l3fwd_init();
	create_arp_table();
	create_nd_table();
	populate_lpm_routes();
	print_interface_details();
	#endif
	return 0;
}

static int
app_pipeline_type_cmd_push(struct app_params *app,
	struct pipeline_type *ptype)
{
	cmdline_parse_ctx_t *cmds;
	uint32_t n_cmds, i;

	/* Check input arguments */
	if ((app == NULL) ||
		(ptype == NULL))
		return -EINVAL;

	n_cmds = pipeline_type_cmds_count(ptype);
	if (n_cmds == 0)
		return 0;

	cmds = ptype->fe_ops->cmds;

	/* Check for available slots in the application commands array */
	if (n_cmds > APP_MAX_CMDS - app->n_cmds)
		return -ENOMEM;

	/* Push pipeline commands into the application */
	memcpy(&app->cmds[app->n_cmds],
		cmds,
		n_cmds * sizeof(cmdline_parse_ctx_t));

	for (i = 0; i < n_cmds; i++)
		app->cmds[app->n_cmds + i]->data = app;

	app->n_cmds += n_cmds;
	app->cmds[app->n_cmds] = NULL;

	return 0;
}

int
app_pipeline_type_register(struct app_params *app, struct pipeline_type *ptype)
{
	uint32_t n_cmds, i;

	/* Check input arguments */
	if ((app == NULL) ||
		(ptype == NULL) ||
		(ptype->name == NULL) ||
		(strlen(ptype->name) == 0) ||
		(ptype->be_ops->f_init == NULL) ||
		(ptype->be_ops->f_timer == NULL))
		return -EINVAL;

	/* Check for duplicate entry */
	for (i = 0; i < app->n_pipeline_types; i++)
		if (strcmp(app->pipeline_type[i].name, ptype->name) == 0)
			return -EEXIST;

	/* Check for resource availability */
	n_cmds = pipeline_type_cmds_count(ptype);
	if ((app->n_pipeline_types == APP_MAX_PIPELINE_TYPES) ||
		(n_cmds > APP_MAX_CMDS - app->n_cmds))
		return -ENOMEM;

	/* Copy pipeline type */
	memcpy(&app->pipeline_type[app->n_pipeline_types++],
		ptype,
		sizeof(struct pipeline_type));

	/* Copy CLI commands */
	if (n_cmds)
		app_pipeline_type_cmd_push(app, ptype);

	return 0;
}

struct
pipeline_type *app_pipeline_type_find(struct app_params *app, char *name)
{
	uint32_t i;

	for (i = 0; i < app->n_pipeline_types; i++)
		if (strcmp(app->pipeline_type[i].name, name) == 0)
			return &app->pipeline_type[i];

	return NULL;
}
