/*
 * VPK extraction and promotion follows VitaShell's package installer.
 * VitaShell is Copyright (C) 2015-2018 TheFloW, GPL-3.0.
 */

#include "package_installer.h"
#include "sha1.h"
#include "vita_shell_head_bin.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define INSTALL_STAGE "ux0:data/vitacompanion-package"
#define INSTALL_HEAD INSTALL_STAGE "/sce_sys/package/head.bin"
#define INSTALL_SFO INSTALL_STAGE "/sce_sys/param.sfo"
#define INSTALL_PATH_MAX 1024
#define INSTALL_IO_BUFFER (64 * 1024)
#define INSTALL_SFO_MAX (1024 * 1024)
#define INSTALL_TOTAL_MAX ((uint64_t)4 * 1024 * 1024 * 1024)

enum {
  INSTALL_ERROR_ARGUMENT = -0x7101,
  INSTALL_ERROR_ARCHIVE = -0x7102,
  INSTALL_ERROR_ARCHIVE_PATH = -0x7103,
  INSTALL_ERROR_ARCHIVE_TYPE = -0x7104,
  INSTALL_ERROR_ARCHIVE_SIZE = -0x7105,
  INSTALL_ERROR_SFO = -0x7106,
  INSTALL_ERROR_TITLE_ID = -0x7107,
  INSTALL_ERROR_MEMORY = -0x7108,
  INSTALL_ERROR_IO = -0x7109
};

typedef struct __attribute__((packed)) SfoHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t key_offset;
  uint32_t value_offset;
  uint32_t count;
} SfoHeader;

typedef struct __attribute__((packed)) SfoEntry {
  uint16_t name_offset;
  uint8_t alignment;
  uint8_t type;
  uint32_t value_size;
  uint32_t total_size;
  uint32_t data_offset;
} SfoEntry;

static int remove_tree(const char *path)
{
  SceIoStat stat;
  memset(&stat, 0, sizeof(stat));
  int result = sceIoGetstat(path, &stat);
  if (result < 0)
    return 0;
  if (!SCE_S_ISDIR(stat.st_mode))
    return sceIoRemove(path);

  SceUID directory = sceIoDopen(path);
  if (directory < 0)
    return directory;
  SceIoDirent entry;
  while (1) {
    memset(&entry, 0, sizeof(entry));
    result = sceIoDread(directory, &entry);
    if (result <= 0)
      break;
    if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
      continue;
    char child[INSTALL_PATH_MAX];
    int length = snprintf(child, sizeof(child), "%s/%s", path, entry.d_name);
    if (length < 0 || (size_t)length >= sizeof(child)) {
      result = INSTALL_ERROR_ARCHIVE_PATH;
      break;
    }
    result = remove_tree(child);
    if (result < 0)
      break;
  }
  sceIoDclose(directory);
  if (result < 0)
    return result;
  return sceIoRmdir(path);
}

static int ensure_directory(const char *path)
{
  char current[INSTALL_PATH_MAX];
  size_t length = strlen(path);
  if (length == 0 || length >= sizeof(current))
    return INSTALL_ERROR_ARCHIVE_PATH;
  memcpy(current, path, length + 1);

  for (char *cursor = strchr(current, ':'); cursor != NULL && *cursor; cursor++) {
    if (*cursor != '/')
      continue;
    *cursor = '\0';
    if (cursor > current && cursor[-1] != ':') {
      int result = sceIoMkdir(current, 0777);
      if (result < 0) {
        SceIoStat stat;
        memset(&stat, 0, sizeof(stat));
        if (sceIoGetstat(current, &stat) < 0 || !SCE_S_ISDIR(stat.st_mode)) {
          *cursor = '/';
          return result;
        }
      }
    }
    *cursor = '/';
  }

  int result = sceIoMkdir(current, 0777);
  if (result < 0) {
    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    if (sceIoGetstat(current, &stat) < 0 || !SCE_S_ISDIR(stat.st_mode))
      return result;
  }
  return 0;
}

static int ensure_parent_directory(const char *path)
{
  char parent[INSTALL_PATH_MAX];
  size_t length = strlen(path);
  if (length == 0 || length >= sizeof(parent))
    return INSTALL_ERROR_ARCHIVE_PATH;
  memcpy(parent, path, length + 1);
  char *slash = strrchr(parent, '/');
  if (slash == NULL)
    return INSTALL_ERROR_ARCHIVE_PATH;
  *slash = '\0';
  return ensure_directory(parent);
}

static int is_safe_archive_path(const char *path)
{
  if (path == NULL || path[0] == '\0' || path[0] == '/' || path[0] == '\\' ||
      strchr(path, ':') != NULL || strchr(path, '\\') != NULL)
    return 0;

  const char *component = path;
  while (*component != '\0') {
    const char *slash = strchr(component, '/');
    const char *end = slash != NULL ? slash : component + strlen(component);
    size_t length = (size_t)(end - component);
    if (length == 0 || (length == 1 && component[0] == '.') ||
        (length == 2 && component[0] == '.' && component[1] == '.'))
      return 0;
    if (slash == NULL || slash[1] == '\0')
      return 1;
    component = slash + 1;
  }
  return 1;
}

static uint16_t read_le16(const uint8_t *data)
{
  return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static size_t bounded_string_length(const char *value, size_t limit)
{
  size_t length = 0;
  while (length < limit && value[length] != '\0')
    length++;
  return length;
}

static uint32_t read_le32(const uint8_t *data)
{
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static int read_at(SceUID file, SceOff offset, void *buffer, size_t size)
{
  if (sceIoLseek(file, offset, SCE_SEEK_SET) < 0)
    return INSTALL_ERROR_IO;
  size_t done = 0;
  while (done < size) {
    int read = sceIoRead(file, (uint8_t *)buffer + done, size - done);
    if (read <= 0)
      return read < 0 ? read : INSTALL_ERROR_IO;
    done += (size_t)read;
  }
  return 0;
}

static int write_all(SceUID file, const void *buffer, size_t size)
{
  size_t done = 0;
  while (done < size) {
    int written = sceIoWrite(file, (const uint8_t *)buffer + done, size - done);
    if (written <= 0)
      return written < 0 ? written : INSTALL_ERROR_IO;
    done += (size_t)written;
  }
  return 0;
}

static int extract_zip_data(SceUID input, SceOff data_offset,
                            uint32_t compressed_size,
                            uint32_t uncompressed_size, uint16_t method,
                            uint32_t expected_crc, const char *destination)
{
  int result = ensure_parent_directory(destination);
  if (result < 0)
    return result;
  SceUID output = sceIoOpen(destination,
      SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (output < 0)
    return output;
  if (sceIoLseek(input, data_offset, SCE_SEEK_SET) < 0) {
    sceIoClose(output);
    sceIoRemove(destination);
    return INSTALL_ERROR_IO;
  }

  uint8_t *input_buffer = malloc(INSTALL_IO_BUFFER);
  uint8_t *output_buffer = malloc(INSTALL_IO_BUFFER);
  if (input_buffer == NULL || output_buffer == NULL) {
    free(input_buffer);
    free(output_buffer);
    sceIoClose(output);
    sceIoRemove(destination);
    return INSTALL_ERROR_MEMORY;
  }

  uint32_t checksum = crc32(0L, Z_NULL, 0);
  uint32_t compressed_left = compressed_size;
  uint32_t produced_total = 0;
  if (method == 0) {
    if (compressed_size != uncompressed_size) {
      result = INSTALL_ERROR_ARCHIVE_SIZE;
    } else {
      while (compressed_left > 0) {
        size_t chunk_size = compressed_left < INSTALL_IO_BUFFER ?
                            compressed_left : INSTALL_IO_BUFFER;
        int read = sceIoRead(input, input_buffer, chunk_size);
        if (read != (int)chunk_size) {
          result = read < 0 ? read : INSTALL_ERROR_IO;
          break;
        }
        checksum = crc32(checksum, input_buffer, chunk_size);
        result = write_all(output, input_buffer, chunk_size);
        if (result < 0)
          break;
        compressed_left -= (uint32_t)chunk_size;
        produced_total += (uint32_t)chunk_size;
      }
    }
  } else if (method == 8) {
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    int zresult = inflateInit2(&stream, -MAX_WBITS);
    if (zresult != Z_OK) {
      result = INSTALL_ERROR_ARCHIVE;
    } else {
      int finished = 0;
      while (!finished) {
        if (stream.avail_in == 0 && compressed_left > 0) {
          size_t chunk_size = compressed_left < INSTALL_IO_BUFFER ?
                              compressed_left : INSTALL_IO_BUFFER;
          int read = sceIoRead(input, input_buffer, chunk_size);
          if (read != (int)chunk_size) {
            result = read < 0 ? read : INSTALL_ERROR_IO;
            break;
          }
          stream.next_in = input_buffer;
          stream.avail_in = chunk_size;
          compressed_left -= (uint32_t)chunk_size;
        }
        stream.next_out = output_buffer;
        stream.avail_out = INSTALL_IO_BUFFER;
        zresult = inflate(&stream, Z_NO_FLUSH);
        size_t produced = INSTALL_IO_BUFFER - stream.avail_out;
        if (produced > 0) {
          checksum = crc32(checksum, output_buffer, produced);
          result = write_all(output, output_buffer, produced);
          if (result < 0)
            break;
          produced_total += (uint32_t)produced;
        }
        if (zresult == Z_STREAM_END) {
          finished = 1;
        } else if (zresult != Z_OK ||
                   (produced == 0 && stream.avail_in == 0 && compressed_left == 0)) {
          result = INSTALL_ERROR_ARCHIVE;
          break;
        }
      }
      inflateEnd(&stream);
    }
  } else {
    result = INSTALL_ERROR_ARCHIVE_TYPE;
  }

  free(input_buffer);
  free(output_buffer);
  sceIoClose(output);
  if (result >= 0 &&
      (produced_total != uncompressed_size || checksum != expected_crc))
    result = INSTALL_ERROR_ARCHIVE;
  if (result < 0)
    sceIoRemove(destination);
  return result;
}

static int find_central_directory(SceUID file, SceOff file_size,
                                  uint32_t *offset, uint16_t *entry_count)
{
  size_t tail_size = file_size < 0x10016 ? (size_t)file_size : 0x10016;
  uint8_t *tail = malloc(tail_size);
  if (tail == NULL)
    return INSTALL_ERROR_MEMORY;
  int result = read_at(file, file_size - tail_size, tail, tail_size);
  if (result < 0) {
    free(tail);
    return result;
  }
  result = INSTALL_ERROR_ARCHIVE;
  for (size_t index = tail_size - 22;; index--) {
    if (read_le32(tail + index) == 0x06054B50) {
      uint16_t disk = read_le16(tail + index + 4);
      uint16_t central_disk = read_le16(tail + index + 6);
      uint16_t entries_on_disk = read_le16(tail + index + 8);
      uint16_t entries = read_le16(tail + index + 10);
      uint32_t central_size = read_le32(tail + index + 12);
      uint32_t central_offset = read_le32(tail + index + 16);
      if (disk == 0 && central_disk == 0 && entries == entries_on_disk &&
          entries != 0xFFFF &&
          (uint64_t)central_offset + central_size <= (uint64_t)file_size) {
        *offset = central_offset;
        *entry_count = entries;
        result = 0;
      }
      break;
    }
    if (index == 0)
      break;
  }
  free(tail);
  return result;
}

static int extract_vpk(const char *vpk_path)
{
  SceUID file = sceIoOpen(vpk_path, SCE_O_RDONLY, 0);
  if (file < 0)
    return file;
  SceOff file_size = sceIoLseek(file, 0, SCE_SEEK_END);
  if (file_size < 22) {
    sceIoClose(file);
    return INSTALL_ERROR_ARCHIVE;
  }

  uint32_t central_offset = 0;
  uint16_t entry_count = 0;
  int result = find_central_directory(file, file_size, &central_offset,
                                      &entry_count);
  uint64_t total_size = 0;
  SceOff cursor = central_offset;
  for (uint16_t index = 0; result >= 0 && index < entry_count; index++) {
    uint8_t central[46];
    result = read_at(file, cursor, central, sizeof(central));
    if (result < 0 || read_le32(central) != 0x02014B50) {
      result = INSTALL_ERROR_ARCHIVE;
      break;
    }
    uint16_t flags = read_le16(central + 8);
    uint16_t method = read_le16(central + 10);
    uint32_t expected_crc = read_le32(central + 16);
    uint32_t compressed_size = read_le32(central + 20);
    uint32_t uncompressed_size = read_le32(central + 24);
    uint16_t name_size = read_le16(central + 28);
    uint16_t extra_size = read_le16(central + 30);
    uint16_t comment_size = read_le16(central + 32);
    uint16_t disk = read_le16(central + 34);
    uint32_t attributes = read_le32(central + 38);
    uint32_t local_offset = read_le32(central + 42);
    SceOff next = cursor + sizeof(central) + name_size + extra_size + comment_size;
    if ((flags & 1) || disk != 0 || name_size == 0 ||
        name_size >= INSTALL_PATH_MAX / 2 || compressed_size == 0xFFFFFFFF ||
        uncompressed_size == 0xFFFFFFFF || local_offset == 0xFFFFFFFF ||
        (method != 0 && method != 8) || next > file_size) {
      result = INSTALL_ERROR_ARCHIVE;
      break;
    }

    char entry_path[INSTALL_PATH_MAX / 2];
    result = read_at(file, cursor + sizeof(central), entry_path, name_size);
    if (result < 0)
      break;
    entry_path[name_size] = '\0';
    if (!is_safe_archive_path(entry_path)) {
      result = INSTALL_ERROR_ARCHIVE_PATH;
      break;
    }
    uint32_t unix_type = (attributes >> 16) & 0170000;
    int is_directory = entry_path[name_size - 1] == '/';
    if (unix_type != 0 && unix_type != 0100000 && unix_type != 0040000) {
      result = INSTALL_ERROR_ARCHIVE_TYPE;
      break;
    }
    if (uncompressed_size > INSTALL_TOTAL_MAX - total_size) {
      result = INSTALL_ERROR_ARCHIVE_SIZE;
      break;
    }
    total_size += uncompressed_size;

    char destination[INSTALL_PATH_MAX];
    int length = snprintf(destination, sizeof(destination), "%s/%s",
                          INSTALL_STAGE, entry_path);
    if (length < 0 || (size_t)length >= sizeof(destination)) {
      result = INSTALL_ERROR_ARCHIVE_PATH;
      break;
    }
    if (is_directory) {
      destination[length - 1] = '\0';
      result = ensure_directory(destination);
    } else {
      uint8_t local[30];
      result = read_at(file, local_offset, local, sizeof(local));
      if (result < 0 || read_le32(local) != 0x04034B50 ||
          read_le16(local + 8) != method) {
        result = INSTALL_ERROR_ARCHIVE;
        break;
      }
      uint16_t local_name_size = read_le16(local + 26);
      uint16_t local_extra_size = read_le16(local + 28);
      uint64_t data_offset = (uint64_t)local_offset + sizeof(local) +
                             local_name_size + local_extra_size;
      if (data_offset + compressed_size > (uint64_t)file_size) {
        result = INSTALL_ERROR_ARCHIVE;
        break;
      }
      result = extract_zip_data(file, data_offset, compressed_size,
          uncompressed_size, method, expected_crc, destination);
    }
    cursor = next;
  }
  sceIoClose(file);
  return result;
}

static int read_file(const char *path, void **buffer_out, size_t *size_out)
{
  SceUID file = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (file < 0)
    return file;
  SceOff end = sceIoLseek(file, 0, SCE_SEEK_END);
  if (end <= 0 || end > INSTALL_SFO_MAX) {
    sceIoClose(file);
    return INSTALL_ERROR_SFO;
  }
  if (sceIoLseek(file, 0, SCE_SEEK_SET) < 0) {
    sceIoClose(file);
    return INSTALL_ERROR_IO;
  }
  void *buffer = malloc((size_t)end);
  if (buffer == NULL) {
    sceIoClose(file);
    return INSTALL_ERROR_MEMORY;
  }
  size_t offset = 0;
  while (offset < (size_t)end) {
    int read = sceIoRead(file, (char *)buffer + offset, (size_t)end - offset);
    if (read <= 0) {
      free(buffer);
      sceIoClose(file);
      return read < 0 ? read : INSTALL_ERROR_IO;
    }
    offset += (size_t)read;
  }
  sceIoClose(file);
  *buffer_out = buffer;
  *size_out = (size_t)end;
  return 0;
}

static int sfo_string(const void *buffer, size_t size, const char *key,
                      char *value, size_t value_size)
{
  if (size < sizeof(SfoHeader) || value_size == 0)
    return INSTALL_ERROR_SFO;
  const uint8_t *bytes = (const uint8_t *)buffer;
  const SfoHeader *header = (const SfoHeader *)buffer;
  if (header->magic != 0x46535000 ||
      header->count > (size - sizeof(SfoHeader)) / sizeof(SfoEntry))
    return INSTALL_ERROR_SFO;
  const SfoEntry *entries = (const SfoEntry *)(bytes + sizeof(SfoHeader));

  for (uint32_t index = 0; index < header->count; index++) {
    uint64_t key_offset = (uint64_t)header->key_offset + entries[index].name_offset;
    uint64_t data_offset = (uint64_t)header->value_offset + entries[index].data_offset;
    if (key_offset >= size || data_offset >= size || entries[index].value_size == 0 ||
        entries[index].value_size > size - data_offset)
      return INSTALL_ERROR_SFO;
    size_t key_space = size - (size_t)key_offset;
    size_t data_space = entries[index].value_size;
    if (memchr(bytes + key_offset, '\0', key_space) == NULL ||
        memchr(bytes + data_offset, '\0', data_space) == NULL)
      return INSTALL_ERROR_SFO;
    if (strcmp((const char *)bytes + key_offset, key) != 0)
      continue;
    size_t length = bounded_string_length(
        (const char *)bytes + data_offset, data_space);
    if (length >= value_size)
      length = value_size - 1;
    memcpy(value, bytes + data_offset, length);
    value[length] = '\0';
    return 0;
  }
  return INSTALL_ERROR_SFO;
}

static int valid_title_id(const char *title_id)
{
  if (strlen(title_id) != 9)
    return 0;
  for (size_t index = 0; index < 9; index++) {
    char value = title_id[index];
    if (!((value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')))
      return 0;
  }
  return 1;
}

static void fpkg_hmac(const uint8_t *data, size_t length, uint8_t hmac[16])
{
  uint8_t sha1[SHA1_BLOCK_SIZE];
  uint8_t buffer[64];
  SHA1_CTX context;
  sha1_init(&context);
  sha1_update(&context, data, length);
  sha1_final(&context, sha1);
  memset(buffer, 0, sizeof(buffer));
  memcpy(buffer, sha1 + 4, 8);
  memcpy(buffer + 8, sha1 + 4, 8);
  memcpy(buffer + 16, sha1 + 12, 4);
  buffer[20] = sha1[16];
  buffer[21] = sha1[1];
  buffer[22] = sha1[2];
  buffer[23] = sha1[3];
  memcpy(buffer + 24, buffer + 16, 8);
  sha1_init(&context);
  sha1_update(&context, buffer, sizeof(buffer));
  sha1_final(&context, sha1);
  memcpy(hmac, sha1, 16);
}

static uint32_t read_be32(const uint8_t *data)
{
  uint32_t value;
  memcpy(&value, data, sizeof(value));
  return __builtin_bswap32(value);
}

static int make_head_bin(char *title_id, size_t title_id_size)
{
  void *sfo = NULL;
  size_t sfo_size = 0;
  int result = read_file(INSTALL_SFO, &sfo, &sfo_size);
  if (result < 0)
    return result;

  char parsed_title_id[12] = {0};
  char content_id[48] = {0};
  result = sfo_string(sfo, sfo_size, "TITLE_ID", parsed_title_id,
                      sizeof(parsed_title_id));
  if (result >= 0 && !valid_title_id(parsed_title_id))
    result = INSTALL_ERROR_TITLE_ID;
  if (result >= 0)
    sfo_string(sfo, sfo_size, "CONTENT_ID", content_id, sizeof(content_id));
  free(sfo);
  if (result < 0)
    return result;

  uint8_t *head = malloc(vita_shell_head_bin_len);
  if (head == NULL)
    return INSTALL_ERROR_MEMORY;
  memcpy(head, vita_shell_head_bin, vita_shell_head_bin_len);

  char default_content_id[48] = {0};
  snprintf(default_content_id, sizeof(default_content_id),
           "EP9000-%s_00-0000000000000000", parsed_title_id);
  const char *selected_content_id = content_id[0] ? content_id : default_content_id;
  memset(head + 0x30, 0, 48);
  size_t content_length = bounded_string_length(selected_content_id, 48);
  memcpy(head + 0x30, selected_content_id, content_length);

  uint8_t hmac[16];
  uint32_t length = read_be32(head + 0xD0);
  if ((uint64_t)length + 16 > vita_shell_head_bin_len) {
    free(head);
    return INSTALL_ERROR_SFO;
  }
  fpkg_hmac(head, length, hmac);
  memcpy(head + length, hmac, 16);

  uint32_t offset = read_be32(head + 0x8);
  length = read_be32(head + 0x10);
  uint32_t output = read_be32(head + 0xD4);
  if (length < 64 || (uint64_t)offset + length > vita_shell_head_bin_len ||
      (uint64_t)output + 16 > vita_shell_head_bin_len) {
    free(head);
    return INSTALL_ERROR_SFO;
  }
  fpkg_hmac(head + offset, length - 64, hmac);
  memcpy(head + output, hmac, 16);

  length = read_be32(head + 0xE8);
  if ((uint64_t)length + 16 > vita_shell_head_bin_len) {
    free(head);
    return INSTALL_ERROR_SFO;
  }
  fpkg_hmac(head, length, hmac);
  memcpy(head + length, hmac, 16);

  result = ensure_parent_directory(INSTALL_HEAD);
  if (result >= 0) {
    SceUID output_file = sceIoOpen(INSTALL_HEAD,
        SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (output_file < 0) {
      result = output_file;
    } else {
      int written = sceIoWrite(output_file, head, vita_shell_head_bin_len);
      sceIoClose(output_file);
      if (written != (int)vita_shell_head_bin_len)
        result = written < 0 ? written : INSTALL_ERROR_IO;
    }
  }
  free(head);
  if (result < 0)
    return result;
  snprintf(title_id, title_id_size, "%s", parsed_title_id);
  return 0;
}

static int load_paf(void)
{
  static uint32_t arguments[] = {0x180000, -1, -1, 1, -1, -1};
  int result = -1;
  uint32_t options[4] = {sizeof(options), (uint32_t)&result, -1, -1};
  return sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF,
      sizeof(arguments), arguments, (const SceSysmoduleOpt *)options);
}

static int unload_paf(void)
{
  uint32_t options = 0;
  return sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF,
      0, NULL, (const SceSysmoduleOpt *)&options);
}

static int promote_stage(void)
{
  int result = load_paf();
  if (result < 0)
    return result;
  result = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (result < 0) {
    unload_paf();
    return result;
  }
  result = scePromoterUtilityInit();
  if (result >= 0) {
    result = scePromoterUtilityPromotePkgWithRif(INSTALL_STAGE, 1);
    int exit_result = scePromoterUtilityExit();
    if (result >= 0 && exit_result < 0)
      result = exit_result;
  }
  int unload_result =
      sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
  if (result >= 0 && unload_result < 0)
    result = unload_result;
  unload_result = unload_paf();
  if (result >= 0 && unload_result < 0)
    result = unload_result;
  return result;
}

int install_vpk(const char *vpk_path, char *title_id, size_t title_id_size)
{
  if (vpk_path == NULL || title_id == NULL || title_id_size < 10)
    return INSTALL_ERROR_ARGUMENT;
  size_t path_length = strlen(vpk_path);
  if (path_length < 5 || strcmp(vpk_path + path_length - 4, ".vpk") != 0)
    return INSTALL_ERROR_ARGUMENT;

  title_id[0] = '\0';
  remove_tree(INSTALL_STAGE);
  int result = ensure_directory(INSTALL_STAGE);
  if (result >= 0)
    result = extract_vpk(vpk_path);
  if (result >= 0)
    result = make_head_bin(title_id, title_id_size);
  if (result >= 0)
    result = promote_stage();
  int cleanup_result = remove_tree(INSTALL_STAGE);
  if (result >= 0 && cleanup_result < 0)
    result = cleanup_result;
  return result;
}
