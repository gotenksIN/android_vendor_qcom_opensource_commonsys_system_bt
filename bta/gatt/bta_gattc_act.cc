/******************************************************************************
 *
 *  Copyright (C) 2003-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *
 *  Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted (subject to the limitations in the
 *  disclaimer below) provided that the following conditions are met:
 *
 *  Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *  Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following
 *  disclaimer in the documentation and/or other materials provided
 *  with the distribution.
 *
 *  Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *  contributors may be used to endorse or promote products derived
 *  from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains the GATT client action functions for the state
 *  machine.
 *
 ******************************************************************************/

#define LOG_TAG "bt_bta_gattc"

#include <string.h>

#include <base/callback.h>
#include "bt_common.h"
#include "bt_target.h"
#include "bta_closure_api.h"
#include "bta_gattc_int.h"
#include "bta_sys.h"
#include "btif/include/btif_debug_conn.h"
#include "l2c_api.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "stack/l2cap/l2c_int.h"
#include "utl.h"
#include "device/include/interop.h"
#include "stack/btm/btm_int_types.h"
#include "btif/include/btif_storage.h"
#include "btm_int.h"
#include "stack/include/gap_api.h"

#if (BTA_HH_LE_INCLUDED == TRUE)
#include "bta_hh_int.h"
#endif

#include "osi/include/osi.h"
#include "osi/include/socket_utils/sockets.h"
#include "osi/include/properties.h"
#include "stack/gatt/gatt_int.h"

#ifdef ADV_AUDIO_FEATURE
#include "bta_dm_adv_audio.h"
#endif

#if (OFF_TARGET_TEST_ENABLED == TRUE)
#include "bt_prop.h"
#endif

using base::StringPrintf;
using bluetooth::Uuid;

#ifdef ADV_AUDIO_FEATURE
extern tACL_CONN* btm_bda_to_acl(const RawAddress& bda, tBT_TRANSPORT transport);
extern bool is_remote_support_adv_audio(const RawAddress remote_bdaddr);
#endif

/*****************************************************************************
 *  Constants
 ****************************************************************************/
static void bta_gattc_conn_cback(tGATT_IF gattc_if, const RawAddress& bda,
                                 uint16_t conn_id, bool connected,
                                 tGATT_DISCONN_REASON reason,
                                 tBT_TRANSPORT transport);

static void bta_gattc_cmpl_cback(uint16_t conn_id, tGATTC_OPTYPE op,
                                 tGATT_STATUS status,
                                 tGATT_CL_COMPLETE* p_data, uint32_t trans_id);

static void bta_gattc_deregister_cmpl(tBTA_GATTC_RCB* p_clreg);
static void bta_gattc_enc_cmpl_cback(tGATT_IF gattc_if, const RawAddress& bda);
static void bta_gattc_cong_cback(uint16_t conn_id, bool congested);
static void bta_gattc_phy_update_cback(tGATT_IF gatt_if, uint16_t conn_id,
                                       uint8_t tx_phy, uint8_t rx_phy,
                                       uint8_t status);
static void bta_gattc_conn_update_cback(tGATT_IF gatt_if, uint16_t conn_id,
                                        uint16_t interval, uint16_t latency,
                                        uint16_t timeout, uint8_t status);
static void bta_gattc_subrate_chg_cback(tGATT_IF gatt_if, uint16_t conn_id,
                                        uint16_t subrate_factor, uint16_t latency,
                                        uint16_t cont_num, uint16_t timeout, uint8_t status);

static tGATT_CBACK bta_gattc_cl_cback = {bta_gattc_conn_cback,
                                         bta_gattc_cmpl_cback,
                                         bta_gattc_disc_res_cback,
                                         bta_gattc_disc_cmpl_cback,
                                         NULL,
                                         bta_gattc_enc_cmpl_cback,
                                         bta_gattc_cong_cback,
                                         bta_gattc_phy_update_cback,
                                         bta_gattc_conn_update_cback,
                                         bta_gattc_subrate_chg_cback};

/* opcode(tGATTC_OPTYPE) order has to be comply with internal event order */
static uint16_t bta_gattc_opcode_to_int_evt[] = {
    /* Skip: GATTC_OPTYPE_NONE */
    /* Skip: GATTC_OPTYPE_DISCOVERY */
    BTA_GATTC_API_READ_EVT,   /* GATTC_OPTYPE_READ */
    BTA_GATTC_API_WRITE_EVT,  /* GATTC_OPTYPE_WRITE */
    BTA_GATTC_API_EXEC_EVT,   /* GATTC_OPTYPE_EXE_WRITE */
    BTA_GATTC_API_CFG_MTU_EVT /* GATTC_OPTYPE_CONFIG */
};

static const char* bta_gattc_op_code_name[] = {
    "Unknown",      /* GATTC_OPTYPE_NONE */
    "Discovery",    /* GATTC_OPTYPE_DISCOVERY */
    "Read",         /* GATTC_OPTYPE_READ */
    "Write",        /* GATTC_OPTYPE_WRITE */
    "Exec",         /* GATTC_OPTYPE_EXE_WRITE */
    "Config",       /* GATTC_OPTYPE_CONFIG */
    "Notification", /* GATTC_OPTYPE_NOTIFICATION */
    "Indication"    /* GATTC_OPTYPE_INDICATION */
};

std::map <uint8_t, RawAddress> dev_addr_map;

/*****************************************************************************
 *  Action Functions
 ****************************************************************************/

void bta_gattc_reset_discover_st(tBTA_GATTC_SERV* p_srcb, tGATT_STATUS status);

/** Enables GATTC module */
static void bta_gattc_enable() {
  VLOG(1) << __func__;
  char native_access_notif_prop[PROPERTY_VALUE_MAX] = "false";

  if (bta_gattc_cb.state == BTA_GATTC_STATE_DISABLED) {
    /* initialize control block */
    bta_gattc_cb = tBTA_GATTC_CB();
    bta_gattc_cb.state = BTA_GATTC_STATE_ENABLED;
    bta_gattc_cb.is_gatt_skt_connected = false;
    bta_gattc_cb.gatt_skt_fd = -1;
    bta_gattc_cb.native_access_uuid_list = {Uuid::FromString(GATT_UUID_ACCEL_GYRO_STR),
        Uuid::FromString(GATT_UUID_MAG_DATA_STR), Uuid::FromString(GATT_UUID_PRESSURE_SENSOR_STR)};

    property_get(
        "persist.vendor.btstack.enable.native_access_notification", native_access_notif_prop, "false");
    if(!strcmp(native_access_notif_prop, "true")) {
      bta_gattc_cb.native_access_notif_enabled = true;
    }
  } else {
    VLOG(1) << "GATTC is already enabled";
  }
}

/** Disable GATTC module by cleaning up all active connections and deregister
 * all application */
void bta_gattc_disable() {
  uint8_t i;

  VLOG(1) << __func__;

  if (bta_gattc_cb.state != BTA_GATTC_STATE_ENABLED) {
    LOG(ERROR) << "not enabled, or disabled in progress";
    return;
  }

  for (i = 0; i < BTA_GATTC_CL_MAX; i++) {
    if (!bta_gattc_cb.cl_rcb[i].in_use) continue;

    bta_gattc_cb.state = BTA_GATTC_STATE_DISABLING;
/* don't deregister HH GATT IF */
/* HH GATT IF will be deregistered by bta_hh_le_deregister when disable HH */
#if (BTA_HH_LE_INCLUDED == TRUE)
    if (!bta_hh_le_is_hh_gatt_if(bta_gattc_cb.cl_rcb[i].client_if)) {
#endif
      bta_gattc_deregister(&bta_gattc_cb.cl_rcb[i]);
#if (BTA_HH_LE_INCLUDED == TRUE)
    }
#endif
  }

  if (bta_gattc_cb.gatt_skt_fd > -1)
    close(bta_gattc_cb.gatt_skt_fd);
  bta_gattc_cb.is_gatt_skt_connected = false;
  bta_gattc_cb.gatt_skt_fd = -1;
  bta_gattc_cb.native_access_uuid_list.clear();
  bta_gattc_cb.native_access_notif_enabled = false;

  /* no registered apps, indicate disable completed */
  if (bta_gattc_cb.state != BTA_GATTC_STATE_DISABLING) {
    bta_gattc_cb = tBTA_GATTC_CB();
    bta_gattc_cb.state = BTA_GATTC_STATE_DISABLED;
  }
}

/** start an application interface */
void bta_gattc_start_if(uint8_t client_if) {
  if (!bta_gattc_cl_get_regcb(client_if)) {
    LOG(ERROR) << "Unable to start app.: Unknown client_if=" << +client_if;
    return;
  }

  GATT_StartIf(client_if);
}

/** Register a GATT client application with BTA */
void bta_gattc_register(const Uuid& app_uuid, tBTA_GATTC_CBACK* p_cback,
                        BtaAppRegisterCallback cb, bool eatt_support) {
  tGATT_STATUS status = GATT_NO_RESOURCES;
  uint8_t client_if = 0;
  VLOG(1) << __func__ << ": state:" << +bta_gattc_cb.state;

  /* check if  GATTC module is already enabled . Else enable */
  if (bta_gattc_cb.state == BTA_GATTC_STATE_DISABLED) {
    bta_gattc_enable();
  }
  /* todo need to check duplicate uuid */
  for (uint8_t i = 0; i < BTA_GATTC_CL_MAX; i++) {
    if (!bta_gattc_cb.cl_rcb[i].in_use) {
      if ((bta_gattc_cb.cl_rcb[i].client_if =
               GATT_Register(app_uuid, &bta_gattc_cl_cback, eatt_support)) == 0) {
        LOG(ERROR) << "Register with GATT stack failed.";
        status = GATT_ERROR;
      } else {
        bta_gattc_cb.cl_rcb[i].in_use = true;
        bta_gattc_cb.cl_rcb[i].p_cback = p_cback;
        bta_gattc_cb.cl_rcb[i].app_uuid = app_uuid;

        /* BTA use the same client interface as BTE GATT statck */
        client_if = bta_gattc_cb.cl_rcb[i].client_if;

        do_in_bta_thread(FROM_HERE,
                          base::Bind(&bta_gattc_start_if, client_if));

        status = GATT_SUCCESS;
        break;
      }
    }
  }

  if (!cb.is_null()) cb.Run(client_if, status);
}

/** De-Register a GATT client application with BTA */
void bta_gattc_deregister(tBTA_GATTC_RCB* p_clreg) {
  if (!p_clreg) {
    LOG(ERROR) << __func__ << ": Deregister Failed unknown client cif";
    return;
  }

  /* remove bg connection associated with this rcb */
  for (uint8_t i = 0; i < BTM_GetWhiteListSize(); i++) {
    if (!bta_gattc_cb.bg_track[i].in_use) continue;

    if (bta_gattc_cb.bg_track[i].cif_mask & ((tBTA_GATTC_CIF_MASK)1 << (p_clreg->client_if - 1))) {
      bta_gattc_mark_bg_conn(p_clreg->client_if,
                             bta_gattc_cb.bg_track[i].remote_bda, false);
      GATT_CancelConnect(p_clreg->client_if,
                         bta_gattc_cb.bg_track[i].remote_bda, false);
    }
  }

  if (p_clreg->num_clcb == 0) {
    bta_gattc_deregister_cmpl(p_clreg);
    return;
  }

  /* close all CLCB related to this app */
  for (uint8_t i = 0; i < BTA_GATTC_CLCB_MAX; i++) {
    if (!bta_gattc_cb.clcb[i].in_use || (bta_gattc_cb.clcb[i].p_rcb != p_clreg))
      continue;

    p_clreg->dereg_pending = true;

    BT_HDR buf;
    buf.event = BTA_GATTC_API_CLOSE_EVT;
    buf.layer_specific = bta_gattc_cb.clcb[i].bta_conn_id;
    bta_gattc_close(&bta_gattc_cb.clcb[i], (tBTA_GATTC_DATA*)&buf);
  }
}

/** process connect API request */
void bta_gattc_process_api_open(tBTA_GATTC_DATA* p_msg) {
  uint16_t event = ((BT_HDR*)p_msg)->event;


  tBTA_GATTC_RCB* p_clreg = bta_gattc_cl_get_regcb(p_msg->api_conn.client_if);
  if (!p_clreg) {
    LOG(ERROR) << __func__
               << ": Failed, unknown client_if=" << +p_msg->api_conn.client_if;
    return;
  }

#ifdef ADV_AUDIO_FEATURE
  RawAddress bd_addr = p_msg->api_conn.remote_bda;
  if (is_remote_support_adv_audio(bd_addr)) {
    tACL_CONN* p_acl_le = btm_bda_to_acl(bd_addr, BT_TRANSPORT_LE);
    if (p_acl_le == NULL) {
      RawAddress map_addr = btif_get_map_address(bd_addr);
      if (map_addr != RawAddress::kEmpty) {
        //Checking whether ACL connection is UP or not?
        LOG(INFO) << __func__ << " Valid Mapaddr " <<map_addr;
        tACL_CONN* p_acl = btm_bda_to_acl(map_addr, BT_TRANSPORT_LE);
        if (p_acl != NULL) {
          tBTA_GATTC_CLCB* p_clcb =
            bta_gattc_cl_get_regcb_by_bdaddr(map_addr, BT_TRANSPORT_LE);
          if (p_clcb != NULL) {
            dev_addr_map[p_msg->api_conn.client_if] = bd_addr;
            p_msg->api_conn.remote_bda = map_addr;
          }
        } else {
          if (!p_msg->api_conn.opportunistic) {
            tBT_DEVICE_TYPE dev_type;
            tBLE_ADDR_TYPE addr_type;

            BTM_ReadDevInfo(bd_addr, &dev_type, &addr_type);
            bool addr_is_rpa = (addr_type == BLE_ADDR_RANDOM && BTM_BLE_IS_RESOLVE_BDA(bd_addr));
            LOG(INFO) << __func__ << " -- addr_is_rpa " << addr_is_rpa;
            bta_dm_update_adv_audio_db(map_addr);
            if (addr_is_rpa) {
              dev_addr_map[p_msg->api_conn.client_if] = bd_addr;
              p_msg->api_conn.remote_bda = map_addr;
            } else if (!is_remote_support_adv_audio(map_addr)) {
              dev_addr_map[p_msg->api_conn.client_if] = bd_addr;
              p_msg->api_conn.remote_bda = map_addr;
            }
          }
        }
      }
    }
  }
#endif /* ADV_AUDIO_FEATURE */

  if (!p_msg->api_conn.is_direct) {
    bta_gattc_init_bk_conn(&p_msg->api_conn, p_clreg);
    return;
  }

  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_alloc_clcb(
      p_msg->api_conn.client_if, p_msg->api_conn.remote_bda,
      p_msg->api_conn.transport);
  if (p_clcb != NULL) {
    bta_gattc_sm_execute(p_clcb, event, p_msg);
  } else {
    LOG(ERROR) << "No resources to open a new connection.";

    bta_gattc_send_open_cback(p_clreg, GATT_NO_RESOURCES,
                              p_msg->api_conn.remote_bda, GATT_INVALID_CONN_ID,
                              p_msg->api_conn.transport, 0);
  }
}

/** process connect API request */
void bta_gattc_process_api_open_cancel(tBTA_GATTC_DATA* p_msg) {
  CHECK(p_msg != nullptr);

  uint16_t event = ((BT_HDR*)p_msg)->event;

  if (!p_msg->api_cancel_conn.is_direct) {
    LOG(INFO) << "Cancel GATT client background connection";
    bta_gattc_cancel_bk_conn(&p_msg->api_cancel_conn);
    return;
  }
  LOG(INFO) << "Cancel GATT client direct connection";

  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_cif(
      p_msg->api_cancel_conn.client_if, p_msg->api_cancel_conn.remote_bda,
      GATT_TRANSPORT_LE);
  if (p_clcb != NULL) {
    bta_gattc_sm_execute(p_clcb, event, p_msg);
    return;
  }

  LOG(ERROR) << "No such connection need to be cancelled";

  tBTA_GATTC_RCB* p_clreg =
      bta_gattc_cl_get_regcb(p_msg->api_cancel_conn.client_if);

  if (p_clreg && p_clreg->p_cback) {
    tBTA_GATTC cb_data;
    cb_data.status = GATT_ERROR;
    (*p_clreg->p_cback)(BTA_GATTC_CANCEL_OPEN_EVT, &cb_data);
  }
}

/** process encryption complete message */
void bta_gattc_process_enc_cmpl(tGATT_IF client_if, const RawAddress& bda) {
  tBTA_GATTC_RCB* p_clreg = bta_gattc_cl_get_regcb(client_if);

  if (!p_clreg || !p_clreg->p_cback) return;

  tBTA_GATTC cb_data;
  memset(&cb_data, 0, sizeof(tBTA_GATTC));

  cb_data.enc_cmpl.client_if = client_if;
  cb_data.enc_cmpl.remote_bda = bda;

  (*p_clreg->p_cback)(BTA_GATTC_ENC_CMPL_CB_EVT, &cb_data);
}

void bta_gattc_cancel_open_error(tBTA_GATTC_CLCB* p_clcb,
                                 UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC cb_data;

  cb_data.status = GATT_ERROR;

  if (p_clcb && p_clcb->p_rcb && p_clcb->p_rcb->p_cback)
    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CANCEL_OPEN_EVT, &cb_data);
}

void bta_gattc_open_error(tBTA_GATTC_CLCB* p_clcb,
                          UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  LOG(ERROR) << "Connection already opened. wrong state";

  bta_gattc_send_open_cback(p_clcb->p_rcb, GATT_ALREADY_OPEN, p_clcb->bda,
                            p_clcb->bta_conn_id, p_clcb->transport, 0);
}

void bta_gattc_open_fail(tBTA_GATTC_CLCB* p_clcb,
                         tBTA_GATTC_DATA* p_data) {
  tGATT_DISCONN_REASON disc_reason = GATT_ERROR;

  if (p_data != NULL) {
    disc_reason = p_data->int_conn.reason;
  }
  LOG(WARNING) << __func__ << ": Cannot establish Connection. conn_id="
               << loghex(p_clcb->bta_conn_id) << ". Return GATT_Status("
               << +disc_reason << ")";
  bta_gattc_send_open_cback(p_clcb->p_rcb, disc_reason, p_clcb->bda,
                            p_clcb->bta_conn_id, p_clcb->transport, 0);
  /* open failure, remove clcb */
  bta_gattc_clcb_dealloc(p_clcb);
}

/** Process API connection function */
void bta_gattc_open(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC_DATA gattc_data;

  /* open/hold a connection */
  if (!GATT_Connect(p_clcb->p_rcb->client_if, p_data->api_conn.remote_bda, true,
                    p_data->api_conn.transport, p_data->api_conn.opportunistic,
                    p_data->api_conn.initiating_phys)) {
    LOG(ERROR) << "Connection open failure";
    p_data->int_conn.reason = GATT_ERROR;
    bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_OPEN_FAIL_EVT, p_data);
    return;
  }

  tBTA_GATTC_RCB* p_clreg = p_clcb->p_rcb;
  /* Re-enable notification registration for closed connection */
  for (int i = 0; i < BTA_GATTC_NOTIF_REG_MAX; i++) {
    if (p_clreg->notif_reg[i].in_use &&
        p_clreg->notif_reg[i].remote_bda == p_clcb->bda &&
        p_clreg->notif_reg[i].app_disconnected) {
      p_clreg->notif_reg[i].app_disconnected = false;
    }
  }

  /* a connected remote device */
  if (GATT_GetConnIdIfConnected(
          p_clcb->p_rcb->client_if, p_data->api_conn.remote_bda,
          &p_clcb->bta_conn_id, p_data->api_conn.transport)) {
    gattc_data.int_conn.hdr.layer_specific = p_clcb->bta_conn_id;

    bool is_eatt_supported =
            GATT_GetEattSupportIfConnected(p_clcb->p_rcb->client_if, p_data->api_conn.remote_bda,
                                           p_data->api_conn.transport);
    if (!is_eatt_supported) {
      bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_CONN_EVT, &gattc_data);
    }
  }
  /* else wait for the callback event */
}

/** Process API Open for a background connection */
void bta_gattc_init_bk_conn(tBTA_GATTC_API_OPEN* p_data,
                            tBTA_GATTC_RCB* p_clreg) {
  if (!bta_gattc_mark_bg_conn(p_data->client_if, p_data->remote_bda, true)) {
    bta_gattc_send_open_cback(p_clreg, GATT_NO_RESOURCES, p_data->remote_bda,
                              GATT_INVALID_CONN_ID, GATT_TRANSPORT_LE, 0);
    return;
  }

  /* always call open to hold a connection */
  if (!GATT_Connect(p_data->client_if, p_data->remote_bda, false,
                    p_data->transport, false)) {
    LOG(ERROR) << __func__
               << " unable to connect to remote bd_addr=" << p_data->remote_bda;
    bta_gattc_send_open_cback(p_clreg, GATT_ERROR, p_data->remote_bda,
                              GATT_INVALID_CONN_ID, GATT_TRANSPORT_LE, 0);
    return;
  }

  uint16_t conn_id;
  /* if is not a connected remote device */
  if (!GATT_GetConnIdIfConnected(p_data->client_if, p_data->remote_bda,
                                 &conn_id, p_data->transport)) {
    return;
  }

  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_alloc_clcb(
      p_data->client_if, p_data->remote_bda, GATT_TRANSPORT_LE);
  if (!p_clcb) {
    LOG(ERROR) << __func__ << " CLCB Resources exhausted";
    bta_gattc_send_open_cback(p_clreg, GATT_NO_RESOURCES, p_data->remote_bda,
                              GATT_INVALID_CONN_ID, GATT_TRANSPORT_LE, 0);
    return;
  }

  tBTA_GATTC_DATA gattc_data;
  gattc_data.hdr.layer_specific = p_clcb->bta_conn_id = conn_id;

  bool is_eatt_supported =
              GATT_GetEattSupportIfConnected(p_data->client_if, p_data->remote_bda,
                                             p_data->transport);
  if (!is_eatt_supported) {
    /* open connection */
    bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_CONN_EVT, &gattc_data);
  }
}

/** Process API Cancel Open for a background connection */
void bta_gattc_cancel_bk_conn(tBTA_GATTC_API_CANCEL_OPEN* p_data) {
  tBTA_GATTC_RCB* p_clreg;
  tBTA_GATTC cb_data;
  cb_data.status = GATT_ERROR;

  /* remove the device from the bg connection mask */
  if (bta_gattc_mark_bg_conn(p_data->client_if, p_data->remote_bda, false)) {
    if (GATT_CancelConnect(p_data->client_if, p_data->remote_bda, false)) {
      cb_data.status = GATT_SUCCESS;
    } else {
      LOG(ERROR) << __func__ << ": failed";
    }
  }
  p_clreg = bta_gattc_cl_get_regcb(p_data->client_if);

  if (p_clreg && p_clreg->p_cback) {
    (*p_clreg->p_cback)(BTA_GATTC_CANCEL_OPEN_EVT, &cb_data);
  }
}

void bta_gattc_cancel_open_ok(tBTA_GATTC_CLCB* p_clcb,
                              UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC cb_data;

  if (p_clcb->p_rcb->p_cback) {
    cb_data.status = GATT_SUCCESS;
    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CANCEL_OPEN_EVT, &cb_data);
  }

  bta_gattc_clcb_dealloc(p_clcb);
}

void bta_gattc_cancel_open(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC cb_data;

  if (GATT_CancelConnect(p_clcb->p_rcb->client_if,
                         p_data->api_cancel_conn.remote_bda, true)) {
    bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_CANCEL_OPEN_OK_EVT, p_data);
  } else {
    if (p_clcb->p_rcb->p_cback) {
      cb_data.status = GATT_ERROR;
      (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CANCEL_OPEN_EVT, &cb_data);
    }
  }
}

/** receive connection callback from stack */
void bta_gattc_conn(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  tGATT_IF gatt_if;
  VLOG(1) << __func__ << ": server cache state=" << +p_clcb->p_srcb->state;

  if (p_data != NULL) {
    VLOG(1) << __func__ << ": conn_id=" << loghex(p_data->hdr.layer_specific);
    p_clcb->bta_conn_id = p_data->int_conn.hdr.layer_specific;

    GATT_GetConnectionInfor(p_data->hdr.layer_specific, &gatt_if, p_clcb->bda,
                            &p_clcb->transport);
  }

  p_clcb->p_srcb->connected = true;

  if (p_clcb->p_srcb->mtu == 0) p_clcb->p_srcb->mtu = GATT_DEF_BLE_MTU_SIZE;

  p_clcb->p_srcb->mtu = GATT_GetMtuSize(p_clcb->bta_conn_id, p_clcb->bda, p_clcb->transport);

  tBTA_GATTC_RCB* p_clreg = p_clcb->p_rcb;
  /* Re-enable notification registration for closed connection */
  for (int i = 0; i < BTA_GATTC_NOTIF_REG_MAX; i++) {
    if (p_clreg->notif_reg[i].in_use &&
        p_clreg->notif_reg[i].remote_bda == p_clcb->bda &&
        p_clreg->notif_reg[i].app_disconnected) {
      p_clreg->notif_reg[i].app_disconnected = false;
    }
  }

  /* start database cache if needed */
  if (p_clcb->p_srcb->gatt_database.IsEmpty() ||
      p_clcb->p_srcb->state != BTA_GATTC_SERV_IDLE) {
    if (p_clcb->p_srcb->state == BTA_GATTC_SERV_IDLE) {
      p_clcb->p_srcb->state = BTA_GATTC_SERV_LOAD;

      // Consider the case that if GATT Server is changed, but no service
      // changed indication is received, the database might be out of date. So
      // if robust caching is known to be supported, always check the db hash
      // first, before loading the stored database.

      // Only load the database if we are bonded, since the device cache is
      // meaningless otherwise (as we need to do rediscovery regardless)
      gatt::Database db = btm_sec_is_a_bonded_dev(p_clcb->bda)
                              ? bta_gattc_cache_load(p_clcb->p_srcb->server_bda)
                              : gatt::Database();
      auto robust_caching_support = GetRobustCachingSupport(p_clcb, db);
      LOG_INFO(LOG_TAG, "Connected to %s, robust caching support is %d",
               p_clcb->bda.ToRedactedStringForLogging().c_str(),
               robust_caching_support);
      if (db.IsEmpty() ||
          robust_caching_support == RobustCachingSupport::SUPPORTED) {
        // If the peer device is expected to support robust caching, or if we
        // don't know its services yet, then we should do discovery (which may
        // short-circuit through a hash match, but might also do the full
        // discovery).
        p_clcb->p_srcb->state = BTA_GATTC_SERV_DISC;

        /* set true to read database hash before service discovery */
        if (bta_gattc_is_robust_caching_enabled()) {
          p_clcb->p_srcb->srvc_hdl_db_hash = true;
        }

        /* cache load failure, start discovery */
        bta_gattc_start_discover(p_clcb, NULL);
      } else {
        p_clcb->p_srcb->gatt_database = db;
        p_clcb->p_srcb->state = BTA_GATTC_SERV_IDLE;
        bta_gattc_reset_discover_st(p_clcb->p_srcb, GATT_SUCCESS);
      }
    } else /* cache is building */
      p_clcb->state = BTA_GATTC_DISCOVER_ST;
  }

  else {
    /* a pending service handle change indication */
    if (p_clcb->p_srcb->srvc_hdl_chg) {
      p_clcb->p_srcb->srvc_hdl_chg = false;

      /* set true to read database hash before service discovery */
      if (bta_gattc_is_robust_caching_enabled()) {
        p_clcb->p_srcb->srvc_hdl_db_hash = true;
      }

      /* start discovery */
      bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_DISCOVER_EVT, NULL);
    }
  }

  if (p_clcb->p_rcb) {
    /* there is no RM for GATT */
    if (p_clcb->transport == BTA_TRANSPORT_BR_EDR)
      bta_sys_conn_open(BTA_ID_GATTC, BTA_ALL_APP_ID, p_clcb->bda);
#ifdef ADV_AUDIO_FEATURE
    if (is_remote_support_adv_audio(p_clcb->bda)) {
      auto itr = dev_addr_map.find(p_clcb->p_rcb->client_if);
      if (itr != dev_addr_map.end()) {
        bta_gattc_send_open_cback(p_clcb->p_rcb, GATT_SUCCESS, itr->second,
            p_clcb->bta_conn_id, p_clcb->transport,
            p_clcb->p_srcb->mtu);
        return;
      }
    }
#endif
    bta_gattc_send_open_cback(p_clcb->p_rcb, GATT_SUCCESS, p_clcb->bda,
        p_clcb->bta_conn_id, p_clcb->transport,
        p_clcb->p_srcb->mtu);

  }
}

/** close a  connection */
void bta_gattc_close_fail(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC cb_data;

  if (p_clcb->p_rcb->p_cback) {
    memset(&cb_data, 0, sizeof(tBTA_GATTC));
    cb_data.close.client_if = p_clcb->p_rcb->client_if;
    cb_data.close.conn_id = p_data->hdr.layer_specific;
    cb_data.close.remote_bda = p_clcb->bda;
    cb_data.close.status = GATT_ERROR;
    cb_data.close.reason = BTA_GATT_CONN_NONE;

    LOG(WARNING) << __func__ << ": conn_id=" << loghex(cb_data.close.conn_id)
                 << ". Returns GATT_ERROR(" << +GATT_ERROR << ").";

    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CLOSE_EVT, &cb_data);
  }
}

/** close a GATTC connection */
void bta_gattc_close(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC_CBACK* p_cback = p_clcb->p_rcb->p_cback;
  tBTA_GATTC_RCB* p_clreg = p_clcb->p_rcb;
  tBTA_GATTC cb_data;

  cb_data.close.client_if = p_clcb->p_rcb->client_if;
  cb_data.close.conn_id = p_clcb->bta_conn_id;
  cb_data.close.reason = p_clcb->reason;
  cb_data.close.remote_bda = p_clcb->bda;
  cb_data.close.status = GATT_SUCCESS;

#ifdef ADV_AUDIO_FEATURE
  if (is_remote_support_adv_audio(p_clcb->bda)) {
    auto itr = dev_addr_map.find(p_clcb->p_rcb->client_if);
    if (itr != dev_addr_map.end()) {
      cb_data.close.remote_bda = itr->second;
      dev_addr_map.erase(p_clcb->p_rcb->client_if);
    }
  }
#endif
  if (p_clcb->transport == BTA_TRANSPORT_BR_EDR)
    bta_sys_conn_close(BTA_ID_GATTC, BTA_ALL_APP_ID, p_clcb->bda);

  /* Disable notification registration for closed connection */
  for (int i = 0; i < BTA_GATTC_NOTIF_REG_MAX; i++) {
    if (p_clreg->notif_reg[i].in_use &&
        p_clreg->notif_reg[i].remote_bda == p_clcb->bda) {
      p_clreg->notif_reg[i].app_disconnected = true;
    }
  }

  if (p_data->hdr.event == BTA_GATTC_INT_DISCONN_EVT) {
    /* Since link has been disconnected by and it is possible that here are
     * already some new p_clcb created for the background connect, the number of
     * p_srcb->num_clcb is NOT 0. This will prevent p_srcb to be cleared inside
     * the bta_gattc_clcb_dealloc.
     *
     * In this point of time, we know that link does not exist, so let's make
     * sure the connection state, mtu and database is cleared.
     */
    bta_gattc_server_disconnected(p_clcb->p_srcb);
  }

  bta_gattc_clcb_dealloc(p_clcb);

  if (p_data->hdr.event == BTA_GATTC_API_CLOSE_EVT) {
    cb_data.close.status = GATT_Disconnect(p_data->hdr.layer_specific);
    LOG(INFO) << __func__
              << "Local close event client_if: "
              << loghex(cb_data.close.client_if)
              << ", conn_id: " << cb_data.close.conn_id
              << ", reason: " << cb_data.close.reason;
  } else if (p_data->hdr.event == BTA_GATTC_INT_DISCONN_EVT) {
    cb_data.close.status = static_cast<tGATT_STATUS>(p_data->int_conn.reason);
    cb_data.close.reason = p_data->int_conn.reason;
    LOG(INFO) << __func__
              << "Peer close disconnect event client_if: "
              << loghex(cb_data.close.client_if)
              << ", conn_id: " << cb_data.close.conn_id
              << ", reason: " << cb_data.close.reason;
  }

  if (p_cback) (*p_cback)(BTA_GATTC_CLOSE_EVT, &cb_data);

  if (p_clreg->num_clcb == 0 && p_clreg->dereg_pending) {
    bta_gattc_deregister_cmpl(p_clreg);
  }
  bta_gattc_clear_notif_reg_on_disc(p_clreg, p_clcb->bda);
}

/** when a SRCB finished discovery, tell all related clcb */
void bta_gattc_reset_discover_st(tBTA_GATTC_SERV* p_srcb, tGATT_STATUS status) {
  for (uint8_t i = 0; i < BTA_GATTC_CLCB_MAX; i++) {
    if (bta_gattc_cb.clcb[i].p_srcb == p_srcb) {
      bta_gattc_cb.clcb[i].status = status;
      bta_gattc_sm_execute(&bta_gattc_cb.clcb[i], BTA_GATTC_DISCOVER_CMPL_EVT,
                           NULL);
    }
  }
}

/** close a GATTC connection while in discovery state */
void bta_gattc_disc_close(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  VLOG(1) << __func__
          << ": Discovery cancel conn_id=" << loghex(p_clcb->bta_conn_id);

  if (p_clcb->disc_active)
    bta_gattc_reset_discover_st(p_clcb->p_srcb, GATT_ERROR);
  else
    p_clcb->state = BTA_GATTC_CONN_ST;

  // This function only gets called as the result of a BTA_GATTC_API_CLOSE_EVT
  // while in the BTA_GATTC_DISCOVER_ST state. Once the state changes, the
  // connection itself still needs to be closed to resolve the original event.
  if (p_clcb->state == BTA_GATTC_CONN_ST) {
    VLOG(1) << "State is back to BTA_GATTC_CONN_ST. Trigger connection close";
    bta_gattc_close(p_clcb, p_data);
  }
}

/** when a SRCB start discovery, tell all related clcb and set the state */
void bta_gattc_set_discover_st(tBTA_GATTC_SERV* p_srcb) {
  uint8_t i;

  if (!interop_match_addr_or_name(INTEROP_DISABLE_LE_CONN_UPDATES, &p_srcb->server_bda)) {
    L2CA_EnableUpdateBleConnParams(p_srcb->server_bda, false);
  }

  for (i = 0; i < BTA_GATTC_CLCB_MAX; i++) {
    if (bta_gattc_cb.clcb[i].p_srcb == p_srcb) {
      bta_gattc_cb.clcb[i].status = GATT_SUCCESS;
      if (p_srcb->srvc_hdl_db_hash &&
          (bta_gattc_cb.clcb[i].state == BTA_GATTC_W4_CONN_ST)) {
        bta_gattc_cb.clcb[i].state = BTA_GATTC_DISCOVER_ST_RC;
      }
      else {
        bta_gattc_cb.clcb[i].state = BTA_GATTC_DISCOVER_ST;
      }
      bta_gattc_cb.clcb[i].request_during_discovery =
          BTA_GATTC_DISCOVER_REQ_NONE;
    }
  }
}

/** process service change in discovery state, mark up the auto update flag and
 * set status to be discovery cancel for current discovery.
 */
void bta_gattc_restart_discover(tBTA_GATTC_CLCB* p_clcb,
                                UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  p_clcb->status = GATT_CANCEL;
  p_clcb->auto_update = BTA_GATTC_DISC_WAITING;
}

/** Configure MTU size on the GATT connection */
void bta_gattc_cfg_mtu(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  uint16_t current_mtu = 0;
  auto result = GATTC_TryMtuRequest(p_clcb->bda, p_clcb->transport,
                                    p_clcb->bta_conn_id, &current_mtu);
  switch (result) {
    case MTU_EXCHANGE_DEVICE_DISCONNECTED:
      VLOG(1) << __func__ << " Device " << p_clcb->bda << " disconnected";
      bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_CONFIG,
                             GATT_NO_RESOURCES, NULL);
      bta_gattc_continue(p_clcb);
      return;
    case MTU_EXCHANGE_NOT_ALLOWED:
      VLOG(1) << __func__ << " Not allowed for BR/EDR devices " << p_clcb->bda;
      bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_CONFIG,
                             GATT_ERR_UNLIKELY, NULL);
      bta_gattc_continue(p_clcb);
      return;
    case MTU_EXCHANGE_ALREADY_DONE:
      /* Check if MTU is not already set, if so, just report it back to the user
       * and continue with other requests.
       */
      GATTC_UpdateUserAttMtuIfNeeded(p_clcb->bda, p_clcb->transport,
                                     p_data->api_mtu.mtu);
      bta_gattc_send_mtu_response(p_clcb, p_data, current_mtu);
      return;
    case MTU_EXCHANGE_IN_PROGRESS:
      VLOG(1) << __func__ << " Enqueue MTU Request  - waiting for response on p_clcb: " << p_clcb;
      bta_gattc_enqueue(p_clcb, p_data);
      return;

    case MTU_EXCHANGE_NOT_DONE_YET:
      /* OK to proceed */
      break;
  }

  if (bta_gattc_enqueue(p_clcb, p_data) == ENQUEUED_FOR_LATER) return;

  tGATT_STATUS status =
      GATTC_ConfigureMTU(p_clcb->bta_conn_id, p_data->api_mtu.mtu);

  /* if failed, return callback here */
  if (status != GATT_SUCCESS && status != GATT_CMD_STARTED) {
    /* Dequeue the data, if it was enqueued */
    if (p_clcb->p_q_cmd == p_data) {
      if (status == GATT_BUSY) return;
      else {
        p_clcb->p_q_cmd = NULL;
      }
    }
    bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_CONFIG, status,
        NULL);
    bta_gattc_continue(p_clcb);
  }
}

void bta_gattc_start_discover_internal(tBTA_GATTC_CLCB* p_clcb) {
  if (p_clcb->transport == BT_TRANSPORT_LE) {
    if (!interop_match_addr_or_name(INTEROP_DISABLE_LE_CONN_UPDATES, &p_clcb->p_srcb->server_bda)) {
      L2CA_EnableUpdateBleConnParams(p_clcb->p_srcb->server_bda, false);
    }
  }

  bta_gattc_init_cache(p_clcb->p_srcb);
  p_clcb->status = bta_gattc_discover_pri_service(
      p_clcb->bta_conn_id, p_clcb->p_srcb, GATT_DISC_SRVC_ALL);
  if (p_clcb->status != GATT_SUCCESS) {
    LOG(ERROR) << "discovery on server failed";
    bta_gattc_reset_discover_st(p_clcb->p_srcb, p_clcb->status);
  } else
    p_clcb->disc_active = true;
}

/** Start a discovery on server */
void bta_gattc_start_discover(tBTA_GATTC_CLCB* p_clcb,
                              UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  VLOG(1) << __func__ << ": conn_id:" << loghex(p_clcb->bta_conn_id)
          << " p_clcb->p_srcb->state:" << +p_clcb->p_srcb->state;

  if (((p_clcb->p_q_cmd == NULL ||
        p_clcb->auto_update == BTA_GATTC_REQ_WAITING) &&
       p_clcb->p_srcb->state == BTA_GATTC_SERV_IDLE) ||
      p_clcb->p_srcb->state == BTA_GATTC_SERV_DISC)
  /* no pending operation, start discovery right away */
  {
    p_clcb->auto_update = BTA_GATTC_NO_SCHEDULE;

    if (p_clcb->p_srcb != NULL) {
      /* set all srcb related clcb into discovery ST */
      bta_gattc_set_discover_st(p_clcb->p_srcb);

      // Before clear mask, set is_svc_chg to
      // 1. true, invoked by service changed indication
      // 2. false, invoked by connect API
      bool is_svc_chg = p_clcb->p_srcb->srvc_hdl_chg;

      /* clear the service change mask */
      p_clcb->p_srcb->srvc_hdl_chg = false;
      p_clcb->p_srcb->update_count = 0;
      p_clcb->p_srcb->state = BTA_GATTC_SERV_DISC_ACT;

      if (GetRobustCachingSupport(p_clcb, p_clcb->p_srcb->gatt_database) ==
          RobustCachingSupport::UNSUPPORTED) {
        // Skip initial DB hash read if we have strong reason (due to interop,
        // or a prior discovery) to believe that it is unsupported.
        p_clcb->p_srcb->srvc_hdl_db_hash = false;
      }

      /* read db hash if db hash characteristic exists */
      if (bta_gattc_is_robust_caching_enabled() &&
          p_clcb->p_srcb->srvc_hdl_db_hash &&
          bta_gattc_read_db_hash(p_clcb, is_svc_chg)) {
        LOG(INFO) << __func__
                  << ": pending service discovery, read db hash first";
        p_clcb->p_srcb->srvc_hdl_db_hash = false;
        return;
      }

      bta_gattc_start_discover_internal(p_clcb);
    } else {
      LOG(ERROR) << "unknown device, can not start discovery";
    }
  }
  /* pending operation, wait until it finishes */
  else {
    p_clcb->auto_update = BTA_GATTC_DISC_WAITING;

    if (p_clcb->p_srcb->state == BTA_GATTC_SERV_IDLE)
      p_clcb->state = BTA_GATTC_CONN_ST; /* set clcb state */
  }
}

/** discovery on server is finished */
void bta_gattc_disc_cmpl(tBTA_GATTC_CLCB* p_clcb,
                         UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  tBTA_GATTC_DATA* p_q_cmd = p_clcb->p_q_cmd;

  VLOG(1) << __func__ << ": conn_id=" << loghex(p_clcb->bta_conn_id)
                      << ", status = " << +p_clcb->status;

  if (p_clcb->transport == BTA_TRANSPORT_LE) {
    if (p_clcb->p_srcb &&
      (!interop_match_addr_or_name(INTEROP_DISABLE_LE_CONN_UPDATES,
          &p_clcb->p_srcb->server_bda))) {
      L2CA_EnableUpdateBleConnParams(p_clcb->p_srcb->server_bda, true);
    }
  }

  if (p_clcb->p_srcb) {
    p_clcb->p_srcb->state = BTA_GATTC_SERV_IDLE;
  }
  p_clcb->disc_active = false;

  if (p_clcb->status != GATT_SUCCESS) {
    /* clean up cache */
    if (p_clcb->p_srcb) {
      p_clcb->p_srcb->gatt_database.Clear();
      /* used to reset cache in application */
      bta_gattc_cache_reset(p_clcb->p_srcb->server_bda);
    }
    uint16_t p_conn_id;
    if (!GATT_GetConnIdIfConnected((uint8_t)p_clcb->bta_conn_id, p_clcb->bda,
                                  &p_conn_id, p_clcb->transport)) {
      osi_free_and_reset((void**)&p_q_cmd);
      p_clcb->p_q_cmd = NULL;
    }
  }

  if (p_clcb->p_srcb) {
    p_clcb->p_srcb->pending_discovery.Clear();
  }

  if (p_clcb->auto_update == BTA_GATTC_DISC_WAITING) {
    /* start discovery again */
    p_clcb->auto_update = BTA_GATTC_REQ_WAITING;
    bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_DISCOVER_EVT, NULL);
  }
  /* get any queued command to proceed */
  else if (p_q_cmd != NULL) {
    p_clcb->p_q_cmd = NULL;
    /* execute pending operation of link block still present */
    if (p_clcb->p_srcb &&
        l2cu_find_lcb_by_bd_addr(p_clcb->p_srcb->server_bda,
          p_clcb->transport)) {
      bta_gattc_sm_execute(p_clcb, p_q_cmd->hdr.event, p_q_cmd);
    }
    /* if the command executed requeued the cmd, we don't
     * want to free the underlying buffer that's being
     * referenced by p_clcb->p_q_cmd
     */
    if (p_q_cmd != p_clcb->p_q_cmd) osi_free_and_reset((void**)&p_q_cmd);
  } else {
    bta_gattc_continue(p_clcb);
  }

  if (p_clcb->p_rcb && p_clcb->p_rcb->p_cback && p_clcb->p_srcb) {
    tBTA_GATTC bta_gattc;
    bta_gattc.remote_bda = p_clcb->p_srcb->server_bda;
    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_SRVC_DISC_DONE_EVT, &bta_gattc);
  }

  if (btm_cb.enc_adv_data_enabled) {
    if ((p_clcb->status == GATT_SUCCESS) && p_clcb->p_srcb
        && btm_sec_is_a_bonded_dev(p_clcb->p_srcb->server_bda)) {
      tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(p_clcb->p_srcb->server_bda);
      tGATT_TCB* p_tcb = gatt_find_tcb_by_addr(p_clcb->p_srcb->server_bda, BT_TRANSPORT_LE);
      if (p_dev_rec && (p_dev_rec->sec_flags & BTM_SEC_LE_ENCRYPTED)) {
        VLOG(1) << __func__ << ": Encryption is done, read enc key values";
        GAP_BleGetEncKeyMaterialInfo(p_clcb->p_srcb->server_bda, BT_TRANSPORT_LE);
      } else if (p_tcb){
        p_tcb->is_read_enc_key_pending = true;
        VLOG(1) << __func__ << ": Encryption is not done, dont read enc key values";
      }
    }
  }
}

/** Read an attribute */
void bta_gattc_read(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  if (bta_gattc_enqueue(p_clcb, p_data) == ENQUEUED_FOR_LATER) return;

  tGATT_STATUS status;
  if (p_data->api_read.handle != 0) {
    tGATT_READ_PARAM read_param;
    memset(&read_param, 0, sizeof(tGATT_READ_PARAM));
    read_param.by_handle.handle = p_data->api_read.handle;
    read_param.by_handle.auth_req = p_data->api_read.auth_req;
    status = GATTC_Read(p_clcb->bta_conn_id, GATT_READ_BY_HANDLE, &read_param);
  } else {
    tGATT_READ_PARAM read_param;
    memset(&read_param, 0, sizeof(tGATT_READ_BY_TYPE));

    read_param.char_type.s_handle = p_data->api_read.s_handle;
    read_param.char_type.e_handle = p_data->api_read.e_handle;
    read_param.char_type.uuid = p_data->api_read.uuid;
    read_param.char_type.auth_req = p_data->api_read.auth_req;
    status = GATTC_Read(p_clcb->bta_conn_id, GATT_READ_BY_TYPE, &read_param);
  }

  /* read fail */
  if (status != GATT_SUCCESS) {
    /* Dequeue the data, if it was enqueued */
    if (p_clcb->p_q_cmd == p_data) p_clcb->p_q_cmd = NULL;

    bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_READ, status,
                           NULL);
    bta_gattc_continue(p_clcb);
  }
}

/** read multiple */
void bta_gattc_read_multi(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  if (bta_gattc_enqueue(p_clcb, p_data) == ENQUEUED_FOR_LATER) return;

  tGATT_READ_PARAM read_param;
  memset(&read_param, 0, sizeof(tGATT_READ_PARAM));
  tGATT_READ_TYPE type;

  read_param.read_multiple.num_handles = p_data->api_read_multi.num_attr;
  read_param.read_multiple.auth_req = p_data->api_read_multi.auth_req;
  read_param.read_multiple.is_variable_len= p_data->api_read_multi.is_variable_len;
  memcpy(&read_param.read_multiple.handles, p_data->api_read_multi.handles,
         sizeof(uint16_t) * p_data->api_read_multi.num_attr);

  if (p_data->api_read_multi.is_variable_len) {
    type = GATT_READ_MULTIPLE_VARIABLE;
  } else {
    type = GATT_READ_MULTIPLE;
  }

  tGATT_STATUS status =
      GATTC_Read(p_clcb->bta_conn_id, type, &read_param);
  /* read fail */
  if (status != GATT_SUCCESS) {
    /* Dequeue the data, if it was enqueued */
    if (p_clcb->p_q_cmd == p_data) p_clcb->p_q_cmd = NULL;

    bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_READ, status,
                           NULL);
    bta_gattc_continue(p_clcb);
  }
}

/** Write an attribute */
void bta_gattc_write(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  if (bta_gattc_enqueue(p_clcb, p_data) == ENQUEUED_FOR_LATER) return;

  tGATT_STATUS status = GATT_SUCCESS;
  tGATT_VALUE attr;

  attr.conn_id = p_clcb->bta_conn_id;
  attr.handle = p_data->api_write.handle;
  attr.offset = p_data->api_write.offset;
  attr.len = p_data->api_write.len;
  attr.auth_req = p_data->api_write.auth_req;

  /* Before coping to the fixed array, make sure it fits. */
  if (attr.len > GATT_MAX_ATTR_LEN) {
    status = GATT_INVALID_ATTR_LEN;
  } else {
    if (p_data->api_write.p_value)
      memcpy(attr.value, p_data->api_write.p_value, p_data->api_write.len);

    status =
        GATTC_Write(p_clcb->bta_conn_id, p_data->api_write.write_type, &attr);
  }

  /* write fail */
  if (status != GATT_SUCCESS) {
    /* Dequeue the data, if it was enqueued */
    if (p_clcb->p_q_cmd == p_data) p_clcb->p_q_cmd = NULL;

    bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_WRITE, status,
                           NULL);
    bta_gattc_continue(p_clcb);
  }
}

/** send execute write */
void bta_gattc_execute(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  if (bta_gattc_enqueue(p_clcb, p_data) == ENQUEUED_FOR_LATER) return;

  tGATT_STATUS status =
      GATTC_ExecuteWrite(p_clcb->bta_conn_id, p_data->api_exec.is_execute);
  if (status != GATT_SUCCESS) {
    /* Dequeue the data, if it was enqueued */
    if (p_clcb->p_q_cmd == p_data) p_clcb->p_q_cmd = NULL;

    bta_gattc_cmpl_sendmsg(p_clcb->bta_conn_id, GATTC_OPTYPE_EXE_WRITE, status,
                           NULL);
    bta_gattc_continue(p_clcb);
  }
}

/** send handle value confirmation */
void bta_gattc_confirm(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  uint16_t handle = p_data->api_confirm.handle;
  uint32_t trans_id = p_data->api_confirm.trans_id;

  if (GATTC_SendHandleValueConfirm(p_data->api_confirm.hdr.layer_specific,
                                   handle, trans_id) != GATT_SUCCESS) {
    LOG(ERROR) << __func__ << ": to handle=" << loghex(handle) << " failed";
  } else {
    /* if over BR_EDR, inform PM for mode change */
    if (p_clcb->transport == BTA_TRANSPORT_BR_EDR) {
      bta_sys_busy(BTA_ID_GATTC, BTA_ALL_APP_ID, p_clcb->bda);
      bta_sys_idle(BTA_ID_GATTC, BTA_ALL_APP_ID, p_clcb->bda);
    }
  }
}

/** read multi complete */
void bta_gattc_read_multi_cmpl(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_OP_CMPL* p_data) {
  VLOG(1) << __func__;
  bool is_variable_len = false;

  GATT_READ_MULTI_OP_CB cb = p_clcb->p_q_cmd->api_read_multi.read_multi_cb;

  osi_free_and_reset((void**)&p_clcb->p_q_cmd);

  VLOG(1) << __func__ << " read_sub_type:" << + p_data->p_cmpl->att_value.read_sub_type;
  if(p_data->p_cmpl->att_value.read_sub_type == GATT_READ_MULTIPLE_VARIABLE) {
    is_variable_len = true;
  }

  if (cb) {
    cb(p_clcb->bta_conn_id, p_data->status, is_variable_len,
       p_data->p_cmpl->att_value.len, p_data->p_cmpl->att_value.value);
  }
}

/** read complete */
void bta_gattc_read_cmpl(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_OP_CMPL* p_data) {

  if ((p_data->p_cmpl->att_value.read_sub_type == GATT_READ_MULTIPLE) ||
      (p_data->p_cmpl->att_value.read_sub_type == GATT_READ_MULTIPLE_VARIABLE)) {
    VLOG(1) << __func__ << " Read multi cmpl";
    bta_gattc_read_multi_cmpl(p_clcb, p_data);
    return;
  }

  GATT_READ_OP_CB cb = p_clcb->p_q_cmd->api_read.read_cb;
  void* my_cb_data = p_clcb->p_q_cmd->api_read.read_cb_data;

  /* if it was read by handle, return the handle requested, if read by UUID, use
   * handle returned from remote
   */
  uint16_t handle = p_clcb->p_q_cmd->api_read.handle;
  if (handle == 0) handle = p_data->p_cmpl->att_value.handle;

  osi_free_and_reset((void**)&p_clcb->p_q_cmd);

  if (cb) {
    cb(p_clcb->bta_conn_id, p_data->status, handle,
       p_data->p_cmpl->att_value.len, p_data->p_cmpl->att_value.value,
       my_cb_data);
  }
}

/** write complete */
static void bta_gattc_write_cmpl(tBTA_GATTC_CLCB* p_clcb,
                                 const tBTA_GATTC_OP_CMPL* p_data) {
  GATT_WRITE_OP_CB cb = p_clcb->p_q_cmd->api_write.write_cb;
  void* my_cb_data = p_clcb->p_q_cmd->api_write.write_cb_data;

  if (cb) {
    if (p_data->status == 0 &&
        p_clcb->p_q_cmd->api_write.write_type == BTA_GATTC_WRITE_PREPARE) {
      LOG_DEBUG(LOG_TAG, "Handling prepare write success response: handle 0x%04x",
                p_data->p_cmpl->att_value.handle);
      /* If this is successful Prepare write, lets provide to the callback the
       * data provided by server */
      cb(p_clcb->bta_conn_id, p_data->status, p_data->p_cmpl->att_value.handle,
         p_data->p_cmpl->att_value.len, p_data->p_cmpl->att_value.value,
         my_cb_data);
    } else {
      LOG_DEBUG(LOG_TAG, "Handling write response type: %d: handle 0x%04x",
                p_clcb->p_q_cmd->api_write.write_type,
                p_data->p_cmpl->att_value.handle);
      /* Otherwise, provide data which were intended to write. */
      cb(p_clcb->bta_conn_id, p_data->status, p_data->p_cmpl->att_value.handle,
         p_clcb->p_q_cmd->api_write.len, p_clcb->p_q_cmd->api_write.p_value,
         my_cb_data);
    }
  }

  osi_free_and_reset((void**)&p_clcb->p_q_cmd);
}

/** execute write complete */
void bta_gattc_exec_cmpl(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_OP_CMPL* p_data) {
  tBTA_GATTC cb_data;

  osi_free_and_reset((void**)&p_clcb->p_q_cmd);
  p_clcb->status = GATT_SUCCESS;

  /* execute complete, callback */
  cb_data.exec_cmpl.conn_id = p_clcb->bta_conn_id;
  cb_data.exec_cmpl.status = p_data->status;

  (*p_clcb->p_rcb->p_cback)(BTA_GATTC_EXEC_EVT, &cb_data);
}

/** configure MTU operation complete */
void bta_gattc_cfg_mtu_cmpl(tBTA_GATTC_CLCB* p_clcb,
                            tBTA_GATTC_OP_CMPL* p_data) {
  GATT_CONFIGURE_MTU_OP_CB cb = p_clcb->p_q_cmd->api_mtu.mtu_cb;
  void* my_cb_data = p_clcb->p_q_cmd->api_mtu.mtu_cb_data;
  tBTA_GATTC cb_data;

  osi_free_and_reset((void**)&p_clcb->p_q_cmd);

  if (p_data->p_cmpl && p_data->status == GATT_SUCCESS)
    p_clcb->p_srcb->mtu = p_data->p_cmpl->mtu;

  /* configure MTU complete, callback */
  p_clcb->status = p_data->status;
  cb_data.cfg_mtu.conn_id = p_clcb->bta_conn_id;
  cb_data.cfg_mtu.status = p_data->status;
  cb_data.cfg_mtu.mtu = p_clcb->p_srcb->mtu;

  if (cb) {
    cb(p_clcb->bta_conn_id, p_data->status, my_cb_data);
  }

  (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CFG_MTU_EVT, &cb_data);
}

/** operation completed */
void bta_gattc_op_cmpl(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  uint8_t op = (uint8_t)p_data->op_cmpl.op_code;
  uint8_t mapped_op = 0;
  bool is_eatt_supported = false;

  VLOG(1) << __func__ << ": op:" << +op;

  if (op == GATTC_OPTYPE_INDICATION || op == GATTC_OPTYPE_NOTIFICATION) {
    LOG(ERROR) << "unexpected operation, ignored";
    return;
  }

  if (op < GATTC_OPTYPE_READ) return;

  if (!p_clcb) {
    LOG(ERROR) << "p_clcb is NULL";
    return;
  }

  is_eatt_supported =
      GATT_GetEattSupportIfConnected(p_clcb->p_rcb->client_if, p_clcb->bda,
                                     p_clcb->transport);

  VLOG(1) << __func__ << " is_eatt_supported:" << +is_eatt_supported;

  if (p_clcb->p_q_cmd == NULL) {
    if (!(is_eatt_supported && (op == GATTC_OPTYPE_CONFIG))) {
      LOG(ERROR) << "No pending command";
      return;
    }
  }

  if (p_clcb->p_q_cmd && (p_clcb->p_q_cmd->hdr.event !=
      bta_gattc_opcode_to_int_evt[op - GATTC_OPTYPE_READ]) &&
     (p_clcb->p_q_cmd->hdr.event != BTA_GATTC_API_READ_MULTI_EVT) &&
     !(is_eatt_supported && (op == GATTC_OPTYPE_CONFIG))) {
    mapped_op =
        p_clcb->p_q_cmd->hdr.event - BTA_GATTC_API_READ_EVT + GATTC_OPTYPE_READ;
    if (mapped_op > GATTC_OPTYPE_INDICATION) mapped_op = 0;
    VLOG(1) << __func__ << ": mapped_op:" << +mapped_op;

    LOG(ERROR) << StringPrintf(
        "expect op:(%s :0x%04x), receive unexpected operation (%s).",
        bta_gattc_op_code_name[mapped_op], p_clcb->p_q_cmd->hdr.event,
        bta_gattc_op_code_name[op]);
    return;
  }

  /* Except for MTU configuration, discard responses if service change
   * indication is received before operation completed
   */
  if (p_clcb->auto_update == BTA_GATTC_DISC_WAITING &&
      p_clcb->p_srcb->srvc_hdl_chg && op != GATTC_OPTYPE_CONFIG) {
    VLOG(1) << "Discard all responses when service change indication is "
               "received.";
    p_data->op_cmpl.status = GATT_ERROR;
  }

  /* service handle change void the response, discard it */
  if (op == GATTC_OPTYPE_READ) {
    bta_gattc_read_cmpl(p_clcb, &p_data->op_cmpl);
  } else if (op == GATTC_OPTYPE_WRITE) {
    bta_gattc_write_cmpl(p_clcb, &p_data->op_cmpl);
  } else if (op == GATTC_OPTYPE_EXE_WRITE) {
    bta_gattc_exec_cmpl(p_clcb, &p_data->op_cmpl);
  } else if (op == GATTC_OPTYPE_CONFIG) {
    bta_gattc_cfg_mtu_cmpl(p_clcb, &p_data->op_cmpl);

    /* If there are more clients waiting for the MTU results on the same device,
     * lets trigger them now.
     */

    auto outstanding_conn_ids =
        GATTC_GetAndRemoveListOfConnIdsWaitingForMtuRequest(p_clcb->bda);
    for (auto conn_id : outstanding_conn_ids) {
      tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
      VLOG(1) << __func__ << " Continue MTU request clcb " << p_clcb;
      if (p_clcb) {
        VLOG(1) << __func__ << "Continue MTU request for client conn_id:"
                << +conn_id;
        bta_gattc_continue(p_clcb);
      }
    }
  }

  // If receive DATABASE_OUT_OF_SYNC error code, bta_gattc should start service
  // discovery immediately
  if (bta_gattc_is_robust_caching_enabled() &&
      p_data->op_cmpl.status == GATT_DATABASE_OUT_OF_SYNC) {
    LOG(INFO) << __func__ << ": DATABASE_OUT_OF_SYNC, re-discover service";
    p_clcb->auto_update = BTA_GATTC_REQ_WAITING;
    /* request read db hash first */
    p_clcb->p_srcb->srvc_hdl_db_hash = true;
    bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_DISCOVER_EVT, NULL);
    return;
  }

  if (p_clcb->auto_update == BTA_GATTC_DISC_WAITING) {
    p_clcb->auto_update = BTA_GATTC_REQ_WAITING;

    /* request read db hash first */
    if (bta_gattc_is_robust_caching_enabled()) {
      p_clcb->p_srcb->srvc_hdl_db_hash = true;
    }

    bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_DISCOVER_EVT, NULL);
    return;
  }
  bta_gattc_continue(p_clcb);
}

/** start a search in the local server cache */
void bta_gattc_search(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  tGATT_STATUS status = GATT_INTERNAL_ERROR;
  tBTA_GATTC cb_data;
  VLOG(1) << __func__ << ": conn_id=" << loghex(p_clcb->bta_conn_id);
  if (p_clcb->p_srcb && !p_clcb->p_srcb->gatt_database.IsEmpty()) {
    status = GATT_SUCCESS;
    /* search the local cache of a server device */
    bta_gattc_search_service(p_clcb, p_data->api_search.p_srvc_uuid);
  }
  cb_data.search_cmpl.status = status;
  cb_data.search_cmpl.conn_id = p_clcb->bta_conn_id;

  /* end of search or no server cache available */
  (*p_clcb->p_rcb->p_cback)(BTA_GATTC_SEARCH_CMPL_EVT, &cb_data);
}

/** enqueue a command into control block, usually because discovery operation is
 * busy */
void bta_gattc_q_cmd(tBTA_GATTC_CLCB* p_clcb, tBTA_GATTC_DATA* p_data) {
  bta_gattc_enqueue(p_clcb, p_data);
}

/** report API call failure back to apps */
void bta_gattc_fail(tBTA_GATTC_CLCB* p_clcb,
                    UNUSED_ATTR tBTA_GATTC_DATA* p_data) {
  if (p_clcb->status == GATT_SUCCESS) {
    LOG(ERROR) << "operation not supported at current state " << +p_clcb->state;
  }
}

/* De-Register a GATT client application with BTA completed */
static void bta_gattc_deregister_cmpl(tBTA_GATTC_RCB* p_clreg) {
  tGATT_IF client_if = p_clreg->client_if;
  tBTA_GATTC cb_data;
  tBTA_GATTC_CBACK* p_cback = p_clreg->p_cback;

  memset(&cb_data, 0, sizeof(tBTA_GATTC));

  GATT_Deregister(p_clreg->client_if);
  memset(p_clreg, 0, sizeof(tBTA_GATTC_RCB));

  cb_data.reg_oper.client_if = client_if;
  cb_data.reg_oper.status = GATT_SUCCESS;

  if (p_cback) /* callback with de-register event */
    (*p_cback)(BTA_GATTC_DEREG_EVT, &cb_data);

  if (bta_gattc_num_reg_app() == 0 &&
      bta_gattc_cb.state == BTA_GATTC_STATE_DISABLING) {
    bta_gattc_cb.state = BTA_GATTC_STATE_DISABLED;
  }
}

/** callback functions to GATT client stack */
static void bta_gattc_conn_cback(tGATT_IF gattc_if, const RawAddress& bdaddr,
                                 uint16_t conn_id, bool connected,
                                 tGATT_DISCONN_REASON reason,
                                 tBT_TRANSPORT transport) {
  if (reason != 0) {
    LOG(WARNING) << __func__ << ": cif=" << +gattc_if
                 << " connected=" << connected << " conn_id=" << loghex(conn_id)
                 << " reason=" << loghex(reason);
  }

  if (connected)
    btif_debug_conn_state(bdaddr, BTIF_DEBUG_CONNECTED, GATT_CONN_UNKNOWN);
  else {
    btif_debug_conn_state(bdaddr, BTIF_DEBUG_DISCONNECTED, reason);
    //close native socket during disconnection.
    if (bta_gattc_cb.gatt_skt_fd > -1)
      close(bta_gattc_cb.gatt_skt_fd);
    bta_gattc_cb.is_gatt_skt_connected = false;
    bta_gattc_cb.gatt_skt_fd = -1;
  }

  tBTA_GATTC_DATA* p_buf =
      (tBTA_GATTC_DATA*)osi_calloc(sizeof(tBTA_GATTC_DATA));
  p_buf->int_conn.hdr.event =
      connected ? BTA_GATTC_INT_CONN_EVT : BTA_GATTC_INT_DISCONN_EVT;
  p_buf->int_conn.hdr.layer_specific = conn_id;
  p_buf->int_conn.client_if = gattc_if;
  p_buf->int_conn.role = L2CA_GetBleConnRole(bdaddr);
  p_buf->int_conn.reason = reason;
  p_buf->int_conn.transport = transport;
  p_buf->int_conn.remote_bda = bdaddr;

  bta_sys_sendmsg(p_buf);
}

/** encryption complete callback function to GATT client stack */
static void bta_gattc_enc_cmpl_cback(tGATT_IF gattc_if, const RawAddress& bda) {
  tBTA_GATTC_CLCB* p_clcb =
      bta_gattc_find_clcb_by_cif(gattc_if, bda, GATT_TRANSPORT_LE);

  if (p_clcb == NULL) return;

  VLOG(1) << __func__ << ": cif:" << +gattc_if;

  LOG(WARNING) << __func__ << ": cif:" << +gattc_if
               << " state: " <<+p_clcb->state;

  if (p_clcb->state == BTA_GATTC_CONN_ST) {
    do_in_bta_thread(FROM_HERE,
                   base::Bind(&bta_gattc_process_enc_cmpl, gattc_if, bda));
  }
}

/** process refresh API to delete cache and start a new discovery if currently
 * connected */
void bta_gattc_process_api_refresh(const RawAddress& remote_bda) {
  tBTA_GATTC_SERV* p_srvc_cb = bta_gattc_find_srvr_cache(remote_bda);
  if (p_srvc_cb) {
    /* try to find a CLCB */
    if (p_srvc_cb->connected && p_srvc_cb->num_clcb != 0) {
      bool found = false;
      tBTA_GATTC_CLCB* p_clcb = &bta_gattc_cb.clcb[0];
      for (uint8_t i = 0; i < BTA_GATTC_CLCB_MAX; i++, p_clcb++) {
        if (p_clcb->in_use && p_clcb->p_srcb == p_srvc_cb) {
          found = true;
          break;
        }
      }
      if (found) {
          if (p_clcb->p_srcb->state == BTA_GATTC_SERV_IDLE)
            bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_DISCOVER_EVT, NULL);
          else
            APPL_TRACE_DEBUG(
            "%s: Discovery is in progress , ignore refresh.  state = %d",
           __func__, p_clcb->p_srcb->state);
        return;
      }
    }
    /* in all other cases, mark it and delete the cache */

    p_srvc_cb->gatt_database.Clear();
  }

  /* used to reset cache in application */
  bta_gattc_cache_reset(remote_bda);
}

/** process service change indication */
bool bta_gattc_process_srvc_chg_ind(uint16_t conn_id, tBTA_GATTC_RCB* p_clrcb,
                                    tBTA_GATTC_SERV* p_srcb,
                                    tBTA_GATTC_CLCB* p_clcb,
                                    tBTA_GATTC_NOTIFY* p_notify,
                                    tGATT_VALUE* att_value) {

  Uuid gattp_uuid = Uuid::From16Bit(UUID_SERVCLASS_GATT_SERVER);
  Uuid srvc_chg_uuid = Uuid::From16Bit(GATT_UUID_GATT_SRV_CHGD);

  if (p_srcb->gatt_database.IsEmpty() && p_srcb->state == BTA_GATTC_SERV_IDLE) {
    gatt::Database db = bta_gattc_cache_load(p_srcb->server_bda);
    if (!db.IsEmpty()) {
      p_srcb->gatt_database = db;
    }
  }

  const gatt::Characteristic* p_char =
      bta_gattc_get_characteristic_srcb(p_srcb, p_notify->handle);
  if (!p_char) return false;
  const gatt::Service* p_svc =
      bta_gattc_get_service_for_handle_srcb(p_srcb, p_char->value_handle);
  if (!p_svc || p_svc->uuid != gattp_uuid || p_char->uuid != srvc_chg_uuid) {
    return false;
  }

  if (att_value->len != BTA_GATTC_SERVICE_CHANGED_LEN) {
    LOG(ERROR) << __func__
               << ": received malformed service changed indication, skipping";
    return false;
  }

  uint8_t* p = att_value->value;
  uint16_t s_handle = ((uint16_t)(*(p)) + (((uint16_t)(*(p + 1))) << 8));
  uint16_t e_handle = ((uint16_t)(*(p + 2)) + (((uint16_t)(*(p + 3))) << 8));

  LOG(ERROR) << __func__ << ": service changed s_handle=" << loghex(s_handle)
             << ", e_handle=" << loghex(e_handle);

  /* mark service handle change pending */
  p_srcb->srvc_hdl_chg = true;
  /* clear up all notification/indication registration */
  bta_gattc_clear_notif_registration(p_srcb, conn_id, s_handle, e_handle);
  /* service change indication all received, do discovery update */
  if (++p_srcb->update_count == bta_gattc_num_reg_app()) {
    /* not an opened connection; or connection busy */
    /* search for first available clcb and start discovery */
    if (p_clcb == NULL || (p_clcb && p_clcb->p_q_cmd != NULL)) {
      for (size_t i = 0; i < BTA_GATTC_CLCB_MAX; i++) {
        if (bta_gattc_cb.clcb[i].in_use &&
            bta_gattc_cb.clcb[i].p_srcb == p_srcb &&
            bta_gattc_cb.clcb[i].p_q_cmd == NULL) {
          p_clcb = &bta_gattc_cb.clcb[i];
          break;
        }
      }
    }
    /* send confirmation here if this is an indication, it should always be */
    GATTC_SendHandleValueConfirm(conn_id, att_value->handle, p_notify->trans_id);

    /* if connection available, refresh cache by doing discovery now */
    if (p_clcb) {
      /* request read db hash first */
      if (bta_gattc_is_robust_caching_enabled()) {
        p_srcb->srvc_hdl_db_hash = true;
      }
      bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_DISCOVER_EVT, NULL);
    }
  }

  /* notify applicationf or service change */
  if (p_clrcb->p_cback) {
    tBTA_GATTC bta_gattc;
    bta_gattc.service_changed.remote_bda = p_srcb->server_bda;
    bta_gattc.service_changed.conn_id = conn_id;
    (*p_clrcb->p_cback)(BTA_GATTC_SRVC_CHG_EVT, &bta_gattc);
  }

  return true;
}

/** build native access GATT notification data */
uint8_t* bta_gattc_build_skt_notification_data(Uuid char_uuid,
                                               tGATT_CL_COMPLETE* p_data,
                                               size_t* total_len) {
  size_t data_hdr_len = 4;
  uint8_t char_uuid_type = 0x01;
  uint8_t notif_data_type = 0x02;
  size_t len_char_uuid = char_uuid.GetShortestRepresentationSize();
  size_t len_data = p_data->att_value.len;
  uint8_t* p_uuid = new uint8_t[len_char_uuid];
  *total_len = len_char_uuid + len_data + data_hdr_len;
  uint8_t* p = new uint8_t[*total_len];
  uint8_t* pp = p;
  uint8_t* p_tmp = pp;

  sscanf(char_uuid.ToString().c_str(),
         "%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx"
         "-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
         &p_uuid[0], &p_uuid[1], &p_uuid[2], &p_uuid[3], &p_uuid[4], &p_uuid[5],
         &p_uuid[6], &p_uuid[7], &p_uuid[8], &p_uuid[9], &p_uuid[10], &p_uuid[11],
         &p_uuid[12], &p_uuid[13], &p_uuid[14], &p_uuid[15]);

  //LTV for char uuid
  UINT8_TO_STREAM(pp, (len_char_uuid+1));
  UINT8_TO_STREAM(pp, char_uuid_type);
  ARRAY_TO_STREAM(pp, p_uuid, (int)len_char_uuid);

  //LTV for notification data
  UINT8_TO_STREAM(pp, (len_data+1));
  UINT8_TO_STREAM(pp, notif_data_type);
  ARRAY_TO_STREAM(pp, p_data->att_value.value, (int)len_data);

  VLOG(1) << __func__ << " Socket data bytes: ";
  for(uint8_t k=0; k<(data_hdr_len + len_char_uuid + len_data); k++) {
    VLOG(1) << __func__ << " value: " << +p_tmp[k];
  }

  return p;
}

/** write native access GATT notification data to socket */
bool bta_gattc_write_to_socket(tGATT_CL_COMPLETE* p_data, Uuid char_uuid) {
  size_t count = 0, total_len = 0;
  int sent = 0;
  uint8_t* p_skt_data;

  VLOG(1) << __func__;
  if (bta_gattc_cb.gatt_skt_fd < 0) {
    bta_gattc_cb.gatt_skt_fd = socket(AF_LOCAL, SOCK_SEQPACKET, 0);

    if (bta_gattc_cb.gatt_skt_fd < 0) {
      VLOG(1) << __func__ << " failed to create socket";
      bta_gattc_cb.is_gatt_skt_connected = false;
      bta_gattc_cb.gatt_skt_fd = -1;
      return false;
    }
  }

  if (!bta_gattc_cb.is_gatt_skt_connected) {
    if (osi_socket_local_client_connect(
        bta_gattc_cb.gatt_skt_fd, BTA_GATTC_NATIVE_ACCESS_SOCKET, ANDROID_SOCKET_NAMESPACE_FILESYSTEM, SOCK_SEQPACKET) < 0) {
      VLOG(1) << __func__ << " failed to connect: error: " << +strerror(errno);
      close(bta_gattc_cb.gatt_skt_fd);
      bta_gattc_cb.gatt_skt_fd = -1;
      bta_gattc_cb.is_gatt_skt_connected = false;
      return false;
    }
    else {
      bta_gattc_cb.is_gatt_skt_connected = true;
    }
  }

  p_skt_data = bta_gattc_build_skt_notification_data(char_uuid, p_data, &total_len);

  //write to socket
  while (count < total_len) {
    OSI_NO_INTR(sent = send(bta_gattc_cb.gatt_skt_fd, p_skt_data, total_len - count, MSG_NOSIGNAL | MSG_DONTWAIT));
    if (sent == -1) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        VLOG(1) << __func__ << "write failed with error:" << +strerror(errno);
        bta_gattc_cb.is_gatt_skt_connected = false;
        close(bta_gattc_cb.gatt_skt_fd);
        bta_gattc_cb.gatt_skt_fd = -1;
        delete[] p_skt_data;
        return false;
      }
    }
    count += sent;
    p_skt_data = (uint8_t*)p_skt_data + sent;
  }

  delete[] p_skt_data;
  return true;
}

/** process native access notification */
void bta_gattc_proc_native_access_notification(tBTA_GATTC_SERV* p_srcb,
                                    tGATT_CL_COMPLETE* p_data) {
  VLOG(1) << __func__
          << StringPrintf(": p_data->att_value.handle=%d",
          p_data->att_value.handle);

  std::vector<Uuid> char_uuid_list = bta_gattc_cb.native_access_uuid_list;
  std::vector<Uuid>::iterator it;

  uint16_t handle = p_data->att_value.handle;
  const gatt::Characteristic* p_char = bta_gattc_get_characteristic_srcb(p_srcb, handle);
  bool ret = false;
  if(p_char) {
    it = std::find (char_uuid_list.begin(), char_uuid_list.end(), p_char->uuid);
    if (it != char_uuid_list.end()) {
      VLOG(1) << __func__ << " GATT characteristic receiving notifications";
      ret = bta_gattc_write_to_socket(p_data, p_char->uuid);
      if(ret) {
        VLOG(1) << __func__ << " GATT notification data write to socket successful";
      }
      else {
        VLOG(1) << __func__ << " GATT notification data write to socket unsuccessful";
      }
    }
  }
}

/** process all non-service change indication/notification */
void bta_gattc_proc_other_indication(tBTA_GATTC_CLCB* p_clcb, uint8_t op,
                                     tGATT_CL_COMPLETE* p_data,
                                     tBTA_GATTC_NOTIFY* p_notify) {
  VLOG(1) << __func__
          << StringPrintf(
                 ": check p_data->att_value.handle=%d p_data->handle=%d",
                 p_data->att_value.handle, p_data->handle);
  VLOG(1) << "is_notify" << p_notify->is_notify;
  uint16_t conn_id = 0;
  RawAddress remote_bda;
  tGATT_IF gatt_if;
  tBTA_TRANSPORT transport;

  if (!p_clcb) {
    LOG(ERROR) << __func__ << ": p_clcb is NULL";
    return;
  }

  if(bta_gattc_cb.native_access_notif_enabled) {
    //check for native access notification
    if(op == GATTC_OPTYPE_NOTIFICATION) {
      if (p_clcb) {
        conn_id = p_clcb->bta_conn_id;
        if (!GATT_GetConnectionInfor(conn_id, &gatt_if, remote_bda, &transport)) {
          LOG(ERROR) << __func__ << ": notification for unknown app";
          return;
        }
        tBTA_GATTC_SERV* p_srcb = bta_gattc_find_srcb(remote_bda);
        bta_gattc_proc_native_access_notification(p_srcb, p_data);
      }
    }
  }

  p_notify->is_notify = (op == GATTC_OPTYPE_INDICATION) ? false : true;
  p_notify->len = p_data->att_value.len;
  p_notify->bda = p_clcb->bda;
  memcpy(p_notify->value, p_data->att_value.value, p_data->att_value.len);
  p_notify->conn_id = p_clcb->bta_conn_id;

#ifdef ADV_AUDIO_FEATURE
  if (is_remote_support_adv_audio(p_clcb->bda)) {
    auto itr = dev_addr_map.find(p_notify->conn_id);
    if (itr != dev_addr_map.end()) {
      p_notify->bda = itr->second;
    }
  }
#endif
  if (p_clcb->p_rcb->p_cback) {
    tBTA_GATTC bta_gattc;
    bta_gattc.notify = *p_notify;
    (*p_clcb->p_rcb->p_cback)(BTA_GATTC_NOTIF_EVT, &bta_gattc);
  }
}

/** process indication/notification */
void bta_gattc_process_indicate(uint16_t conn_id, tGATTC_OPTYPE op,
                                tGATT_CL_COMPLETE* p_data, uint32_t trans_id) {
  uint16_t handle = p_data->att_value.handle;
  tBTA_GATTC_NOTIFY notify;
  RawAddress remote_bda;
  tGATT_IF gatt_if;
  tBTA_TRANSPORT transport;

  if (!GATT_GetConnectionInfor(conn_id, &gatt_if, remote_bda, &transport)) {
    LOG(ERROR) << __func__ << ": indication/notif for unknown app";
    if (op == GATTC_OPTYPE_INDICATION)
      GATTC_SendHandleValueConfirm(conn_id, handle, trans_id);
    return;
  }

  tBTA_GATTC_RCB* p_clrcb = bta_gattc_cl_get_regcb(gatt_if);
  if (p_clrcb == NULL) {
    LOG(ERROR) << __func__ << ": indication/notif for unregistered app";
    if (op == GATTC_OPTYPE_INDICATION)
      GATTC_SendHandleValueConfirm(conn_id, handle, trans_id);
    return;
  }

  tBTA_GATTC_SERV* p_srcb = bta_gattc_find_srcb(remote_bda);
  if (p_srcb == NULL) {
    LOG(ERROR) << __func__ << ": indication/notif for unknown device, ignore";
    if (op == GATTC_OPTYPE_INDICATION)
      GATTC_SendHandleValueConfirm(conn_id, handle, trans_id);
    return;
  }

  LOG(INFO) << __func__ << " conn_id " << conn_id << " gatt_if "
    << loghex(gatt_if);
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);

  notify.handle = handle;
  notify.trans_id = trans_id;

  /* if service change indication/notification, don't forward to application */
  if (bta_gattc_process_srvc_chg_ind(conn_id, p_clrcb, p_srcb, p_clcb, &notify,
                                     &p_data->att_value))
    return;

  /* Not a service change indication, check for an unallocated HID conn */
  if (bta_hh_le_is_hh_gatt_if(gatt_if) && !p_clcb) {
    APPL_TRACE_ERROR("%s, ignore HID ind/notificiation", __func__);
    return;
  }

  /* if app registered for the notification */
  if (bta_gattc_check_notif_registry(p_clrcb, p_srcb, &notify)) {
    /* connection not open yet */
  LOG(ERROR) << __func__ << " conn_id " << conn_id << " gatt_if " << gatt_if;
    if (p_clcb == NULL) {
      p_clcb = bta_gattc_clcb_alloc(gatt_if, remote_bda, transport);

      if (p_clcb == NULL) {
        LOG(ERROR) << "No resources";
        return;
      }

      p_clcb->bta_conn_id = conn_id;
      p_clcb->transport = transport;

      bta_gattc_sm_execute(p_clcb, BTA_GATTC_INT_CONN_EVT, NULL);
    }

    if (p_clcb != NULL)
      bta_gattc_proc_other_indication(p_clcb, op, p_data, &notify);
  }
  /* no one intersted and need ack? */
  else if (op == GATTC_OPTYPE_INDICATION) {
    VLOG(1) << __func__ << " no one interested, ack now";
    GATTC_SendHandleValueConfirm(conn_id, handle, trans_id);
  }
}

/** client operation complete callback register with BTE GATT */
static void bta_gattc_cmpl_cback(uint16_t conn_id, tGATTC_OPTYPE op,
                                 tGATT_STATUS status,
                                 tGATT_CL_COMPLETE* p_data,
                                 uint32_t trans_id) {
  VLOG(1) << __func__ << ": conn_id:" << +conn_id << " op:" << +op
          << " status:" << +status;

  /* notification and indication processed right away */
  if (op == GATTC_OPTYPE_NOTIFICATION || op == GATTC_OPTYPE_INDICATION) {
    bta_gattc_process_indicate(conn_id, op, p_data, trans_id);
    return;
  }
  /* for all other operation, not expected if w/o connection */
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
  if (!p_clcb) {
    LOG(ERROR) << __func__ << ": unknown conn_id=" << loghex(conn_id)
               << " ignore data";
    return;
  }

  /* if over BR_EDR, inform PM for mode change */
  if (p_clcb->transport == BTA_TRANSPORT_BR_EDR) {
    bta_sys_busy(BTA_ID_GATTC, BTA_ALL_APP_ID, p_clcb->bda);
    bta_sys_idle(BTA_ID_GATTC, BTA_ALL_APP_ID, p_clcb->bda);
  }

  bta_gattc_cmpl_sendmsg(conn_id, op, status, p_data);
}

/** client operation complete send message */
void bta_gattc_cmpl_sendmsg(uint16_t conn_id, tGATTC_OPTYPE op,
                            tGATT_STATUS status, tGATT_CL_COMPLETE* p_data) {
  const size_t len = sizeof(tBTA_GATTC_OP_CMPL) + sizeof(tGATT_CL_COMPLETE);
  tBTA_GATTC_OP_CMPL* p_buf = (tBTA_GATTC_OP_CMPL*)osi_calloc(len);

  p_buf->hdr.event = BTA_GATTC_OP_CMPL_EVT;
  p_buf->hdr.layer_specific = conn_id;
  p_buf->status = status;
  p_buf->op_code = op;

  if (p_data) {
    p_buf->p_cmpl = (tGATT_CL_COMPLETE*)(p_buf + 1);
    memcpy(p_buf->p_cmpl, p_data, sizeof(tGATT_CL_COMPLETE));
  }

  bta_sys_sendmsg(p_buf);
}

/** congestion callback for BTA GATT client */
static void bta_gattc_cong_cback(uint16_t conn_id, bool congested) {
  tBTA_GATTC_CLCB* p_clcb = bta_gattc_find_clcb_by_conn_id(conn_id);
  if (!p_clcb || !p_clcb->p_rcb->p_cback) return;

  tBTA_GATTC cb_data;
  cb_data.congest.conn_id = conn_id;
  cb_data.congest.congested = congested;

  (*p_clcb->p_rcb->p_cback)(BTA_GATTC_CONGEST_EVT, &cb_data);
}

static void bta_gattc_phy_update_cback(tGATT_IF gatt_if, uint16_t conn_id,
                                       uint8_t tx_phy, uint8_t rx_phy,
                                       uint8_t status) {
  tBTA_GATTC_RCB* p_clreg = bta_gattc_cl_get_regcb(gatt_if);

  if (!p_clreg || !p_clreg->p_cback) {
    LOG(ERROR) << __func__ << ": client_if=" << +gatt_if << " not found";
    return;
  }

  tBTA_GATTC cb_data;
  cb_data.phy_update.conn_id = conn_id;
  cb_data.phy_update.server_if = gatt_if;
  cb_data.phy_update.tx_phy = tx_phy;
  cb_data.phy_update.rx_phy = rx_phy;
  cb_data.phy_update.status = status;
  (*p_clreg->p_cback)(BTA_GATTC_PHY_UPDATE_EVT, &cb_data);
}

static void bta_gattc_conn_update_cback(tGATT_IF gatt_if, uint16_t conn_id,
                                        uint16_t interval, uint16_t latency,
                                        uint16_t timeout, uint8_t status) {
  tBTA_GATTC_RCB* p_clreg = bta_gattc_cl_get_regcb(gatt_if);

  if (!p_clreg || !p_clreg->p_cback) {
    LOG(ERROR) << __func__ << ": client_if=" << gatt_if << " not found";
    return;
  }

  tBTA_GATTC cb_data;
  cb_data.conn_update.conn_id = conn_id;
  cb_data.conn_update.interval = interval;
  cb_data.conn_update.latency = latency;
  cb_data.conn_update.timeout = timeout;
  cb_data.conn_update.status = status;
  (*p_clreg->p_cback)(BTA_GATTC_CONN_UPDATE_EVT, &cb_data);
}

static void bta_gattc_subrate_chg_cback(tGATT_IF gatt_if, uint16_t conn_id,
                                        uint16_t subrate_factor, uint16_t latency,
                                        uint16_t cont_num, uint16_t timeout,
                                        uint8_t status) {
  tBTA_GATTC_RCB* p_clreg = bta_gattc_cl_get_regcb(gatt_if);

  if (!p_clreg || !p_clreg->p_cback) {
    LOG(ERROR) << __func__ << ": client_if=" << gatt_if << " not found";
    return;
  }

  tBTA_GATTC cb_data;
  cb_data.subrate_chg.conn_id = conn_id;
  cb_data.subrate_chg.subrate_factor = subrate_factor;
  cb_data.subrate_chg.latency = latency;
  cb_data.subrate_chg.cont_num = cont_num;
  cb_data.subrate_chg.timeout = timeout;
  cb_data.subrate_chg.status = status;
  (*p_clreg->p_cback)(BTA_GATTC_SUBRATE_CHG_EVT, &cb_data);
}
