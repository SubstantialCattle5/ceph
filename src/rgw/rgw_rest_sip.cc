// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013 eNovance SAS <licensing@enovance.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include "rgw_rest_sip.h"
#include "rgw_rest_conn.h"
#include "rgw_sync_info.h"
#include "rgw_sal_rados.h"

#include "common/errno.h"
#include "include/ceph_assert.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw


void RGWOp_SIP_GetInfo::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");

  if (provider) {
    sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, *provider, opt_instance);
  } else {
    sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip_by_type(this,
                                                 *data_type,
                                                 SIProvider::stage_type_from_str(*stage_type),
                                                 opt_instance);
  }
  if (!sip) {
    ldout(s->cct, 20) << "ERROR: sync info provider not found" << dendl;
    op_ret = -ENOENT;
    return;
  }
}

void RGWOp_SIP_GetInfo::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;

  encode_json("info", sip->get_info(this), s->formatter);
  flusher.flush();
}

void RGWOp_SIP_GetStageStatus::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");

  auto sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, provider, opt_instance);
  if (!sip) {
    ldout(s->cct, 5) << "ERROR: sync info provider not found" << dendl;
    op_ret = -ENOENT;
    return;
  }

  auto opt_stage_id = s->info.args.get_std_optional("stage-id");
  if (!opt_stage_id) {
    ldout(s->cct,  5) << "ERROR: missing 'stage-id' param" << dendl;
    op_ret = -EINVAL;
    return;
  }
  auto& sid = *opt_stage_id;

  int shard_id;
  op_ret = s->info.args.get_int("shard-id", &shard_id, 0);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'shard-id' param: " << op_ret << dendl;
    return;
  }

  op_ret = sip->get_start_marker(this, sid, shard_id, &start_pos.marker, &start_pos.timestamp);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: sip->get_start_marker() returned error: ret=" << op_ret << dendl;
    return;
  }

  op_ret = sip->get_cur_state(this, sid, shard_id, &cur_pos.marker, &cur_pos.timestamp, &disabled, y);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: sip->get_cur_state() returned error: ret=" << op_ret << dendl;
    return;
  }
}

void RGWOp_SIP_GetStageStatus::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;

  {
    Formatter::ObjectSection top_section(*s->formatter, "result");
    {
      Formatter::ObjectSection markers_section(*s->formatter, "markers");
      encode_json("start", start_pos, s->formatter);
      encode_json("current", cur_pos, s->formatter);
    }
    encode_json("disabled", disabled, s->formatter);
  }
  flusher.flush();
}

void RGWOp_SIP_GetMarkerInfo::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");

  auto sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, provider, opt_instance);
  if (!sip) {
    ldout(s->cct, 5) << "ERROR: sync info provider not found" << dendl;
    op_ret = -ENOENT;
    return;
  }

  auto opt_stage_id = s->info.args.get_std_optional("stage-id");
  if (!opt_stage_id) {
    ldout(s->cct,  5) << "ERROR: missing 'stage-id' param" << dendl;
    op_ret = -EINVAL;
    return;
  }
  auto& sid = *opt_stage_id;

  int shard_id;
  op_ret = s->info.args.get_int("shard-id", &shard_id, 0);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'shard-id' param: " << op_ret << dendl;
    return;
  }

  auto marker_handler = static_cast<rgw::sal::RadosStore*>(driver)->svc()->sip_marker->get_handler(sip);
  if (!marker_handler) {
    ldout(s->cct, 0) << "ERROR: can't get sip marker handler" << dendl;
    op_ret = -EIO;
    return;
  }

  op_ret = marker_handler->get_info(this, sid, shard_id, &sinfo);
  if (op_ret < 0) {
    ldout(s->cct, 0) << "ERROR: failed to fetch marker handler stage marker info: " << cpp_strerror(-op_ret) << dendl;
    return;
  }

}

void RGWOp_SIP_GetMarkerInfo::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;

  {
    Formatter::ObjectSection top_section(*s->formatter, "result");
    encode_json("info", sinfo, s->formatter);
  }
  flusher.flush();
}

void RGWOp_SIP_SetMarkerInfo::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");

  auto sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, provider, opt_instance);
  if (!sip) {
    ldout(s->cct, 5) << "ERROR: sync info provider not found" << dendl;
    op_ret = -ENOENT;
    return;
  }

  auto opt_stage_id = s->info.args.get_std_optional("stage-id");
  if (!opt_stage_id) {
    ldout(s->cct,  5) << "ERROR: missing 'stage-id' param" << dendl;
    op_ret = -EINVAL;
    return;
  }
  auto& sid = *opt_stage_id;

  int shard_id;
  op_ret = s->info.args.get_int("shard-id", &shard_id, 0);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'shard-id' param: " << op_ret << dendl;
    return;
  }

#define SET_PARAMS_INPUT_MAX_LEN (128 * 1024)

  RGWSI_SIP_Marker::SetParams params;
  bool empty;
  op_ret = get_json_input(driver->ctx(), s, params, SET_PARAMS_INPUT_MAX_LEN, &empty);
  if (op_ret < 0) {
    ldout(s->cct,  5) << "ERROR: " << __func__ << "(): failed parsing input" << dendl;
    return;
  }

  if (params.target_id.empty()) {
    ldout(s->cct,  5) << "ERROR: missing or empty target_id field" << dendl;
    op_ret = -EINVAL;
    return;
  }

  auto marker_handler = static_cast<rgw::sal::RadosStore*>(driver)->svc()->sip_marker->get_handler(sip);
  if (!marker_handler) {
    ldout(s->cct, 0) << "ERROR: can't get sip marker handler" << dendl;
    op_ret = -EIO;
    return;
  }

  RGWSI_SIP_Marker::Handler::modify_result result;

  op_ret = marker_handler->set_marker(this, sid, shard_id, params, &result);
  if (op_ret < 0) {
    ldout(s->cct, 0) << "ERROR: failed to set target marker info: " << cpp_strerror(-op_ret) << dendl;
    return;
  }
}

void RGWOp_SIP_SetMarkerInfo::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;
}

void RGWOp_SIP_RemoveMarkerInfo::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");

  auto sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, provider, opt_instance);
  if (!sip) {
    ldout(s->cct, 5) << "ERROR: sync info provider not found" << dendl;
    op_ret = -ENOENT;
    return;
  }

  auto opt_stage_id = s->info.args.get_std_optional("stage-id");
  if (!opt_stage_id) {
    ldout(s->cct,  5) << "ERROR: missing 'stage-id' param" << dendl;
    op_ret = -EINVAL;
    return;
  }
  auto& sid = *opt_stage_id;

  int shard_id;
  op_ret = s->info.args.get_int("shard-id", &shard_id, 0);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'shard-id' param: " << op_ret << dendl;
    return;
  }

  auto opt_target_id = s->info.args.get_std_optional("target-id");
  if (!opt_target_id) {
    ldout(s->cct,  5) << "ERROR: missing 'target-id' param" << dendl;
    op_ret = -EINVAL;
    return;
  }

  auto marker_handler = static_cast<rgw::sal::RadosStore*>(driver)->svc()->sip_marker->get_handler(sip);
  if (!marker_handler) {
    ldout(s->cct, 0) << "ERROR: can't get sip marker handler" << dendl;
    op_ret = -EIO;
    return;
  }

  RGWSI_SIP_Marker::Handler::modify_result result;

  op_ret = marker_handler->remove_target(this, *opt_target_id, sid, shard_id, &result);
  if (op_ret < 0) {
    ldout(s->cct, 0) << "ERROR: failed to remove target marker info: " << cpp_strerror(-op_ret) << dendl;
    return;
  }
}

void RGWOp_SIP_RemoveMarkerInfo::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;
}

void RGWOp_SIP_List::execute(optional_yield y) {
  providers = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->list_sip();
}

void RGWOp_SIP_List::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;

  encode_json("providers", providers, s->formatter);
  flusher.flush();
}

void RGWOp_SIP_Fetch::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");
  auto opt_stage_id = s->info.args.get_std_optional("stage-id");
  auto marker = s->info.args.get("marker");

  int max_entries;
  op_ret = s->info.args.get_int("max", &max_entries, default_max);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'max' param: " << op_ret << dendl;
    return;
  }

  int shard_id;
  op_ret = s->info.args.get_int("shard-id", &shard_id, 0);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'shard-id' param: " << op_ret << dendl;
    return;
  }

  sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, provider, opt_instance);
  if (!sip) {
    ldout(s->cct, 20) << "ERROR: sync info provider not found" << dendl;
    return;
  }

  stage_id = opt_stage_id.value_or(sip->get_first_stage(this));

  type_handler = sip->get_type_handler();
  if (!type_handler) {
    ldout(s->cct, 0) << "ERROR: " << __func__ << "(): null type handler, likely a bug" << dendl;
    op_ret = -EIO;
    return;
  }

  op_ret = sip->fetch(this, stage_id, shard_id, marker, max_entries, &result);
  if (op_ret < 0) {
    ldout(s->cct, 0) << "ERROR: failed to fetch entries: " << op_ret << dendl;
    return;
  }
}

void RGWOp_SIP_Fetch::send_response() {
  set_req_state_err(s, op_ret);
  dump_errno(s);
  end_header(s);

  if (op_ret < 0)
    return;

  auto formatter = s->formatter;

  {
    Formatter::ObjectSection top_section(*formatter, "result");
    encode_json("more", result.more, formatter);
    encode_json("done", result.done, formatter);

    Formatter::ArraySection as(*formatter, "entries");

    for (auto& e : result.entries) {
      Formatter::ObjectSection hs(*formatter, "handler");
      encode_json("key", e.key, formatter);
      int r = type_handler->handle_entry(stage_id, e, [&](SIProvider::EntryInfoBase& info) {
                                         encode_json("info", info, formatter);
                                         return 0;
                                       });
      if (r < 0) {
        ldout(s->cct, 0) << "ERROR: provider->handle_entry() failed: r=" << r << dendl;
        break;
      }
    }
  }
  flusher.flush();
}

void RGWOp_SIP_Trim::execute(optional_yield y) {
  auto opt_instance = s->info.args.get_std_optional("instance");
  auto opt_stage_id = s->info.args.get_std_optional("stage-id");
  auto marker = s->info.args.get("marker");

  int shard_id;
  op_ret = s->info.args.get_int("shard-id", &shard_id, 0);
  if (op_ret < 0) {
    ldout(s->cct, 5) << "ERROR: invalid 'shard-id' param: " << op_ret << dendl;
    return;
  }

  auto sip = static_cast<rgw::sal::RadosStore*>(driver)->ctl()->si.mgr->find_sip(this, provider, opt_instance);
  if (!sip) {
    ldout(s->cct, 20) << "ERROR: sync info provider not found" << dendl;
    return;
  }

  auto stage_id = opt_stage_id.value_or(sip->get_first_stage(this));

  op_ret = sip->trim(this, stage_id, shard_id, marker);
  if (op_ret < 0) {
    ldout(s->cct, 0) << "ERROR: failed to fetch entries: " << op_ret << dendl;
    return;
  }
}

RGWOp *RGWHandler_SIP::op_get() {
  auto provider = s->info.args.get_std_optional("provider");
  auto data_type = s->info.args.get_std_optional("data-type");
  auto stage_type = s->info.args.get_std_optional("stage-type");

  if (!provider) {
    if (!data_type && !stage_type) {
      return new RGWOp_SIP_List;
    }
  }

  if (s->info.args.exists("info")) {
    return new RGWOp_SIP_GetInfo(provider, data_type, stage_type);
  }

  if (!provider) {
    return nullptr;
  }

  if (s->info.args.exists("status")) {
    return new RGWOp_SIP_GetStageStatus(std::move(*provider));
  }

  if (s->info.args.exists("marker-info")) {
    return new RGWOp_SIP_GetMarkerInfo(std::move(*provider));
  }

  return new RGWOp_SIP_Fetch(std::move(*provider));
}

RGWOp *RGWHandler_SIP::op_put() {
  auto provider = s->info.args.get_std_optional("provider");
  if (!provider) {
    return new RGWOp_SIP_List;
  }

  if (s->info.args.exists("marker-info")) {
    return new RGWOp_SIP_SetMarkerInfo(std::move(*provider));
  }

  return nullptr;
}

RGWOp *RGWHandler_SIP::op_delete() {
  auto provider = s->info.args.get_std_optional("provider");
  if (!provider) {
    return nullptr;
  }

  return new RGWOp_SIP_Trim(std::move(*provider));
}
