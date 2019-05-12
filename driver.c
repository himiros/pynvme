/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Crane Che <cranechu@gmail.com>
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/sysinfo.h>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/crc32.h"
#include "spdk/rpc.h"
#include "spdk_internal/log.h"
#include "spdk/lib/nvme/nvme_internal.h"
#include "driver.h"


#define US_PER_S              (1000ULL*1000ULL)
#define MIN(X,Y)              ((X) < (Y) ? (X) : (Y))

#ifndef BIT
#define BIT(a)                (1UL << (a))
#endif /* BIT */

// the global configuration of the driver
#define DCFG_VERIFY_READ      (BIT(0))


//// shared data
///////////////////////////////

#define DRIVER_IO_TOKEN_NAME      "driver_io_token"
#define DRIVER_CRC32_TABLE_NAME   "driver_crc32_table"
#define DRIVER_GLOBAL_CONFIG_NAME "driver_global_config"

// TODO: support multiple namespace
static uint64_t g_driver_table_size = 0;
static uint64_t* g_driver_io_token_ptr = NULL;
static uint32_t* g_driver_csum_table_ptr = NULL;
static uint64_t* g_driver_global_config_ptr = NULL;

static int memzone_reserve_shared_memory(uint64_t table_size)
{
  if (spdk_process_is_primary())
  {
    assert(g_driver_io_token_ptr == NULL);
    assert(g_driver_csum_table_ptr == NULL);

    // get the shared memory for token
    SPDK_INFOLOG(SPDK_LOG_NVME, "create token table, size: %ld\n", table_size);
    g_driver_table_size = table_size;
    g_driver_csum_table_ptr = spdk_memzone_reserve(DRIVER_CRC32_TABLE_NAME,
                                                   table_size,
                                                   0, SPDK_MEMZONE_NO_IOVA_CONTIG);
    g_driver_io_token_ptr = spdk_memzone_reserve(DRIVER_IO_TOKEN_NAME,
                                                 sizeof(uint64_t),
                                                 0, 0);
  }
  else
  {
    // find the shared memory for token
    g_driver_table_size = table_size;
    g_driver_io_token_ptr = spdk_memzone_lookup(DRIVER_IO_TOKEN_NAME);
    g_driver_csum_table_ptr = spdk_memzone_lookup(DRIVER_CRC32_TABLE_NAME);
  }

  if (g_driver_csum_table_ptr == NULL)
  {
    SPDK_NOTICELOG("memory is not large enough to keep CRC32 table.\n");
    SPDK_NOTICELOG("Data verification is disabled!\n");
  }

  if (g_driver_io_token_ptr == NULL)
  {
    SPDK_ERRLOG("fail to find memzone space\n");
    return -1;
  }

  if (spdk_process_is_primary())
  {
    // avoid token 0
    *g_driver_io_token_ptr = 1;
  }
  
  return 0;
}

void crc32_clear(uint64_t lba, uint64_t lba_count, int sanitize, int uncorr)
{
  int c = uncorr ? 0xff : 0;
  size_t len = lba_count*sizeof(uint32_t);

  if (sanitize == true)
  {
    assert(lba == 0);
    assert(g_driver_table_size != 0); //Namspace instance not exist, you may need to add nvme0n1 in the fixture list
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "clear the whole table\n");
    len = g_driver_table_size;
  }

  if (g_driver_csum_table_ptr != NULL)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "clear checksum table, lba 0x%lx, c %d, len %ld\n",
                  lba, c, len);
    memset(&g_driver_csum_table_ptr[lba], c, len);
  }
}

static void crc32_fini(void)
{
  if (spdk_process_is_primary())
  {
    spdk_memzone_free(DRIVER_IO_TOKEN_NAME);
    spdk_memzone_free(DRIVER_CRC32_TABLE_NAME);
  }
  g_driver_io_token_ptr = NULL;
  g_driver_csum_table_ptr = NULL;
}


////module: buffer
///////////////////////////////

void* buffer_init(size_t bytes, uint64_t *phys_addr)
{
  void* buf = spdk_dma_zmalloc(bytes, 0x1000, phys_addr);

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "buffer: alloc ptr at %p, size %ld\n",
               buf, bytes);

  assert(buf != NULL);
  return buf;
}

static inline uint32_t buffer_calc_csum(uint64_t* ptr, int len)
{
  uint32_t crc = spdk_crc32c_update(ptr, len, 0);

  //reserve 0: nomapping
  //reserve 0xffffffff: uncorrectable
  if (crc == 0) crc = 1;
  if (crc == 0xffffffff) crc = 0xfffffffe;

  return crc;
}

static void buffer_fill_data(void* buf,
                             uint64_t lba,
                             uint32_t lba_count,
                             uint32_t lba_size)
{
  // token is keeping increasing, so every write has different data
  uint64_t token = __atomic_fetch_add(g_driver_io_token_ptr,
                                      lba_count,
                                      __ATOMIC_SEQ_CST);

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "token: %ld, lba 0x%lx, lba count %d\n", token, lba, lba_count);

  for (uint32_t i=0; i<lba_count; i++, lba++)
  {
    uint64_t* ptr = (uint64_t*)(buf+i*lba_size);

    //first and last 64bit-words are filled with special data
    ptr[0] = lba;
    ptr[lba_size/sizeof(uint64_t)-1] = token+i;

    //keep crc in memory if allocated
    // suppose device modify data correctly. If the command fail, we cannot
    // tell what part of data is updated, while what not. Even when atomic
    // write is supported, we still cannot tell that.
    if (g_driver_csum_table_ptr != NULL)
    {
      uint32_t crc = buffer_calc_csum(ptr, lba_size);
      g_driver_csum_table_ptr[lba] = crc;
    }
  }
}

static int buffer_verify_data(const void* buf,
                              const unsigned long lba_first,
                              const uint32_t lba_count,
                              const uint32_t lba_size)
{
  unsigned long lba = lba_first;

  for (uint32_t i=0; i<lba_count; i++, lba++)
  {
    unsigned long* ptr = (unsigned long*)(buf+i*lba_size);
    uint32_t computed_crc = buffer_calc_csum(ptr, lba_size);
    uint32_t expected_crc = computed_crc;

    // if crc table is not available, just use computed crc as
    //expected crc, to bypass verification
    if (g_driver_csum_table_ptr != NULL)
    {
      expected_crc = g_driver_csum_table_ptr[lba];
    }

    if (expected_crc == 0)
    {
      //no mapping, nothing to verify
      continue;
    }

    if (expected_crc == 0xffffffff)
    {
      SPDK_WARNLOG("lba uncorrectable: lba 0x%lx\n", lba);
      return -1;
    }

    if (lba != ptr[0])
    {
      SPDK_WARNLOG("lba mismatch: lba 0x%lx, but got: 0x%lx\n", lba, ptr[0]);
      return -2;
    }

    if (computed_crc != expected_crc)
    {
      SPDK_WARNLOG("crc mismatch: lba 0x%lx, expected crc 0x%x, but got: 0x%x\n",
                   lba, expected_crc, computed_crc);
      return -3;
    }
  }

  return 0;
}

void buffer_fini(void* buf)
{
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "buffer: free ptr at %p\n", buf);
  assert(buf != NULL);
  spdk_dma_free(buf);
}


////cmd log
///////////////////////////////

// log_table contains latest cmd and cpl and their timestamps
// queue_table traces cmd log tables by queue pairs
// CMD_LOG_DEPTH should be larger than Q depth to keep all outstanding commands.
#define CMD_LOG_DEPTH (2048-1)  // reserved one slot space for tail value
#define CMD_LOG_QPAIR_COUNT (16)

struct cmd_log_entry_t {
  // cmd and cpl
  struct spdk_nvme_cmd cmd;
  struct timeval time_cmd;
  struct spdk_nvme_cpl cpl;
  uint32_t cpl_latency_us;
  uint32_t dummy;
  
  // for data verification after read
  void* buf;

  // callback to user cb functions
  struct nvme_request* req;
  void* cb_arg;
};
static_assert(sizeof(struct cmd_log_entry_t) == 128, "cacheline aligned");

struct cmd_log_table_t {
  struct cmd_log_entry_t table[CMD_LOG_DEPTH];
  uint32_t tail_index;
  uint32_t msix_data;
  uint32_t msix_enabled;
  uint32_t mask_offset;
  struct spdk_nvme_qpair* qpair;
  uint32_t dummy[26];
};
static_assert(sizeof(struct cmd_log_table_t) == sizeof(struct cmd_log_entry_t)*(CMD_LOG_DEPTH+1), "cacheline aligned");

#define DRIVER_CMDLOG_TABLE_NAME  "driver_cmdlog_table"
static struct cmd_log_table_t* cmd_log_queue_table;


static uint32_t timeval_to_us(struct timeval* t)
{
  return t->tv_sec*US_PER_S + t->tv_usec;
}


static void cmd_log_qpair_init(struct spdk_nvme_qpair* q)
{
  uint16_t qid = 0;

  if (q)
  {
    qid = q->id;
  }
  assert(qid < CMD_LOG_QPAIR_COUNT);

  // set tail to invalid value, means the qpair is empty
  cmd_log_queue_table[qid].tail_index = 0;
  cmd_log_queue_table[qid].qpair = q;
}


static void cmd_log_qpair_clear(uint16_t qid)
{
  assert(qid < CMD_LOG_QPAIR_COUNT);

  // set tail to invalid value, means the qpair is empty
  cmd_log_queue_table[qid].tail_index = CMD_LOG_DEPTH;
}


static int cmd_log_init(void)
{
  if (spdk_process_is_primary())
  {
    cmd_log_queue_table = spdk_memzone_reserve(DRIVER_CMDLOG_TABLE_NAME,
                                               sizeof(struct cmd_log_table_t)*CMD_LOG_QPAIR_COUNT,
                                               0, SPDK_MEMZONE_NO_IOVA_CONTIG);

    // clear all qpair's cmd log
    for (int i=0; i<CMD_LOG_QPAIR_COUNT; i++)
    {
      cmd_log_qpair_clear(i);
    }

    // also init config word with cmdlog
    g_driver_global_config_ptr = spdk_memzone_reserve(DRIVER_GLOBAL_CONFIG_NAME,
                                                      sizeof(uint64_t),
                                                      0, 0);
    *g_driver_global_config_ptr = 0;
  }
  else
  {
    cmd_log_queue_table = spdk_memzone_lookup(DRIVER_CMDLOG_TABLE_NAME);
    g_driver_global_config_ptr = spdk_memzone_lookup(DRIVER_GLOBAL_CONFIG_NAME);
  }

  if (cmd_log_queue_table == NULL)
  {
    fprintf(stderr, "Cannot allocate or find the cmdlog memory!\n");
    return -1;
  }

  return 0;
}


static void cmd_log_finish(void)
{
  spdk_memzone_free(DRIVER_CMDLOG_TABLE_NAME);
  spdk_memzone_free(DRIVER_GLOBAL_CONFIG_NAME);
}

static struct cmd_log_entry_t* debug_log_entry = NULL;

void cmdlog_cmd_cpl(struct nvme_request* req, struct spdk_nvme_cpl* cpl)
{
#if 0
  struct timeval diff;
  struct timeval now;
  struct cmd_log_entry_t* log_entry = req->cmdlog_entry;

  assert(cpl != NULL);
  assert(log_entry != NULL);

  if (debug_log_entry)
  {
    SPDK_INFOLOG(SPDK_LOG_NVME, "overlapped entry %p, cpl entry %p, cpl req %p, this req %p \n",
                 debug_log_entry, log_entry, log_entry->req, req);
  }

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "cmd completed, cid %d\n", log_entry->cpl.cid);

  //check if the log entry is still for this completed cmd
  if (log_entry->req == NULL || log_entry->req != req)
  {
    //it's an overlapped entry, just skip cmdlog callback
    SPDK_NOTICELOG("skip overlapped cmdlog entry %p\n", log_entry);
    return;
  }

  //reuse dword2 of cpl as latency value
  gettimeofday(&now, NULL);
  memcpy(&log_entry->cpl, cpl, sizeof(struct spdk_nvme_cpl));
  timersub(&now, &log_entry->time_cmd, &diff);
  log_entry->cpl_latency_us = timeval_to_us(&diff);

  //verify read data
  if (log_entry->cmd.opc == 2 && log_entry->buf != NULL)
  {
    if ((*g_driver_global_config_ptr & DCFG_VERIFY_READ) != 0)
    {
      struct spdk_nvme_cmd* cmd = &log_entry->cmd;
      uint64_t lba = cmd->cdw10 + ((uint64_t)(cmd->cdw11)<<32);
      uint16_t lba_count = (cmd->cdw12 & 0xffff);

      // get ns and lba size of the data
      struct spdk_nvme_ctrlr* ctrlr = log_entry->req->qpair->ctrlr;
      struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, cmd->nsid);
      uint32_t lba_size = spdk_nvme_ns_get_sector_size(ns);

      //verify data pattern and crc
      if (0 != buffer_verify_data(log_entry->buf,
                                  lba,
                                  lba_count,
                                  lba_size))
      {
        assert(log_entry->req);

        //Unrecovered Read Error: The read data could not be recovered from the media.
        cpl->status.sct = 0x02;
        cpl->status.sc = 0x81;
      }
    }
  }

  //recover callback argument
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "recover req %p cb arg, entry %p, old %p, new %p\n",
                log_entry->req, log_entry, log_entry->req->cb_arg, log_entry->cb_arg);
  log_entry->req = NULL;
#endif
}


// for spdk internel ues: nvme_qpair_submit_request
void cmdlog_add_cmd(struct spdk_nvme_qpair* qpair, struct nvme_request* req)
{
#if 0
  uint16_t qid = qpair->id;
  struct cmd_log_table_t* log_table = &cmd_log_queue_table[qid];
  uint32_t tail_index = log_table->tail_index;
  struct cmd_log_entry_t* log_entry = &log_table->table[tail_index];

  assert(req != NULL);
  assert(qid < CMD_LOG_QPAIR_COUNT);
  assert(log_table != NULL);
  assert(tail_index < CMD_LOG_DEPTH);

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "cmdlog: add cmd %s\n", cmd_name(req->cmd.opc, qid==0?0:1));

  if (log_entry->req != NULL)
  {
    // this entry is overlapped before cmd complete
    SPDK_NOTICELOG("uncompleted cmd in cmdlog: %p, old req %p\n", log_entry, log_entry->req);
    nvme_qpair_print_command(qpair, &log_entry->cmd);
    log_cmd_dump(qpair, 0);
    debug_log_entry = log_entry;
  }
  
  log_entry->buf = req->payload.contig_or_cb_arg;
  log_entry->cpl_latency_us = 0;
  memcpy(&log_entry->cmd, &req->cmd, sizeof(struct spdk_nvme_cmd));
  gettimeofday(&log_entry->time_cmd, NULL);

  // link req and cmdlog entry
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "save req %p cb arg to entry %p, new %p, old %p\n",
                req, log_entry, req->cb_arg, log_entry->cb_arg);
  log_entry->req = req;
  req->cmdlog_entry = log_entry;

  // add tail to commit the new cmd only when it is sent successfully
  tail_index += 1;
  if (tail_index == CMD_LOG_DEPTH)
  {
    tail_index = 0;
  }
  log_table->tail_index = tail_index;
#endif
}


//// software MSIx INTC
///////////////////////////////

static uint8_t intc_find_msix(struct spdk_pci_device* pci)
{
  uint8_t cid = 0;
  uint8_t next_offset = 0;

  spdk_pci_device_cfg_read8(pci, &next_offset, 0x34);
  while (next_offset != 0)
  {
    spdk_pci_device_cfg_read8(pci, &cid, next_offset);
    if (cid == 0x11)
    {
      // find the msix capability
      break;
    }

    spdk_pci_device_cfg_read8(pci, &next_offset, next_offset+1);
  }

  assert(next_offset != 0);
  return next_offset;
}


static void intc_init(struct spdk_nvme_ctrlr* ctrlr)
{
  uint8_t msix_base;
  uint16_t control;
  uint32_t table_offset;
  struct spdk_pci_device* pci = spdk_nvme_ctrlr_get_pci_device(ctrlr);

  // find msix capability
  msix_base = intc_find_msix(pci);
  spdk_pci_device_cfg_read16(pci, &control, msix_base+2);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "msix control: 0x%x\n", control);

  // the controller has enough verctors for all qpairs
  assert((control&0x7ff) > CMD_LOG_QPAIR_COUNT);

  // find address of msix table, should in BAR0
  spdk_pci_device_cfg_read32(pci, &table_offset, msix_base+4);
  assert((table_offset&0x7) == 0);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "msix vector table address: 0x%x\n", table_offset);

  // fill msix_data address in msix table, one entry for one qpair, disable
  for (uint32_t i=0; i<CMD_LOG_QPAIR_COUNT; i++)
  {
    uint32_t data;
    uint32_t offset = table_offset + 16*i;
    uint64_t addr = spdk_vtophys(&cmd_log_queue_table[i].msix_data, NULL);

    SPDK_DEBUGLOG(SPDK_LOG_NVME, "vector %d data addr 0x%lx\n", i, addr);

    // clear the interrupt
    cmd_log_queue_table[i].msix_data = 0;
    cmd_log_queue_table[i].msix_enabled = true;

    // fill the vector table
    data = (uint32_t)addr;
    nvme_pcie_ctrlr_set_reg_4(ctrlr, offset, data);
    data = (uint32_t)(addr>>32);
    nvme_pcie_ctrlr_set_reg_4(ctrlr, offset+4, data);
    data = 1;
    nvme_pcie_ctrlr_set_reg_4(ctrlr, offset+8, data);
    data = 0;
    nvme_pcie_ctrlr_set_reg_4(ctrlr, offset+12, data);

    cmd_log_queue_table[i].mask_offset = offset+12;
  }

  // enable msix
  control |= 0x8000;
  spdk_pci_device_cfg_write16(pci, control, msix_base+2);
}


static void intc_fini(struct spdk_nvme_ctrlr* ctrlr)
{
  uint8_t msix_base;
  uint16_t control;
  struct spdk_pci_device* pci = spdk_nvme_ctrlr_get_pci_device(ctrlr);

  // find msix capability
  msix_base = intc_find_msix(pci);
  spdk_pci_device_cfg_read16(pci, &control, msix_base+2);

  // disable msix
  control &= (~0x8000);
  spdk_pci_device_cfg_write16(pci, control, msix_base+2);
}


void intc_clear(struct spdk_nvme_qpair* q)
{
  cmd_log_queue_table[q->id].msix_data = 0;
}


bool intc_isset(struct spdk_nvme_qpair* q)
{
  return cmd_log_queue_table[q->id].msix_data != 0;
}


void intc_mask(struct spdk_nvme_qpair* q)
{
  nvme_pcie_ctrlr_set_reg_4(q->ctrlr, cmd_log_queue_table[q->id].mask_offset, 1);
}


void intc_unmask(struct spdk_nvme_qpair* q)
{
  nvme_pcie_ctrlr_set_reg_4(q->ctrlr, cmd_log_queue_table[q->id].mask_offset, 0);
}


//// probe callbacks
///////////////////////////////

struct cb_ctx {
  struct spdk_nvme_transport_id* trid;
  struct spdk_nvme_ctrlr* ctrlr;
};

static bool probe_cb(void *cb_ctx,
                     const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts)
{
	if (trid->trtype == SPDK_NVME_TRANSPORT_PCIE)
  {
    struct spdk_nvme_transport_id* target = ((struct cb_ctx*)cb_ctx)->trid;
    if (0 != spdk_nvme_transport_id_compare(target, trid))
    {
      SPDK_ERRLOG("Wrong address %s\n", trid->traddr);
      return false;
    }

    opts->use_cmb_sqs = false;
		SPDK_INFOLOG(SPDK_LOG_NVME, "Attaching to NVMe Controller at %s\n",
                 trid->traddr);
	}
  else
  {
    SPDK_INFOLOG(SPDK_LOG_NVME, "Attaching to NVMe over Fabrics controller at %s:%s: %s\n",
                 trid->traddr, trid->trsvcid, trid->subnqn);
	}

	/* Set io_queue_size to UINT16_MAX, NVMe driver
	 * will then reduce this to MQES to maximize
	 * the io_queue_size as much as possible.
	 */
  opts->io_queue_size = UINT16_MAX;

	/* Set the header and data_digest */
  opts->header_digest = false;
	opts->data_digest = false;

	return true;
}


static void attach_cb(void *cb_ctx,
                      const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts)
{
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);

  SPDK_DEBUGLOG(SPDK_LOG_NVME,
                "attached device %s: %s, %d namespaces, pid %d\n",
                trid->traddr, cdata->mn,
                spdk_nvme_ctrlr_get_num_ns(ctrlr),
                getpid());

  ((struct cb_ctx*)cb_ctx)->ctrlr = ctrlr;
}


////module: pcie ctrlr
///////////////////////////////

struct spdk_pci_device* pcie_init(struct spdk_nvme_ctrlr* ctrlr)
{
  return spdk_nvme_ctrlr_get_pci_device(ctrlr);
}

int pcie_cfg_read8(struct spdk_pci_device* pci,
                   unsigned char* value,
                   unsigned int offset)
{
  return spdk_pci_device_cfg_read8(pci, value, offset);
}

int pcie_cfg_write8(struct spdk_pci_device* pci,
                    unsigned char value,
                    unsigned int offset)
{
  return spdk_pci_device_cfg_write8(pci, value, offset);
}


////module: nvme ctrlr
///////////////////////////////
struct spdk_nvme_ctrlr* nvme_probe(char* traddr)
{
  struct spdk_nvme_transport_id trid;
  struct cb_ctx cb_ctx;
	int rc;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "looking for NVMe @%s\n", traddr);

  // device address
  memset(&trid, 0, sizeof(trid));
  if (strchr(traddr, ':') == NULL)
  {
    // tcp/ip address: fixed port to 4420
    trid.trtype = SPDK_NVME_TRANSPORT_TCP;
    trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
    strncpy(trid.traddr, traddr, strlen(traddr)+1);
    strncpy(trid.trsvcid, "4420", 4+1);
    snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);
  }
  else
  {
    // pcie address: contains ':' characters
    trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
    strncpy(trid.traddr, traddr, strlen(traddr)+1);
  }

  cb_ctx.trid = &trid;
  cb_ctx.ctrlr = NULL;
  rc = spdk_nvme_probe(&trid, &cb_ctx, probe_cb, attach_cb, NULL);
  if (rc != 0 || cb_ctx.ctrlr == NULL)
  {
    SPDK_ERRLOG("not found device: %s, rc %d, cb_ctx.ctrlr %p\n",
                trid.traddr, rc, cb_ctx.ctrlr);
    return NULL;
  }

  return cb_ctx.ctrlr;
}

struct spdk_nvme_ctrlr* nvme_init(char * traddr)
{
  struct spdk_nvme_ctrlr* ctrlr;

  //enum the device
  ctrlr = nvme_probe(traddr);
  if (ctrlr == NULL)
  {
    return NULL;
  }
#if 0
  if (spdk_process_is_primary())
  {
    // enable msix interrupt
    intc_init(ctrlr);
  }
#endif
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "found device: %s\n", ctrlr->trid.traddr);
  return ctrlr;
}

int nvme_fini(struct spdk_nvme_ctrlr* ctrlr)
{
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "free ctrlr: %s\n", ctrlr->trid.traddr);

  if (ctrlr == NULL)
  {
    return 0;
  }

  // io qpairs should all be deleted before closing master controller
  if (true == spdk_process_is_primary() &&
      false == TAILQ_EMPTY(&ctrlr->active_io_qpairs))
  {
    return -1;
  }

  if (spdk_process_is_primary())
  {
    // disable msix interrupt
    intc_fini(ctrlr);
  }

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "close device: %s\n", ctrlr->trid.traddr);
  return spdk_nvme_detach(ctrlr);
}

int nvme_set_reg32(struct spdk_nvme_ctrlr* ctrlr,
                   unsigned int offset,
                   unsigned int value)
{
  return nvme_pcie_ctrlr_set_reg_4(ctrlr, offset, value);
}

int nvme_get_reg32(struct spdk_nvme_ctrlr* ctrlr,
                   unsigned int offset,
                   unsigned int* value)
{
  return nvme_pcie_ctrlr_get_reg_4(ctrlr, offset, value);
}

int nvme_wait_completion_admin(struct spdk_nvme_ctrlr* ctrlr)
{
  int32_t rc;

#if 0  
  // check msix interrupt
  if (cmd_log_queue_table[0].msix_enabled)
  {
    if (cmd_log_queue_table[0].msix_data == 0)
    {
      // to check it again later
      return 0;
    }
  }

  // mask the interrupt
  nvme_pcie_ctrlr_set_reg_4(ctrlr, cmd_log_queue_table[0].mask_offset, 1);
#endif
  
  // process all the completions
  rc = spdk_nvme_ctrlr_process_admin_completions(ctrlr);

#if 0
  // clear and un-mask the interrupt
  cmd_log_queue_table[0].msix_data = 0;
  nvme_pcie_ctrlr_set_reg_4(ctrlr, cmd_log_queue_table[0].mask_offset, 0);
#endif
  
  return rc;
}

void nvme_deallocate_ranges(struct spdk_nvme_ctrlr* ctrlr,
                            void* buf, unsigned int count)
{
  struct spdk_nvme_dsm_range *ranges = (struct spdk_nvme_dsm_range*)buf;

  for (unsigned int i=0; i<count; i++)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "deallocate lba 0x%lx, count %d\n",
                 ranges[i].starting_lba,
                 ranges[i].length);
    crc32_clear(ranges[i].starting_lba, ranges[i].length, 0, 0);
  }
}

int nvme_send_cmd_raw(struct spdk_nvme_ctrlr* ctrlr,
                      struct spdk_nvme_qpair *qpair,
                      unsigned int opcode,
                      unsigned int nsid,
                      void* buf, size_t len,
                      unsigned int cdw10,
                      unsigned int cdw11,
                      unsigned int cdw12,
                      unsigned int cdw13,
                      unsigned int cdw14,
                      unsigned int cdw15,
                      spdk_nvme_cmd_cb cb_fn,
                      void* cb_arg)
{
  int rc = 0;
  struct spdk_nvme_cmd cmd;

  assert(ctrlr != NULL);

  //setup cmd structure
  memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
  cmd.opc = opcode;
  cmd.nsid = nsid;
  cmd.cdw10 = cdw10;
  cmd.cdw11 = cdw11;
  cmd.cdw12 = cdw12;
  cmd.cdw13 = cdw13;
  cmd.cdw14 = cdw14;
  cmd.cdw15 = cdw15;

  if (qpair)
  {
    //send io cmd in qpair
    rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair, &cmd, buf, len, cb_fn, cb_arg);
  }
  else
  {
    //not qpair, admin cmd
    rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, &cmd, buf, len, cb_fn, cb_arg);
  }

  return rc;
}


void nvme_register_aer_cb(struct spdk_nvme_ctrlr* ctrlr,
                          spdk_nvme_aer_cb aer_cb,
                          void* aer_cb_arg)
{
  spdk_nvme_ctrlr_register_aer_callback(ctrlr, aer_cb, aer_cb_arg);
}

void nvme_register_timeout_cb(struct spdk_nvme_ctrlr* ctrlr,
                              spdk_nvme_timeout_cb timeout_cb,
                              unsigned int timeout)
{
  spdk_nvme_ctrlr_register_timeout_callback(
      ctrlr, (uint64_t)timeout*US_PER_S, timeout_cb, NULL);
}

int nvme_cpl_is_error(const struct spdk_nvme_cpl* cpl)
{
  return spdk_nvme_cpl_is_error(cpl);
}


////module: qpair
///////////////////////////////

struct spdk_nvme_qpair *qpair_create(struct spdk_nvme_ctrlr* ctrlr,
                                     int prio, int depth)
{
  struct spdk_nvme_qpair* qpair;
  struct spdk_nvme_io_qpair_opts opts;

  //user options
  opts.qprio = prio;
  opts.io_queue_size = depth;
  opts.io_queue_requests = depth*2;

  qpair = spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &opts, sizeof(opts));
  if (qpair == NULL)
  {
    SPDK_ERRLOG("alloc io qpair fail\n");
    return NULL;
  }

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "created qpair %d\n", qpair->id);

  // limited qpair count
  if (qpair->id >= CMD_LOG_QPAIR_COUNT)
  {
    spdk_nvme_ctrlr_free_io_qpair(qpair);
    return NULL;
  }

  cmd_log_qpair_init(qpair);
  return qpair;
}

int qpair_wait_completion(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
  return spdk_nvme_qpair_process_completions(qpair, max_completions);
}

int qpair_get_id(struct spdk_nvme_qpair* q)
{
  // q NULL is admin queue
  return q ? q->id : 0;
}

int qpair_free(struct spdk_nvme_qpair* q)
{
  if (q == NULL)
  {
    return 0;
  }

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "free qpair: %d\n", q->id);
  cmd_log_qpair_clear(q->id);

  return spdk_nvme_ctrlr_free_io_qpair(q);
}


////module: namespace
///////////////////////////////

struct spdk_nvme_ns* ns_init(struct spdk_nvme_ctrlr* ctrlr, uint32_t nsid)
{
  struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
  uint64_t nsze = spdk_nvme_ns_get_num_sectors(ns);

  assert(ns != NULL);
  if (0 != memzone_reserve_shared_memory(sizeof(uint32_t)*nsze))
  {
    return NULL;
  }

  return ns;
}

int ns_refresh(struct spdk_nvme_ns *ns, uint32_t id, struct spdk_nvme_ctrlr *ctrlr)
{
  return nvme_ns_construct(ns, id, ctrlr);
}

int ns_cmd_read_write(int is_read,
                      struct spdk_nvme_ns* ns,
                      struct spdk_nvme_qpair* qpair,
                      void* buf,
                      size_t len,
                      uint64_t lba,
                      uint16_t lba_count,
                      uint32_t io_flags,
                      spdk_nvme_cmd_cb cb_fn,
                      void* cb_arg)
{
  struct spdk_nvme_cmd cmd;
  uint32_t lba_size = spdk_nvme_ns_get_sector_size(ns);

  assert(ns != NULL);
  assert(qpair != NULL);

  //only support 1 namespace now
  assert(ns->id == 1);

  //validate data buffer
  assert(buf != NULL);
  assert(len >= lba_count*lba_size);
  assert((io_flags&0xffff) == 0);

  //setup cmd structure
  memset(&cmd, 0, sizeof(struct spdk_nvme_cmd));
  cmd.opc = is_read ? 2 : 1;
  cmd.nsid = ns->id;
  cmd.cdw10 = lba;
  cmd.cdw11 = lba>>32;
  cmd.cdw12 = io_flags | (lba_count-1);
  cmd.cdw13 = 0;
  cmd.cdw14 = 0;
  cmd.cdw15 = 0;

  //fill write buffer with lba, token, and checksum
  if (is_read != true)
  {
    //for write buffer
    buffer_fill_data(buf, lba, lba_count, lba_size);
  }

  //send io cmd in qpair
  return spdk_nvme_ctrlr_cmd_io_raw(ns->ctrlr, qpair, &cmd, buf, len, cb_fn, cb_arg);
}

uint32_t ns_get_sector_size(struct spdk_nvme_ns* ns)
{
  return spdk_nvme_ns_get_sector_size(ns);
}

uint64_t ns_get_num_sectors(struct spdk_nvme_ns* ns)
{
  return spdk_nvme_ns_get_num_sectors(ns);
}

int ns_fini(struct spdk_nvme_ns* ns)
{
  crc32_fini();
  return 0;
}


////module: ioworker
///////////////////////////////

// used for callback
struct ioworker_io_ctx {
  void* data_buf;
  size_t data_buf_len;
  bool is_read;
  struct timeval time_sent;
  struct ioworker_global_ctx* gctx;
};

struct ioworker_global_ctx {
  struct ioworker_args* args;
  struct ioworker_rets* rets;
  struct spdk_nvme_ns* ns;
  struct spdk_nvme_qpair *qpair;
  struct timeval due_time;
  struct timeval io_due_time;
  struct timeval io_delay_time;
  struct timeval time_next_sec;
  uint64_t io_count_till_last_sec;
  uint64_t sequential_lba;
  uint64_t io_count_sent;
  uint64_t io_count_cplt;
  uint32_t last_sec;
  bool flag_finish;
};

#define ALIGN_UP(n, a)    (((n)%(a))?((n)+(a)-((n)%(a))):((n)))
#define ALIGN_DOWN(n, a)  ((n)-((n)%(a)))

static int ioworker_send_one(struct spdk_nvme_ns* ns,
                             struct spdk_nvme_qpair *qpair,
                             struct ioworker_io_ctx* ctx,
                             struct ioworker_global_ctx* gctx);


static inline void timeradd_second(struct timeval* now,
                                     unsigned int seconds,
                                     struct timeval* due)
{
  struct timeval duration;

  duration.tv_sec = seconds;
  duration.tv_usec = 0;
  timeradd(now, &duration, due);
}

static bool ioworker_send_one_is_finish(struct ioworker_args* args,
                                        struct ioworker_global_ctx* c)
{
  struct timeval now;

  // limit by io count, and/or time, which happens first
  if (c->io_count_sent == args->io_count)
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker finish, sent %ld io\n", c->io_count_sent);
    return true;
  }

  assert(c->io_count_sent < args->io_count);
  gettimeofday(&now, NULL);
  if (true == timercmp(&now, &c->due_time, >))
  {
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker finish, due time %ld us\n", c->due_time.tv_usec);
    return true;
  }

  return false;
}

static void ioworker_one_io_throttle(struct ioworker_global_ctx* gctx,
                                     struct timeval* now)
{
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "this io due at %ld.%06ld\n",
                gctx->io_due_time.tv_sec, gctx->io_due_time.tv_usec);
  if (true == timercmp(&gctx->io_due_time, now, >))
  {
    //delay usec to meet the IOPS prequisit
    struct timeval diff;
    timersub(&gctx->io_due_time, now, &diff);
    usleep(timeval_to_us(&diff));
  }

  timeradd(&gctx->io_due_time, &gctx->io_delay_time, &gctx->io_due_time);
}

static uint32_t ioworker_get_duration(struct timeval* start,
                                      struct ioworker_global_ctx* gctx)
{
  struct timeval now;
  struct timeval diff;
  uint32_t msec;

  gettimeofday(&now, NULL);
  timersub(&now, start, &diff);
  msec = diff.tv_sec * 1000UL;
  return msec + (diff.tv_usec+500)/1000;
}

static uint32_t ioworker_update_rets(struct ioworker_io_ctx* ctx,
                                     struct ioworker_rets* ret,
                                     struct timeval* now)
{
  struct timeval diff;
  uint32_t latency;

  timersub(now, &ctx->time_sent, &diff);
  latency = timeval_to_us(&diff);
  if (latency > ret->latency_max_us)
  {
    ret->latency_max_us = latency;
  }

  if (ctx->is_read == true)
  {
    ret->io_count_read ++;
  }
  else
  {
    ret->io_count_write ++;
  }

  return latency;
}

static inline void ioworker_update_io_count_per_second(
    struct ioworker_global_ctx* gctx,
    struct ioworker_args* args,
    struct ioworker_rets* rets)
{
  uint64_t current_io_count = rets->io_count_read + rets->io_count_write;

  // update to next second
  timeradd_second(&gctx->time_next_sec, 1, &gctx->time_next_sec);
  args->io_counter_per_second[gctx->last_sec ++] = current_io_count - gctx->io_count_till_last_sec;
  gctx->io_count_till_last_sec = current_io_count;
}

static void ioworker_one_cb(void* ctx_in, const struct spdk_nvme_cpl *cpl)
{
  uint32_t latency_us;
  struct timeval now;
  struct ioworker_io_ctx* ctx = (struct ioworker_io_ctx*)ctx_in;
  struct ioworker_args* args = ctx->gctx->args;
  struct ioworker_global_ctx* gctx = ctx->gctx;
  struct ioworker_rets* rets = gctx->rets;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "one io completed, ctx %p, io delay time: %ld\n",
               ctx, gctx->io_delay_time.tv_usec);

  gctx->io_count_cplt ++;

  // update statistics in ret structure
  gettimeofday(&now, NULL);
  assert(rets != NULL);
  latency_us = ioworker_update_rets(ctx, rets, &now);

  // update io count per latency
  if (args->io_counter_per_latency != NULL)
  {
    args->io_counter_per_latency[MIN(US_PER_S-1, latency_us)] ++;
  }

  // throttle IOPS by delay
  if (gctx->io_delay_time.tv_usec != 0)
  {
    ioworker_one_io_throttle(gctx, &now);
  }

  if (true == nvme_cpl_is_error(cpl))
  {
    // terminate ioworker when any error happen
    // only keep the first error code
    uint16_t error = ((*(unsigned short*)(&cpl->status))>>1)&0x7ff;
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "ioworker error happen in cpl\n");
    gctx->flag_finish = true;
    if (rets->error == 0)
    {
      rets->error = error;
    }
  }

  // update io counter per second when required
  if (args->io_counter_per_second != NULL)
  {
    if (true == timercmp(&now, &gctx->time_next_sec, >))
    {
      ioworker_update_io_count_per_second(gctx, args, rets);
    }
  }

  // check if all io are sent
  if (gctx->flag_finish != true)
  {
    //update finish flag
    gctx->flag_finish = ioworker_send_one_is_finish(args, gctx);
  }

  if (gctx->flag_finish != true)
  {
    // send more io
    ioworker_send_one(gctx->ns, gctx->qpair, ctx, gctx);
  }
}

static inline bool ioworker_send_one_is_read(unsigned short read_percentage)
{
  return random()%100 < read_percentage;
}

static uint64_t ioworker_send_one_lba_sequential(struct ioworker_args* args,
                                                 struct ioworker_global_ctx* gctx)
{
  uint64_t ret;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "gctx lba: 0x%lx, align:%d, end: 0x%lx\n",
                gctx->sequential_lba, args->lba_align, args->region_end);

  ret = gctx->sequential_lba;
  if (ret > args->region_end)
  {
    ret = args->region_start;
  }

  return ret;
}

static inline uint64_t ioworker_send_one_lba_random(struct ioworker_args* args)
{
  return (random()%(args->region_end-args->region_start)) + args->region_start;
}

static uint64_t ioworker_send_one_lba(struct ioworker_args* args,
                                      struct ioworker_global_ctx* gctx)
{
  uint64_t ret;

  if (args->lba_random == 0)
  {
    ret = ioworker_send_one_lba_sequential(args, gctx);
    gctx->sequential_lba = ret;
  }
  else
  {
    ret = ioworker_send_one_lba_random(args);
  }

  return ALIGN_DOWN(ret, args->lba_align);
}

static int ioworker_send_one(struct spdk_nvme_ns* ns,
                             struct spdk_nvme_qpair *qpair,
                             struct ioworker_io_ctx* ctx,
                             struct ioworker_global_ctx* gctx)
{
  int ret;
  struct ioworker_args* args = gctx->args;
  bool is_read = ioworker_send_one_is_read(args->read_percentage);
  uint64_t lba_starting = ioworker_send_one_lba(args, gctx);
  uint16_t lba_count = args->lba_size;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "one io: ctx %p, lba 0x%lx, count %d\n",
                ctx, lba_starting, lba_count);

  assert(ctx->data_buf != NULL);

  ret = ns_cmd_read_write(is_read, ns, qpair,
                          ctx->data_buf, ctx->data_buf_len,
                          lba_starting, lba_count,
                          0,  //do not have more options in ioworkers
                          ioworker_one_cb, ctx);
  if (ret != 0)
  {
    SPDK_NOTICELOG("ioworker error happen in cpl\n");
    gctx->flag_finish = true;
    return ret;
  }

  //sent one io cmd successfully
  gctx->sequential_lba += args->lba_align;
  gctx->io_count_sent ++;
  ctx->is_read = is_read;
  gettimeofday(&ctx->time_sent, NULL);
  return 0;
}


int ioworker_entry(struct spdk_nvme_ns* ns,
                   struct spdk_nvme_qpair *qpair,
                   struct ioworker_args* args,
                   struct ioworker_rets* rets)
{
  int ret = 0;
  uint64_t nsze = spdk_nvme_ns_get_num_sectors(ns);
  uint32_t sector_size = spdk_nvme_ns_get_sector_size(ns);
  struct timeval test_start;
  struct ioworker_global_ctx gctx;
  struct ioworker_io_ctx* io_ctx = malloc(sizeof(struct ioworker_io_ctx)*args->qdepth);

  assert(ns != NULL);
  assert(qpair != NULL);
  assert(args != NULL);
  assert(rets != NULL);

  //init rets
  rets->io_count_read = 0;
  rets->io_count_write = 0;
  rets->latency_max_us = 0;
  rets->mseconds = 0;
  rets->error = 0;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_start = %ld\n", args->lba_start);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_size = %d\n", args->lba_size);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_align = %d\n", args->lba_align);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.lba_random = %d\n", args->lba_random);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.region_start = %ld\n", args->region_start);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.region_end = %ld\n", args->region_end);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.read_percentage = %d\n", args->read_percentage);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.iops = %d\n", args->iops);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.io_count = %ld\n", args->io_count);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.seconds = %d\n", args->seconds);
  SPDK_DEBUGLOG(SPDK_LOG_NVME, "args.qdepth = %d\n", args->qdepth);

  //check args
  assert(args->read_percentage <= 100);
  assert(args->io_count != 0 || args->seconds != 0);
  assert(args->seconds < 24*3600ULL);
  assert(args->lba_size != 0);
  assert(args->region_start < args->region_end);
  assert(args->read_percentage >= 0);
  assert(args->read_percentage <= 100);
  assert(args->qdepth <= CMD_LOG_DEPTH/2);

  // check io size
  if (args->lba_size*sector_size > ns->ctrlr->max_xfer_size)
  {
    SPDK_ERRLOG("IO size is larger than max xfer size, %d\n", ns->ctrlr->max_xfer_size);
    rets->error = 0x0002;  // Invalid Field in Command
    free(io_ctx);
    return -2;
  }

  //revise args
  if (args->io_count == 0)
  {
    args->io_count = (unsigned long)-1;
  }
  if (args->seconds == 0 || args->seconds > 24*3600ULL)
  {
    // run ioworker for 24hr at most
    args->seconds = 24*3600ULL;
  }
  if (args->region_end > nsze)
  {
    args->region_end = nsze;
  }

  //adjust region to start_lba's region
  args->region_start = ALIGN_UP(args->region_start, args->lba_align);
  args->region_end = args->region_end-args->lba_size;
  args->region_end = ALIGN_DOWN(args->region_end, args->lba_align);
  if (args->lba_start < args->region_start)
  {
    args->lba_start = args->region_start;
  }
  if (args->io_count < args->qdepth)
  {
    args->qdepth = args->io_count;
  }

  //init global ctx
  memset(&gctx, 0, sizeof(gctx));
  gctx.ns = ns;
  gctx.qpair = qpair;
  gctx.sequential_lba = args->lba_start;
  gctx.io_count_sent = 0;
  gctx.io_count_cplt = 0;
  gctx.flag_finish = false;
  gctx.args = args;
  gctx.rets = rets;
  gettimeofday(&test_start, NULL);
  timeradd_second(&test_start, args->seconds, &gctx.due_time);
  gctx.io_delay_time.tv_sec = 0;
  gctx.io_delay_time.tv_usec = args->iops ? US_PER_S/args->iops : 0;
  timeradd(&test_start, &gctx.io_delay_time, &gctx.io_due_time);
  timeradd_second(&test_start, 1, &gctx.time_next_sec);
  gctx.io_count_till_last_sec = 0;
  gctx.last_sec = 0;

  // sending the first batch of IOs, all remaining IOs are sending
  // in callbacks till end
  for (unsigned int i=0; i<args->qdepth; i++)
  {
    io_ctx[i].data_buf_len = args->lba_size * sector_size;
    io_ctx[i].data_buf = buffer_init(io_ctx[i].data_buf_len, NULL);
    io_ctx[i].gctx = &gctx;
    ioworker_send_one(ns, qpair, &io_ctx[i], &gctx);
  }

  // callbacks check the end condition and mark the flag. Check the
  // flag here if it is time to stop the ioworker and return the
  // statistics data
  while (gctx.io_count_sent != gctx.io_count_cplt ||
         gctx.flag_finish != true)
  {
    //exceed 30 seconds more than the expected test time, abort ioworker
    if (ioworker_get_duration(&test_start, &gctx) >
        args->seconds*1000UL + 30*1000UL)
    {
      //ioworker timeout
      SPDK_INFOLOG(SPDK_LOG_NVME, "ioworker timeout, io sent %ld, io cplt %ld, finish %d\n",
                   gctx.io_count_sent, gctx.io_count_cplt, gctx.flag_finish);
      ret = -4;
      break;
    }

    // collect completions
    spdk_nvme_qpair_process_completions(qpair, 0);
  }

  // final duration
  rets->mseconds = ioworker_get_duration(&test_start, &gctx);

  //release io ctx
  for (unsigned int i=0; i<args->qdepth; i++)
  {
    buffer_fini(io_ctx[i].data_buf);
  }

  free(io_ctx);
  return ret;
}


////module: log
///////////////////////////////

char* log_buf_dump(const char* header, const void* buf, size_t len)
{
  size_t size;
  FILE* fd = NULL;
  char* tmpname = "/tmp/pynvme_buf_dump.tmp";
  static char dump_buf[64*1024];

  // dump buf is limited
  assert(len <= 4096);

  errno = 0;
  fd = fopen(tmpname, "w+");
  if (fd == NULL)
  {
    SPDK_WARNLOG("fopen: %s\n", strerror(errno));
    return NULL;
  }

  spdk_log_dump(fd, header, buf, len);

  // get file size
  size = ftell(fd);

  errno = 0;
  if (-1 == fseek(fd, 0, SEEK_SET))
  {
    SPDK_WARNLOG("lseek: %s\n", strerror(errno));
    return NULL;
  }

  // read the data from temporary file
  errno=0;
  if (fread(dump_buf, size, 1, fd) == 0)
  {
    SPDK_WARNLOG("read: %s\n", strerror(errno));
    return NULL;
  }

  fclose(fd);
  return dump_buf;
}

void log_cmd_dump(struct spdk_nvme_qpair* qpair, size_t count)
{
  uint32_t dump_count = count;
  uint16_t qid = qpair->id;
  struct cmd_log_table_t* table = &cmd_log_queue_table[qid];
  uint32_t index = cmd_log_queue_table[qid].tail_index;
  uint32_t seq = 0;

  assert(qid < CMD_LOG_QPAIR_COUNT);
  assert(table != NULL);

  if (count == 0 || count > CMD_LOG_DEPTH)
  {
    dump_count = CMD_LOG_DEPTH;
  }

  // cmdlog is NOT SQ/CQ. cmdlog keeps CMD/CPL for script test debug purpose
  SPDK_NOTICELOG("dump qpair %d, latest tail in cmdlog: %d\n",
                 qid, table->tail_index);

  // only send the most recent part of cmdlog
  while (seq++ < dump_count)
  {
    // get the next index to read log
    if (index == 0)
    {
      index = CMD_LOG_DEPTH;
    }
    index -= 1;

    // no timeval, empty slot, not print
    struct timeval tv = table->table[index].time_cmd;
    if (timercmp(&tv, &(struct timeval){0}, !=))
    {
      struct tm* time;
      char tmbuf[64];

      //cmd part
      tv = table->table[index].time_cmd;
      time = localtime(&tv.tv_sec);
      strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", time);
      SPDK_NOTICELOG("index %d, %s.%06ld, req %p\n", index, tmbuf, tv.tv_usec, &table->table[index].req);
      nvme_qpair_print_command(qpair, &table->table[index].cmd);

      //cpl part
      tv.tv_usec = table->table[index].cpl_latency_us;
      timeradd(&table->table[index].time_cmd, &tv, &tv);
      time = localtime(&tv.tv_sec);
      strftime(tmbuf, sizeof(tmbuf), "%Y-%m-%d %H:%M:%S", time);
      SPDK_NOTICELOG("index %d, %s.%06ld\n", index, tmbuf, tv.tv_usec);
      nvme_qpair_print_completion(qpair, &table->table[index].cpl);
    }
  } 
}

void log_cmd_dump_admin(struct spdk_nvme_ctrlr* ctrlr, size_t count)
{
  log_cmd_dump(ctrlr->adminq, count);
}


////module: commands name, SPDK
///////////////////////////////

static const char *
admin_opc_name(uint8_t opc)
{
	switch (opc) {
	case SPDK_NVME_OPC_DELETE_IO_SQ:
		return "Delete I/O Submission Queue";
	case SPDK_NVME_OPC_CREATE_IO_SQ:
		return "Create I/O Submission Queue";
	case SPDK_NVME_OPC_GET_LOG_PAGE:
		return "Get Log Page";
	case SPDK_NVME_OPC_DELETE_IO_CQ:
		return "Delete I/O Completion Queue";
	case SPDK_NVME_OPC_CREATE_IO_CQ:
		return "Create I/O Completion Queue";
	case SPDK_NVME_OPC_IDENTIFY:
		return "Identify";
	case SPDK_NVME_OPC_ABORT:
		return "Abort";
	case SPDK_NVME_OPC_SET_FEATURES:
		return "Set Features";
	case SPDK_NVME_OPC_GET_FEATURES:
		return "Get Features";
	case SPDK_NVME_OPC_ASYNC_EVENT_REQUEST:
		return "Asynchronous Event Request";
	case SPDK_NVME_OPC_NS_MANAGEMENT:
		return "Namespace Management";
	case SPDK_NVME_OPC_FIRMWARE_COMMIT:
		return "Firmware Commit";
	case SPDK_NVME_OPC_FIRMWARE_IMAGE_DOWNLOAD:
		return "Firmware Image Download";
	case SPDK_NVME_OPC_DEVICE_SELF_TEST:
		return "Device Self-test";
	case SPDK_NVME_OPC_NS_ATTACHMENT:
		return "Namespace Attachment";
	case SPDK_NVME_OPC_KEEP_ALIVE:
		return "Keep Alive";
	case SPDK_NVME_OPC_DIRECTIVE_SEND:
		return "Directive Send";
	case SPDK_NVME_OPC_DIRECTIVE_RECEIVE:
		return "Directive Receive";
	case SPDK_NVME_OPC_VIRTUALIZATION_MANAGEMENT:
		return "Virtualization Management";
	case SPDK_NVME_OPC_NVME_MI_SEND:
		return "NVMe-MI Send";
	case SPDK_NVME_OPC_NVME_MI_RECEIVE:
		return "NVMe-MI Receive";
	case SPDK_NVME_OPC_DOORBELL_BUFFER_CONFIG:
		return "Doorbell Buffer Config";
	case SPDK_NVME_OPC_FORMAT_NVM:
		return "Format NVM";
	case SPDK_NVME_OPC_SECURITY_SEND:
		return "Security Send";
	case SPDK_NVME_OPC_SECURITY_RECEIVE:
		return "Security Receive";
	case SPDK_NVME_OPC_SANITIZE:
		return "Sanitize";
	default:
		if (opc >= 0xC0) {
			return "Vendor specific";
		}
		return "Unknown";
	}
}

static const char *
io_opc_name(uint8_t opc)
{
	switch (opc) {
	case SPDK_NVME_OPC_FLUSH:
		return "Flush";
	case SPDK_NVME_OPC_WRITE:
		return "Write";
	case SPDK_NVME_OPC_READ:
		return "Read";
	case SPDK_NVME_OPC_WRITE_UNCORRECTABLE:
		return "Write Uncorrectable";
	case SPDK_NVME_OPC_COMPARE:
		return "Compare";
	case SPDK_NVME_OPC_WRITE_ZEROES:
		return "Write Zeroes";
	case SPDK_NVME_OPC_DATASET_MANAGEMENT:
		return "Dataset Management";
	case SPDK_NVME_OPC_RESERVATION_REGISTER:
		return "Reservation Register";
	case SPDK_NVME_OPC_RESERVATION_REPORT:
		return "Reservation Report";
	case SPDK_NVME_OPC_RESERVATION_ACQUIRE:
		return "Reservation Acquire";
	case SPDK_NVME_OPC_RESERVATION_RELEASE:
		return "Reservation Release";
	default:
		if (opc >= 0x80) {
			return "Vendor specific";
		}
		return "Unknown command";
	}
}

const char* cmd_name(uint8_t opc, int set)
{
  if (set == 0)
  {
    return admin_opc_name(opc);
  }
  else if (set == 1)
  {
    return io_opc_name(opc);
  }
  else
  {
    return "Unknown command set";
  }
}


////rpc
///////////////////////////////

static void* rpc_server(void* args)
{
  int rc = 0;

  SPDK_DEBUGLOG(SPDK_LOG_NVME, "starting rpc server ...\n");

  // start the rpc
  rc = spdk_rpc_listen("/var/tmp/pynvme.sock");
  if (rc != 0)
  {
    SPDK_ERRLOG("rpc fail to get the sock \n");
    return NULL;
  }

  // pynvme run as root, but rpc client no need
  chmod("/var/tmp/pynvme.sock", 0777);

  spdk_rpc_set_state(SPDK_RPC_STARTUP);

  while(1)
  {
    spdk_rpc_accept();
    usleep(100000);
  }

  spdk_rpc_close();
}


static void
rpc_list_all_qpair(struct spdk_jsonrpc_request *request,
                   const struct spdk_json_val *params)
{
  struct spdk_json_write_ctx *w;

  w = spdk_jsonrpc_begin_result(request);
  if (w == NULL)
  {
    return;
  }

  spdk_json_write_array_begin(w);
  for (int i=0; i<CMD_LOG_QPAIR_COUNT; i++)
  {
    // only send valid qpair
    if (cmd_log_queue_table[i].tail_index < CMD_LOG_DEPTH)
    {
      uint32_t outstanding = 0;
      struct spdk_nvme_qpair* qpair = cmd_log_queue_table[i].qpair;

      if (qpair != NULL)
      {
        outstanding = nvme_pcie_qpair_outstanding_count(qpair);
      }

      // limit blocks in UI
      if (outstanding > 100)
      {
        outstanding = 100;
      }

      //json: leading 0 means octal, so +1 to avoid it
      spdk_json_write_uint32(w, i+1 + ((outstanding/4)<<16));
    }
  }
  spdk_json_write_array_end(w);
  spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("list_all_qpair", rpc_list_all_qpair, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)


static void
rpc_get_cmdlog(struct spdk_jsonrpc_request *request,
               const struct spdk_json_val *params)
{
  uint32_t qid;
  size_t count;
  struct spdk_json_write_ctx *w;

	if (params == NULL)
  {
    SPDK_ERRLOG("no parameters\n");
    spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "Invalid parameters");
    return;
  }

  if (spdk_json_decode_array(params, spdk_json_decode_uint32,
                             &qid, 1, &count, sizeof(uint32_t)))
  {
    SPDK_ERRLOG("spdk_json_decode_object failed\n");
    spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "Invalid parameters");
    return;
  }

  if (count != 1)
  {
    SPDK_ERRLOG("only 1 parameter required for qid\n");
    spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                     "Invalid parameters");
    return;
  }

  qid = qid-1;  //avoid 0 in json

  w = spdk_jsonrpc_begin_result(request);
  if (w == NULL)
  {
    return;
  }

  struct cmd_log_entry_t* table = cmd_log_queue_table[qid].table;
  uint32_t index = cmd_log_queue_table[qid].tail_index;
  uint32_t seq = 0;

  // list the cmdlog in reversed order
  spdk_json_write_array_begin(w);

  // only send the most recent part of cmdlog
  do
  {
    // get the next index to read log
    if (index == 0)
    {
      index = CMD_LOG_DEPTH;
    }
    index -= 1;

    // no timeval, empty slot, not print
    struct timeval time_cmd = table[index].time_cmd;
    if (timercmp(&time_cmd, &(struct timeval){0}, !=))
    {
      // get the string of the op name
      const char* cmdname = cmd_name(table[index].cmd.opc, qid==0?0:1);
      uint32_t* cmd = (uint32_t*)&table[index].cmd;
      spdk_json_write_string_fmt(w, "%ld.%06ld: [cmd: %s] \n"
                                 "0x%08x, 0x%08x, 0x%08x, 0x%08x\n"
                                 "0x%08x, 0x%08x, 0x%08x, 0x%08x\n"
                                 "0x%08x, 0x%08x, 0x%08x, 0x%08x\n"
                                 "0x%08x, 0x%08x, 0x%08x, 0x%08x",
                                 time_cmd.tv_sec, time_cmd.tv_usec,
                                 cmdname,
                                 cmd[0], cmd[1], cmd[2], cmd[3],
                                 cmd[4], cmd[5], cmd[6], cmd[7],
                                 cmd[8], cmd[9], cmd[10], cmd[11],
                                 cmd[12], cmd[13], cmd[14], cmd[15]);

      if (table[index].cpl_latency_us != 0)
      {
        // a completed command, display its cpl cdws
        struct timeval time_cpl = (struct timeval){0};
        time_cpl.tv_usec = table[index].cpl_latency_us;
        timeradd(&time_cmd, &time_cpl, &time_cpl);

        uint32_t* cpl = (uint32_t*)&table[index].cpl;
        const char* sts = nvme_qpair_get_status_string(&table[index].cpl);
        spdk_json_write_string_fmt(w, "%ld.%06ld: [cpl: %s] \n"
                                   "0x%08x, 0x%08x, 0x%08x, 0x%08x\n",
                                   time_cpl.tv_sec, time_cpl.tv_usec,
                                   sts,
                                   cpl[0], cpl[1], cpl[2], cpl[3]);
      }
      else
      {
        spdk_json_write_string_fmt(w, "not completed ...\n");
      }
    }
  } while (seq++ < 100);

  spdk_json_write_array_end(w);
  spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("get_cmdlog", rpc_get_cmdlog, SPDK_RPC_STARTUP | SPDK_RPC_RUNTIME)


////driver system
///////////////////////////////

int driver_init(void)
{
  int ret = 0;
  char buf[20];
  struct spdk_env_opts opts;

  //init random sequence reproducible
  srandom(1);

  // distribute multiprocessing to different cores
  spdk_env_opts_init(&opts);
  sprintf(buf, "0x%x", 1<<(getpid()%get_nprocs()));
  opts.core_mask = buf;
  opts.shm_id = 0;
  opts.name = "pynvme";
  opts.mem_size = 512;
  if (spdk_env_init(&opts) < 0)
  {
    fprintf(stderr, "Unable to initialize SPDK env\n");
    return -1;
  }

  // distribute multiprocessing to different cores
  // log level setup
  spdk_log_set_flag("nvme");
  spdk_log_set_print_level(SPDK_LOG_INFO);

  // start rpc server in primary process only
  if (spdk_process_is_primary())
  {
    pthread_t rpc_t;
    pthread_create(&rpc_t, NULL, rpc_server, NULL);
  }

  // init cmd log
  ret = cmd_log_init();
  if (ret != 0)
  {
    return ret;
  }

  // init admin cmd log
  if (spdk_process_is_primary())
  {
    cmd_log_qpair_init(NULL);
  }

  return ret;
}


int driver_fini(void)
{
  //delete cmd log of admin queue
  if (spdk_process_is_primary())
  {
    cmd_log_qpair_clear(0);
    cmd_log_finish();
    SPDK_DEBUGLOG(SPDK_LOG_NVME, "pynvme driver unloaded.\n");
  }

	return spdk_env_cleanup();
}


uint64_t driver_config(uint64_t cfg_word)
{
  if (g_driver_csum_table_ptr != NULL)
  {
    *g_driver_global_config_ptr = cfg_word;
  }
  else
  {
    SPDK_INFOLOG(SPDK_LOG_NVME, "no enough memory, not to enable data verification feature.\n");
  }

  return *g_driver_global_config_ptr;
}
