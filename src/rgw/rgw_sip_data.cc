
#include "common/debug.h"
#include "common/ceph_json.h"
#include "common/Formatter.h"

#include "rgw_sip_data.h"
#include "rgw_bucket.h"
#include "rgw_b64.h"
#include "rgw_datalog.h"

#define dout_subsys ceph_subsys_rgw

using namespace std;

void siprovider_data_info::dump(Formatter *f) const
{
  encode_json("key", key, f);
  encode_json("shard_id", shard_id, f);
  encode_json("num_shards", num_shards, f);
  encode_json("timestamp", timestamp, f);
}

void siprovider_data_info::decode_json(JSONObj *obj)
{
  JSONDecoder::decode_json("key", key, obj);
  JSONDecoder::decode_json("shard_id", shard_id, obj);
  JSONDecoder::decode_json("num_shards", num_shards, obj);
  JSONDecoder::decode_json("timestamp", timestamp, obj);
}

static int siprovider_data_create_entry(const DoutPrefixProvider *dpp,
                                        rgw::sal::Driver *driver,
                                        const string& key,
                                        std::optional<ceph::real_time> timestamp,
                                        const std::string& m,
                                        SIProvider::Entry *result)
{
  rgw_bucket bucket;
  int shard_id;
  rgw_bucket_parse_bucket_key(dpp->get_cct(), key, &bucket, &shard_id);

  std::unique_ptr<rgw::sal::Bucket> b;
  int ret = driver->get_bucket(dpp, nullptr, bucket, &b, null_yield);
  if (ret < 0) {
    ldpp_dout(dpp, 0) << "ERROR: " << __func__ << "(): cannot read bucket instance info for bucket=" << bucket << dendl;
    return ret;
  }

  siprovider_data_info data_info = { key,
                                     shard_id,
                                     (int)b->get_info().layout.current_index.layout.normal.num_shards,
                                     timestamp };
  result->key = m;
  data_info.encode(result->data);
  
  return 0;
}


int SIProvider_DataFull::do_fetch(const DoutPrefixProvider *dpp,
                                  int shard_id, std::string marker, int max, fetch_result *result)
{
  if (shard_id != 0) {
    return -ERANGE;
  }

  string section = "bucket.instance";

  void *handle;

  result->done = false;
  result->more = true;

  auto& m = marker;

  int ret = meta.mgr->list_keys_init(dpp, section, m, &handle);
  if (ret < 0) {
    lderr(cct) << "ERROR: " << __func__ << "(): list_keys_init() returned ret=" << ret << dendl;
    return ret;
  }

  while (max > 0) {
    std::list<RGWMetadataHandler::KeyInfo> entries;
    bool truncated;

    ret = meta.mgr->list_keys_next(dpp, handle, max, entries,
                                   &truncated);
    if (ret < 0) {
      lderr(cct) << "ERROR: " << __func__ << "(): list_keys_init() returned ret=" << ret << dendl;
      return ret;
    }

    if (!entries.empty()) {
      max -= entries.size();

      m = entries.back().marker;

      for (auto& k : entries) {
        SIProvider::Entry e;
        ret = siprovider_data_create_entry(dpp, driver, k.key, std::nullopt,
                                           k.marker, &e);
        if (ret < 0) {
          ldpp_dout(dpp, 0) << "ERROR: " << __func__ << "(): skipping entry,siprovider_data_create_entry() returned error: key=" << k.key << " ret=" << ret << dendl;
          continue;
        }

        result->entries.push_back(e);
      }
    }

    if (!truncated) {
      result->done = true;
      result->more = false;
      break;
    }
  }


  return 0;
}

SIProvider_DataInc::SIProvider_DataInc(CephContext *_cct,
				       RGWDataChangesLog *_datalog_svc,
                                       rgw::sal::Driver *_driver) : SIProvider_SingleStage(_cct,
                                                                                           "data.inc",
                                                                                           std::nullopt,
                                                                                           std::make_shared<SITypeHandlerProvider_Default<siprovider_data_info> >(),
                                                                                           std::nullopt, /* stage id */
                                                                                           SIProvider::StageType::INC,
                                                                                           _cct->_conf->rgw_data_log_num_shards,
                                                                                           false), driver(_driver) {
  svc.datalog = _datalog_svc;
}

int SIProvider_DataInc::init(const DoutPrefixProvider *dpp)
{
  data_log = svc.datalog;
  return 0;
}

int SIProvider_DataInc::do_fetch(const DoutPrefixProvider *dpp,
                                 int shard_id, std::string marker, int max, fetch_result *result)
{
  if (shard_id >= stage_info.num_shards ||
      shard_id < 0) {
    return -ERANGE;
  }

  auto& m = marker;

  utime_t start_time;
  utime_t end_time;

  bool truncated;
  do {
    vector<rgw_data_change_log_entry> entries;
    int ret = data_log->list_entries(dpp, shard_id, max, entries, m, &m, &truncated, null_yield);
    if (ret == -ENOENT) {
      truncated = false;
      break;
    }
    if (ret < 0) {
      lderr(cct) << "ERROR: data_log->list_entries() failed: ret=" << ret << dendl;
      return -ret;
    }

    max -= entries.size();

    for (auto& entry : entries) {
      SIProvider::Entry e;
      ret = siprovider_data_create_entry(dpp, driver, entry.entry.key, entry.entry.timestamp,
                                         entry.log_id, &e);
      if (ret < 0) {
        ldpp_dout(dpp, 0) << "ERROR: " << __func__ << "(): skipping entry,siprovider_data_create_entry() returned error: key=" << entry.entry.key << " ret=" << ret << dendl;
        continue;
      }

      result->entries.push_back(e);
    }
  } while (truncated && max > 0);

  result->done = false; /* FIXME */
  result->more = truncated;

  return 0;
}


int SIProvider_DataInc::do_get_start_marker(const DoutPrefixProvider *dpp,
                                            int shard_id, std::string *marker, ceph::real_time *timestamp) const
{
  marker->clear();
  *timestamp = ceph::real_time();
  return 0;
}

int SIProvider_DataInc::do_get_cur_state(const DoutPrefixProvider *dpp,
                                         int shard_id, std::string *marker, ceph::real_time *timestamp,
                                         bool *disabled, optional_yield y) const
{
  RGWDataChangesLogInfo info;
  int ret = data_log->get_info(dpp, shard_id, &info, y);
  if (ret == -ENOENT) {
    ret = 0;
  }
  if (ret < 0) {
    lderr(cct) << "ERROR: data_log->get_info() returned ret=" << ret << dendl;
    return ret;
  }
  *marker = info.marker;
  *timestamp = info.last_update;
  *disabled = false;
  return 0;
}

int SIProvider_DataInc::do_trim(const DoutPrefixProvider *dpp,
                                int shard_id, const std::string& marker)
{
  utime_t start_time, end_time;
  int ret;
  // trim until -ENODATA
  do {
    ret = data_log->trim_entries(dpp, shard_id, marker, null_yield);
  } while (ret == 0);
  if (ret < 0 && ret != -ENODATA) {
    ldpp_dout(dpp, 20) << "ERROR: data_log->trim(): returned ret=" << ret << dendl;
    return ret;
  }
  return 0;
}