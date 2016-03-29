/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PI/pi_base.h"
#include "p4info/actions_int.h"
#include "p4info/tables_int.h"
#include "p4info/fields_int.h"
#include "utils/logging.h"
#include "pi_int.h"

#include <cJSON/cJSON.h>
#include <Judy.h>

#include <stdio.h>
#include <assert.h>
#include <string.h>

static pi_status_t read_actions(cJSON *root, pi_p4info_t *p4info) {
  assert(root);
  cJSON *actions = cJSON_GetObjectItem(root, "actions");
  if (!actions) return PI_STATUS_CONFIG_READER_ERROR;
  size_t num_actions = cJSON_GetArraySize(actions);
  pi_p4info_action_init(p4info, num_actions);

  cJSON *action;
  int id = 0;
  cJSON_ArrayForEach(action, actions) {
    const cJSON *item;
    item = cJSON_GetObjectItem(action, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;

    // ignore the JSON id
    /* item = cJSON_GetObjectItem(action, "id"); */
    /* if (!item) return PI_STATUS_CONFIG_READER_ERROR; */
    /* pi_p4_id_t pi_id = item->valueint; */
    pi_p4_id_t pi_id = pi_make_action_id(id++);

    cJSON *params = cJSON_GetObjectItem(action, "runtime_data");
    if (!params) return PI_STATUS_CONFIG_READER_ERROR;
    size_t num_params = cJSON_GetArraySize(params);

    PI_LOG_DEBUG("Adding action '%s'\n", name);
    pi_p4info_action_add(p4info, pi_id, name, num_params);

    int param_id = 0;
    cJSON *param;
    cJSON_ArrayForEach(param, params) {
      item = cJSON_GetObjectItem(param, "name");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      const char *param_name = item->valuestring;

      item = cJSON_GetObjectItem(param, "bitwidth");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      int param_bitwidth = item->valueint;

      pi_p4info_action_add_param(p4info, pi_id,
                                 pi_make_action_param_id(pi_id, param_id++),
                                 param_name, param_bitwidth);
    }
  }

  return PI_STATUS_SUCCESS;
}

static pi_status_t read_fields(cJSON *root, pi_p4info_t *p4info) {
  assert(root);
  cJSON *headers = cJSON_GetObjectItem(root, "headers");
  if (!headers) return PI_STATUS_CONFIG_READER_ERROR;

  cJSON *header_types = cJSON_GetObjectItem(root, "header_types");
  if (!header_types) return PI_STATUS_CONFIG_READER_ERROR;

  Pvoid_t header_type_map = (Pvoid_t) NULL;

  cJSON *item;

  cJSON *header_type;
  cJSON_ArrayForEach(header_type, header_types) {
    item = cJSON_GetObjectItem(header_type, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *name = item->valuestring;
    Word_t *header_type_json;
    JSLI(header_type_json, header_type_map, (const uint8_t *) name);
    *header_type_json = (Word_t) header_type;
  }

  // find out number of fields in the program
  size_t num_fields = 0u;
  cJSON *header;
  cJSON_ArrayForEach(header, headers) {
    item = cJSON_GetObjectItem(header, "header_type");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *header_type_name = item->valuestring;
    Word_t *header_type_json = NULL;
    JSLG(header_type_json, header_type_map, (const uint8_t *) header_type_name);
    if (!header_type_json) return PI_STATUS_CONFIG_READER_ERROR;
    item = (cJSON *) *header_type_json;
    item = cJSON_GetObjectItem(item, "fields");
    num_fields += cJSON_GetArraySize(item);
  }

  PI_LOG_DEBUG("Number of fields found: %zu\n", num_fields);
  pi_p4info_field_init(p4info, num_fields);

  int id = 0;

  cJSON_ArrayForEach(header, headers) {
    item = cJSON_GetObjectItem(header, "name");
    if (!item) return PI_STATUS_CONFIG_READER_ERROR;
    const char *header_name = item->valuestring;
    item = cJSON_GetObjectItem(header, "header_type");
    const char *header_type_name = item->valuestring;
    Word_t *header_type_json = NULL;
    JSLG(header_type_json, header_type_map, (const uint8_t *) header_type_name);
    if (!header_type_json) return PI_STATUS_CONFIG_READER_ERROR;
    item = (cJSON *) *header_type_json;
    item = cJSON_GetObjectItem(item, "fields");
    cJSON *field;
    cJSON_ArrayForEach(field, item) {
      const char *suffix = cJSON_GetArrayItem(field, 0)->valuestring;
      char fname[256];
      int n = snprintf(fname, sizeof(fname), "%s.%s", header_name, suffix);
      if (n <= 0 || (size_t) n >= sizeof(fname)) return PI_STATUS_BUFFER_ERROR;
      size_t bitwidth = (size_t) cJSON_GetArrayItem(field, 1)->valueint;
      PI_LOG_DEBUG("Adding field '%s'\n", fname);
      pi_p4info_field_add(p4info, pi_make_field_id(id++), fname, bitwidth);
    }
    num_fields += cJSON_GetArraySize(item);
  }

  Word_t Rc_word;
  JSLFA(Rc_word, header_type_map);

  return PI_STATUS_SUCCESS;
}

static pi_p4info_match_type_t match_type_from_str(const char *type) {
  if (!strncmp("exact", type, sizeof "exact"))
    return PI_P4INFO_MATCH_TYPE_EXACT;
  if (!strncmp("lpm", type, sizeof "lpm"))
    return PI_P4INFO_MATCH_TYPE_LPM;
  if (!strncmp("ternary", type, sizeof "ternary"))
    return PI_P4INFO_MATCH_TYPE_TERNARY;
  assert("unsupported match type");
  return PI_P4INFO_MATCH_TYPE_EXACT;
}

static pi_status_t read_tables(cJSON *root, pi_p4info_t *p4info) {
  assert(root);
  cJSON *pipelines = cJSON_GetObjectItem(root, "pipelines");
  if (!pipelines) return PI_STATUS_CONFIG_READER_ERROR;

  size_t num_tables = 0u;

  cJSON *pipe;
  cJSON_ArrayForEach(pipe, pipelines) {
    cJSON *tables = cJSON_GetObjectItem(pipe, "tables");
    if (!tables) return PI_STATUS_CONFIG_READER_ERROR;
    num_tables += cJSON_GetArraySize(tables);
  }

  pi_p4info_table_init(p4info, num_tables);

  cJSON *table;
  int id = 0;
  cJSON_ArrayForEach(pipe, pipelines) {
    cJSON *tables = cJSON_GetObjectItem(pipe, "tables");
    cJSON_ArrayForEach(table, tables) {
      const cJSON *item;
      item = cJSON_GetObjectItem(table, "name");
      if (!item) return PI_STATUS_CONFIG_READER_ERROR;
      const char *name = item->valuestring;

      // ignore the JSON id
      /* item = cJSON_GetObjectItem(table, "id"); */
      /* if (!item) return PI_STATUS_CONFIG_READER_ERROR; */
      /* pi_p4_id_t pi_id = item->valueint; */
      pi_p4_id_t pi_id = pi_make_table_id(id++);

      cJSON *json_match_key = cJSON_GetObjectItem(table, "key");
      if (!json_match_key) return PI_STATUS_CONFIG_READER_ERROR;
      size_t num_match_fields = cJSON_GetArraySize(json_match_key);

      cJSON *json_actions = cJSON_GetObjectItem(table, "actions");
      if (!json_actions) return PI_STATUS_CONFIG_READER_ERROR;
      size_t num_actions = cJSON_GetArraySize(json_actions);

      PI_LOG_DEBUG("Adding table '%s'\n", name);
      pi_p4info_table_add(p4info, pi_id, name, num_match_fields, num_actions);

      cJSON *match_field;
      cJSON_ArrayForEach(match_field, json_match_key) {
        item = cJSON_GetObjectItem(match_field, "match_type");
        if (!item) return PI_STATUS_CONFIG_READER_ERROR;
        pi_p4info_match_type_t match_type = match_type_from_str(
            item->valuestring);

        cJSON *target = cJSON_GetObjectItem(match_field, "target");
        if (!target) return PI_STATUS_CONFIG_READER_ERROR;
        char fname[256];
        int n = snprintf(fname, sizeof(fname), "%s.%s",
                         cJSON_GetArrayItem(target, 0)->valuestring,
                         cJSON_GetArrayItem(target, 1)->valuestring);
        if (n <= 0 || (size_t) n >= sizeof(fname))
          return PI_STATUS_BUFFER_ERROR;
        pi_p4_id_t fid = pi_p4info_field_id_from_name(p4info, fname);
        size_t bitwidth = pi_p4info_field_bitwidth(p4info, fid);
        pi_p4info_table_add_match_field(p4info, pi_id, fid, fname,
                                        match_type, bitwidth);
      }
    }
  }

  return PI_STATUS_SUCCESS;
}

pi_status_t pi_bmv2_json_reader(const char *config,  pi_p4info_t *p4info) {
  cJSON *root = cJSON_Parse(config);
  if (!root) return PI_STATUS_CONFIG_READER_ERROR;

  pi_status_t status;

  if ((status = read_actions(root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_fields(root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  if ((status = read_tables(root, p4info)) != PI_STATUS_SUCCESS) {
    return status;
  }

  cJSON_Delete(root);

  return PI_STATUS_SUCCESS;
}
