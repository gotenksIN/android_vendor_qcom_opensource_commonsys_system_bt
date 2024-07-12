/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
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
 ******************************************************************************/

/******************************************************************************
 *
 * Changes from Qualcomm Innovation Center are provided under the following license:
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 *
*****************************************************************************/

#include "osi/include/config.h"

#include <base/files/file_util.h>
#include <base/logging.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <type_traits>

#include "osi/include/allocator.h"
#include "osi/include/list.h"
#include "osi/include/log.h"
#include "osi/include/compat.h"
#include "log/log.h"
#include "bt_target.h"
#include <inttypes.h>

static section_t* section_new(const char* name);
static void entry_free(void* ptr);


// Empty definition; this type is aliased to list_node_t.
void section_t::Set(std::string key, std::string value) {
  for (entry_t& entry : entries) {
    if (entry.key == key) {
      entry.value = value;
      return;
    }
  }
  // add a new key to the section
  entries.emplace_back(
      entry_t{.key = std::move(key), .value = std::move(value)});
}

std::list<entry_t>::iterator section_t::Find(const std::string& key) {
  return std::find_if(
      entries.begin(), entries.end(),
      [&key](const entry_t& entry) { return entry.key == key; });
}

bool section_t::Has(const std::string& key) {
  return Find(key) != entries.end();
}

std::list<section_t>::iterator config_t::Find(const std::string& section) {
  return std::find_if(
      sections.begin(), sections.end(),
      [&section](const section_t& sec) { return sec.name == section; });
}

bool config_t::Has(const std::string& key) {
  return Find(key) != sections.end();
}

static bool config_parse(FILE* fp, config_t* config);

template <typename T,
          class = typename std::enable_if<std::is_same<
              config_t, typename std::remove_const<T>::type>::value>>

static auto section_find(T& config, const std::string& section) {
  return std::find_if(
      config.sections.begin(), config.sections.end(),
      [&section](const section_t& sec) { return sec.name == section; });
}

static const entry_t* entry_find(const config_t& config,
                                 const std::string& section,
                                 const std::string& key) {
  auto sec = section_find(config, section);
  if (sec == config.sections.end()) return nullptr;
  for (const entry_t& entry : sec->entries) {
    if (entry.key == key) return &entry;
  }
  return nullptr;
}

std::unique_ptr<config_t> config_new_empty(void) {
  return std::make_unique<config_t>();
}


std::unique_ptr<config_t> config_new(const char* filename) {
  CHECK(filename != nullptr);

  std::unique_ptr<config_t> config = config_new_empty();

  FILE* fp = fopen(filename, "rt");
  if (!fp) {
    LOG(ERROR) << LOG_TAG <<   ": unable to open file '" << filename
               << "': " << strerror(errno);
   // config_free(config.get());
    return nullptr;
  }

  if (!config_parse(fp, config.get())) {
    config.reset();
    config = NULL;
  }

  fclose(fp);
  return config;
}

std::string checksum_read(const char* filename) {
  base::FilePath path(filename);
  if (!base::PathExists(path)) {
    LOG(ERROR) << __func__ << ": unable to locate file '" << filename << "'";
    return "";
  }
  std::string encrypted_hash;
  if (!base::ReadFileToString(path, &encrypted_hash)) {
    LOG(ERROR) << __func__ << ": unable to read file '" << filename << "'";
  }
  return encrypted_hash;
}


std::unique_ptr<config_t> config_new_clone(const config_t& src) {
  std::unique_ptr<config_t> ret = config_new_empty();

  for (const section_t& sec : src.sections) {
    for (const entry_t& entry : sec.entries) {
      config_set_string(ret.get(), sec.name, entry.key, entry.value);

    }
  }

  return ret;
}


bool config_has_section(const config_t& config, const std::string& section) {
  return (section_find(config, section) != config.sections.end());
}

bool config_has_key(const config_t& config, const std::string& section,
                    const std::string& key) {
  return (entry_find(config, section, key) != nullptr);
}

int config_get_int(const config_t& config, const std::string& section,
                   const std::string& key, int def_value) {
  const entry_t* entry = entry_find(config, section, key);

  if (!entry) return def_value;

  char* endptr;
  int ret = strtol(entry->value.c_str(), &endptr, 0);
  return (*endptr == '\0') ? ret : def_value;
}

unsigned short int config_get_uint16(const config_t& config, const std::string& section, const std::string& key,
                   uint16_t def_value) {

  const entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  char* endptr;
  uint16_t ret = (uint16_t)strtoumax(entry->value.c_str(), &endptr, 0);
  return (*endptr == '\0') ? ret : def_value;
}

uint64_t config_get_uint64(const config_t& config, const std::string& section, const std::string& key,
                   uint64_t def_value) {

  const entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  char* endptr;
  uint64_t ret = (uint64_t)strtoull(entry->value.c_str(), &endptr, 0);
  return (*endptr == '\0') ? ret : def_value;
}

bool config_get_bool(const config_t& config, const std::string& section,
                     const std::string& key, bool def_value) {

  const entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  if (!strcmp(entry->value.c_str(), "true")) return true;
  if (!strcmp(entry->value.c_str(), "false")) return false;

  return def_value;
}

const std::string* config_get_string(const config_t& config, const std::string& section,
                              const std::string& key, const std::string* def_value) {


  const entry_t* entry = entry_find(config, section, key);
  if (!entry) return def_value;

  return &entry->value;
}

void config_set_int(config_t* config, const std::string& section, const std::string& key,
                    int value) {
  config_set_string(config, section, key, std::to_string(value));
}

void config_set_uint16(config_t* config, const std::string& section, const std::string& key,
                    uint16_t value) {

  char value_str[16] = {0};
  snprintf(value_str, sizeof(value_str), "%u", value);
  config_set_string(config, section, key, value_str);
}

void config_set_uint64(config_t* config, const std::string& section, const std::string& key,
                    uint64_t value) {

  char value_str[64] = {0};
  snprintf(value_str, sizeof(value_str), "%" PRIu64, value);
  config_set_string(config, section, key, value_str);
}

void config_set_bool(config_t* config, const std::string& section,
                     const std::string& key, bool value) {

  config_set_string(config, section, key, value ? "true" : "false");
}

void config_set_string(config_t* config, const std::string& section, const std::string& key,
                       const std::string& value) {
  CHECK(config);

  auto sec = section_find(*config, section);
  if (sec == config->sections.end()) {
    config->sections.emplace_back(section_t{.name = section});
    sec = std::prev(config->sections.end());
  }

  std::string value_string = value;
  std::string value_no_newline;
  size_t newline_position = value_string.find("\n");
  if (newline_position != std::string::npos) {
    android_errorWriteLog(0x534e4554, "70808273");
    value_no_newline = value_string.substr(0, newline_position);
  } else {
    value_no_newline = value_string;
  }

  for (entry_t& entry : sec->entries) {
    if (entry.key == key) {
      entry.value = value;
        return;
    }
  }

  sec->entries.emplace_back(entry_t{.key = key, .value = value});
}



bool config_remove_section(config_t* config, const std::string& section) {
  CHECK(config);

  auto sec = section_find(*config, section);
  if (sec == config->sections.end()) return false;

  config->sections.erase(sec);
  return true;
}

bool config_remove_key(config_t* config, const char* section, const char* key) {
  CHECK(config);

  auto sec = section_find(*config, section);
  if (sec == config->sections.end()) return false;


  for (auto entry = sec->entries.begin(); entry != sec->entries.end();
       ++entry) {
    if (entry->key == key) {
      sec->entries.erase(entry);
      return true;
    }
  }
  return false;
}


const config_section_node_t* config_section_begin(const config_t* config) {
  CHECK(config != NULL);
  return (const config_section_node_t*)list_begin(config->sections);
}

const config_section_node_t* config_section_end(const config_t* config) {
  CHECK(config != NULL);
  return (const config_section_node_t*)list_end(config->sections);
}

const config_section_node_t* config_section_next(
    const config_section_node_t* node) {
  CHECK(node != NULL);
  return (const config_section_node_t*)list_next((const list_node_t*)node);
}

const char* config_section_name(const config_section_node_t* node) {
  CHECK(node != NULL);
  const list_node_t* lnode = (const list_node_t*)node;
  const section_t* section = (const section_t*)list_node(lnode);
  return section->name.c_str();
}

section_t* config_section(const config_section_node_t* node) {
  CHECK(node != NULL);
  const list_node_t* lnode = (const list_node_t*)node;
  section_t* section = (section_t*)list_node(lnode);
  return section;
}

bool config_remove_section_optimal(config_t* config,section_t* section) {
  CHECK(config != NULL);
  CHECK(section != NULL);

  return list_remove(config->sections, section);
}

bool section_has_key(const section_t* section,
                    const char* key) {
  CHECK(section != NULL);
  CHECK(key != NULL);

  for (const list_node_t* node = list_begin(section->entries);
       node != list_end(section->entries); node = list_next(node)) {
    entry_t* entry = static_cast<entry_t*>(list_node(node));
    if (!strcmp(entry->key.c_str(), key)) return true;
  }

  return false;
}

#if (BT_IOT_LOGGING_ENABLED == TRUE)
void config_sections_sort_by_entry_key(config_t* config, compare_func comp) {
  LOG(INFO) << __func__;
  CHECK(config != NULL);

  for (list_node_t* node = list_begin(config->sections);
      node != list_end(config->sections);
      node = list_next(node)) {
    section_t* sec = (section_t*)list_node(node);
    if (list_length(sec->entries) <= 1)
      continue;
    list_node_t* p = list_end(sec->entries);
    list_node_t* head_next = list_next(list_begin(sec->entries));
    bool changed = true;

    while (p != head_next && changed) {
      list_node_t* q = list_begin(sec->entries);
      changed = false;
      for (;list_next(q) && list_next(q) != p; q = list_next(q)) {
        entry_t* first = (entry_t*)list_node(q);
        entry_t* second = (entry_t*)list_node(list_next(q));
        std::string tmp_key;
        std::string tmp_value;
        if (comp(first->key.c_str(), second->key.c_str()) > 0) {
          tmp_key = first->key;
          tmp_value = first->value;
          first->key = second->key;
          first->value = second->value;
          second->key = tmp_key;
          second->value = tmp_value;
          changed = true;
        }
      }
      p = q;
    }

  }
}
#endif

bool config_save(const config_t& config, const char* filename) {
  //CHECK(config != NULL);
 // CHECK(filename != NULL);
 // CHECK(*filename != '\0');
  CHECK(filename != nullptr);
  // Steps to ensure content of config file gets to disk:
  //
  // 1) Open and write to temp file (e.g. bt_config.conf.new).
  // 2) Sync the temp file to disk with fsync().
  // 3) Rename temp file to actual config file (e.g. bt_config.conf).
  //    This ensures atomic update.
  // 4) Sync directory that has the conf file with fsync().
  //    This ensures directory entries are up-to-date.
  int dir_fd = -1;
  FILE* fp = nullptr;

  // Build temp config file based on config file (e.g. bt_config.conf.new).
  static const char* temp_file_ext = ".new";
  const int filename_len = strlen(filename);
  const int temp_filename_len = filename_len + strlen(temp_file_ext) + 1;
  char* temp_filename = static_cast<char*>(osi_calloc(temp_filename_len));
  snprintf(temp_filename, temp_filename_len, "%s%s", filename, temp_file_ext);

  // Extract directory from file path (e.g. /data/misc/bluedroid).
  char* temp_dirname = osi_strdup(filename);
  const char* directoryname = dirname(temp_dirname);
  if (!directoryname) {
    LOG_ERROR(LOG_TAG, "%s error extracting directory from '%s': %s", __func__,
              filename, strerror(errno));
    goto error;
  }

  dir_fd = open(directoryname, O_RDONLY);
  if (dir_fd < 0) {
    LOG_ERROR(LOG_TAG, "%s unable to open dir '%s': %s", __func__,
              directoryname, strerror(errno));
    goto error;
  }

  fp = fopen(temp_filename, "wt");
  if (!fp) {
    LOG_ERROR(LOG_TAG, "%s unable to write file '%s': %s", __func__,
              temp_filename, strerror(errno));
    goto error;
  }



  for (const section_t& section : config.sections) {
    if (fprintf(fp, "[%s]\n", section.name.c_str()) < 0) {
      LOG(ERROR) << __func__ << ": unable to write to file '" << temp_filename
                 << "': " << strerror(errno);
      goto error;
    }

    for (const entry_t& entry : section.entries) {
      if (fprintf(fp, "%s = %s\n", entry.key.c_str(), entry.value.c_str()) <
          0) {
        LOG(ERROR) << __func__ << ": unable to write to file '" << temp_filename
                   << "': " << strerror(errno);
        goto error;
      }
    }


    if (fputc('\n', fp) == EOF) {
      LOG(ERROR) << __func__ << ": unable to write to file '" << temp_filename
                 << "': " << strerror(errno);
      goto error;
    }

  }

  // Sync written temp file out to disk. fsync() is blocking until data makes it
  // to disk.
  if (fsync(fileno(fp)) < 0) {
    LOG_WARN(LOG_TAG, "%s unable to fsync file '%s': %s", __func__,
             temp_filename, strerror(errno));
  }

  if (fclose(fp) == EOF) {
    LOG_ERROR(LOG_TAG, "%s unable to close file '%s': %s", __func__,
              temp_filename, strerror(errno));
    goto error;
  }
  fp = NULL;

  // Change the file's permissions to Read/Write by User and Group
  if (chmod(temp_filename, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) == -1) {
    LOG_ERROR(LOG_TAG, "%s unable to change file permissions '%s': %s",
              __func__, filename, strerror(errno));
    goto error;
  }

  // Rename written temp file to the actual config file.
  if (rename(temp_filename, filename) == -1) {
    LOG_ERROR(LOG_TAG, "%s unable to commit file '%s': %s", __func__, filename,
              strerror(errno));
    goto error;
  }

  // This should ensure the directory is updated as well.
  if (fsync(dir_fd) < 0) {
    LOG_WARN(LOG_TAG, "%s unable to fsync dir '%s': %s", __func__,
             directoryname, strerror(errno));
  }

  if (syncfs(dir_fd) < 0) {
    LOG_WARN(LOG_TAG, "%s unable to syncfs dir '%s': %s", __func__,
             directoryname, strerror(errno));
  }

  if (close(dir_fd) < 0) {
    LOG_ERROR(LOG_TAG, "%s unable to close dir '%s': %s", __func__,
              directoryname, strerror(errno));
    goto error;
  }
  osi_free(temp_filename);
  osi_free(temp_dirname);
  return true;

error:
  // This indicates there is a write issue.  Unlink as partial data is not
  // acceptable.
  unlink(temp_filename);
  if (fp) fclose(fp);
  if (dir_fd != -1) close(dir_fd);
  osi_free(temp_filename);
  osi_free(temp_dirname);
  return false;
}

bool checksum_save(const std::string& checksum, const std::string& filename) {
  CHECK(!checksum.empty()) << __func__ << ": checksum cannot be empty";
  CHECK(!filename.empty()) << __func__ << ": filename cannot be empty";

  // Steps to ensure content of config checksum file gets to disk:
  //
  // 1) Open and write to temp file (e.g.
  // bt_config.conf.encrypted-checksum.new). 2) Sync the temp file to disk with
  // fsync(). 3) Rename temp file to actual config checksum file (e.g.
  // bt_config.conf.encrypted-checksum).
  //    This ensures atomic update.
  // 4) Sync directory that has the conf file with fsync().
  //    This ensures directory entries are up-to-date.
  FILE* fp = nullptr;
  int dir_fd = -1;

  // Build temp config checksum file based on config checksum file (e.g.
  // bt_config.conf.encrypted-checksum.new).
  const std::string temp_filename = filename + ".new";
  base::FilePath path(temp_filename);

  // Extract directory from file path (e.g. /data/misc/bluedroid).
  const std::string directoryname = base::FilePath(filename).DirName().value();
  if (directoryname.empty()) {
    LOG(ERROR) << __func__ << ": error extracting directory from '" << filename
               << "': " << strerror(errno);
    goto error2;
  }

  dir_fd = open(directoryname.c_str(), O_RDONLY);
  if (dir_fd < 0) {
    LOG(ERROR) << __func__ << ": unable to open dir '" << directoryname
               << "': " << strerror(errno);
    goto error2;
  }

  if (base::WriteFile(path, checksum.data(), checksum.size()) !=
      (int)checksum.size()) {
    LOG(ERROR) << __func__ << ": unable to write file '" << filename.c_str();
    goto error2;
  }

  fp = fopen(temp_filename.c_str(), "rb");
  if (!fp) {
    LOG(ERROR) << __func__ << ": unable to write to file '" << temp_filename
               << "': " << strerror(errno);
    goto error2;
  }

  // Sync written temp file out to disk. fsync() is blocking until data makes it
  // to disk.
  if (fsync(fileno(fp)) < 0) {
    LOG(WARNING) << __func__ << ": unable to fsync file '" << temp_filename
                 << "': " << strerror(errno);
  }

  if (fclose(fp) == EOF) {
    LOG(ERROR) << __func__ << ": unable to close file '" << temp_filename
               << "': " << strerror(errno);
    goto error2;
  }
  fp = nullptr;

  // Change the file's permissions to Read/Write by User and Group
  if (chmod(temp_filename.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) ==
      -1) {
    LOG(ERROR) << __func__ << ": unable to change file permissions '"
               << filename << "': " << strerror(errno);
    goto error2;
  }

  // Rename written temp file to the actual config file.
  if (rename(temp_filename.c_str(), filename.c_str()) == -1) {
    LOG(ERROR) << __func__ << ": unable to commit file '" << filename
               << "': " << strerror(errno);
    goto error2;
  }

  // This should ensure the directory is updated as well.
  if (fsync(dir_fd) < 0) {
    LOG(WARNING) << __func__ << ": unable to fsync dir '" << directoryname
                 << "': " << strerror(errno);
  }

  if (close(dir_fd) < 0) {
    LOG(ERROR) << __func__ << ": unable to close dir '" << directoryname
               << "': " << strerror(errno);
    goto error2;
  }

  return true;

error2:
  // This indicates there is a write issue.  Unlink as partial data is not
  // acceptable.
  unlink(temp_filename.c_str());
  if (fp) fclose(fp);
  if (dir_fd != -1) close(dir_fd);
  return false;
}

static char* trim(char* str) {
  while (isspace(*str)) ++str;

  if (!*str) return str;

  char* end_str = str + strlen(str) - 1;
  while (end_str > str && isspace(*end_str)) --end_str;

  end_str[1] = '\0';
  return str;
}

static bool config_parse(FILE* fp, config_t* config) {
  CHECK(fp != nullptr);
  CHECK(config != nullptr);

  int line_num = 0;
  char line[1024] = { '\0' };
  std::string line_new;
  uint16_t MAX_BUF = 1023;
  char section[1024] = { '\0' };
  char comment[1024] = { '\0' };
  bool skip_entries = false;
  strcpy(section, CONFIG_DEFAULT_SECTION);

  while (fgets(line, sizeof(line), fp)) {
    char* line_ptr = trim(line);

    /* fgets stops when either sizeof(line) = MAX_BUF (buffer_length - 1)character
     * are read, the newline character is read, or the end-of-file is reached,
     * whichever comes first.
     * Hence check if sizeof(line) = MAX_BUF and the last character read is not an
     * EOF and/or not '\n' then  Read remaining characters of a line.
     */

    if ((strlen(line) == MAX_BUF) && (line[MAX_BUF - 1] != EOF) &&
        (line[MAX_BUF - 1] != '\n')) {
      line_new = line_ptr;
      while (fgets(line, sizeof(line), fp)) {
        line_new.append(line);

        //stop reading if '\n' or EOF is reached
        if (strlen(line) == 0 || line[strlen(line) - 1] == '\n' ||
            line[strlen(line) - 1] == EOF)
          break;
      }
      line_ptr = &line_new[0];
      line_ptr = trim(line_ptr);
    }
    ++line_num;

    // Skip blanks.
    if (*line_ptr == '\0')
      continue;

    if (*line_ptr == '#') {
        continue;
    /*
        strlcpy(comment, line_ptr, 1024);

        auto sec = section_find(*config, section);
        if (sec == config->sections.end()) {
            section_t *sec_t = section_new(comment);
            if (sec_t)
                list_append(config->sections, sec_t);
        }
*/
    } else if (*line_ptr == '[') {
      size_t len = strlen(line_ptr);
      if (line_ptr[len - 1] != ']') {
        LOG_DEBUG(LOG_TAG, "%s unterminated section name on line %d.", __func__, line_num);
        skip_entries = true;
        continue;
      }
      strncpy(section, line_ptr + 1, len - 2);
      section[len - 2] = '\0';
      skip_entries = false;
    } else {
      char *split = strchr(line_ptr, '=');
      if(skip_entries) {
        LOG_DEBUG(LOG_TAG, "%s skip entries due invalid section line %d.", __func__, line_num);
        continue;
      }
      if (!split) {
        LOG_DEBUG(LOG_TAG, "%s no key/value separator found on line %d.", __func__, line_num);
        continue;
      }

      *split = '\0';
      config_set_string(config, section, trim(line_ptr), trim(split + 1));
    }
  }
  return true;
}

/*
static entry_t* entry_new(const char* key, const char* value) {
  entry_t* entry = static_cast<entry_t*>(osi_calloc(sizeof(entry_t)));

  entry->key = osi_strdup(key);
  entry->value = osi_strdup(value);
  return entry;
}
*/


