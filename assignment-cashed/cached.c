#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include "threading.h"
#include "sockets.h"
#include "stringBuffers.h"
//@ #include <listex.gh>

struct entry {
  struct string_buffer *key;
  struct string_buffer *value;
  struct entry *next;
};

struct dict {
  struct mutex *mutex;
  int bucketCount;
  struct entry **buckets;
};

struct dict *create_dict(int initialBucketCount)
{
  if (UINTPTR_MAX / sizeof(struct entry *) <= (size_t)initialBucketCount)
    abort();
  //@ div_rem_nonneg(UINTPTR_MAX, sizeof(struct entry *));
  //@ mul_mono_l(0, initialBucketCount, sizeof(struct entry *));
  //@ mul_mono_l(initialBucketCount, UINTPTR_MAX / sizeof(struct entry *), sizeof(struct entry *));
  struct entry **buckets = malloc(initialBucketCount * sizeof(struct entry *));
  if (buckets == 0)
    abort();
  for (int i = 0; i < initialBucketCount; i++)
  {
    buckets[i] = 0;
  }
  struct dict *result = malloc(sizeof(struct dict));
  if (result == 0)
    abort();
  result->bucketCount = initialBucketCount;
  result->buckets = buckets;
  struct mutex *mutex = create_mutex();
  result->mutex = mutex;
  return result;
}
    
unsigned hash_of_string_buffer(struct string_buffer *buf)
{
  unsigned result = 0;
  int length = string_buffer_get_length(buf);
  char *cs = string_buffer_get_chars(buf);
  for (int i = 0; i < length; i++)
  {
    result = (/*@truncating@*/ ((/*@truncating@*/ (result * 31)) + (/*@truncating@*/ (unsigned char)cs[i])));
  }
  return result;
}

typedef void entry_visitor(struct entry **pentry, void *data);

void entries_visit_entry(struct entry **pentry, struct string_buffer *key, entry_visitor *visitor, void *visitorData)
{
  if (*pentry == 0 || string_buffer_equals((*pentry)->key, key))
    visitor(pentry, visitorData);
  else
    entries_visit_entry(&(*pentry)->next, key, visitor, visitorData);
}

void dict_visit_entry(struct dict *dict, struct string_buffer *key, entry_visitor *visitor, void *visitorData)
{
  unsigned hash = hash_of_string_buffer(key);
  mutex_acquire(dict->mutex);
  unsigned index = hash % (unsigned)dict->bucketCount;
  //@ div_rem_nonneg(hash, dict->bucketCount);
  //@ mul_mono_l(0, index, sizeof(void *));
  //@ mul_mono_l(index, dict->bucketCount, sizeof(void *));
  entries_visit_entry(&dict->buckets[index], key, visitor, visitorData);
  mutex_release(dict->mutex);
}

void get_entry_visitor(struct entry **pentry, void *data)
{
  struct string_buffer **pvalue = data;
  if (*pentry != 0) {
    *pvalue = string_buffer_copy((*pentry)->value);
  } else {
    *pvalue = 0;
  }
}

void insert_entry_visitor(struct entry **pentry, void *data)
{
  struct entry **pnewEntry = data;
  if (*pentry == 0) {
    *pentry = *pnewEntry;
    *pnewEntry = 0;
  }
}

struct compare_and_set_spec {
  struct string_buffer *old_value;
  struct string_buffer *new_value;
  bool success;
};

void compare_and_set_entry_visitor(struct entry **pentry, void *data)
{
  struct compare_and_set_spec *spec = data;
  if (*pentry == 0 || !string_buffer_equals((*pentry)->value, spec->old_value)) {
    spec->success = false;
  } else {
    string_buffer_dispose((*pentry)->value);
    (*pentry)->value = spec->new_value;
    spec->success = true;
  }
}

struct remove_spec {
  struct string_buffer *value;
  bool success;
};

void remove_entry_visitor(struct entry **pentry, void *data)
{
  struct remove_spec *spec = data;
  if (*pentry == 0 || !string_buffer_equals((*pentry)->value, spec->value)) {
    spec->success = false;
  } else {
    string_buffer_dispose((*pentry)->key);
    string_buffer_dispose((*pentry)->value);
    struct entry *next = (*pentry)->next;
    free(*pentry);
    *pentry = next;
    spec->success = true;
  }
}

struct connection {
  struct dict *dict;
  struct socket *socket;
};

void handle_connection(struct connection *connection)
{
  struct dict *dict = connection->dict;
  struct socket *socket = connection->socket;
  free(connection);

  for (;;)
  {
    struct string_buffer *line = create_string_buffer();
    if (socket_read_line(socket, line)) {
      string_buffer_dispose(line);
      socket_close(socket);
      return;
    }
    struct string_buffer *cmd = create_string_buffer();
    struct string_buffer *arg = create_string_buffer();
    if (!string_buffer_split(line, " ", cmd, arg)) {
      socket_write_string(socket, "argument expected\n");
      string_buffer_dispose(arg);
      string_buffer_dispose(cmd);
      string_buffer_dispose(line);
      continue;
    }
    string_buffer_dispose(line);
    if (string_buffer_equals_string(cmd, "get")) {
      struct string_buffer *key = arg;
      struct string_buffer *value;
      dict_visit_entry(dict, key, get_entry_visitor, &value);
      if (value == 0) {
        socket_write_string(socket, "no such entry\n");
      } else {
        socket_write_string(socket, "ok\n");
        socket_write_string_buffer(socket, value);
        socket_write_string(socket, "\n");
        string_buffer_dispose(value);
      }
      string_buffer_dispose(key);
    } else if (string_buffer_equals_string(cmd, "insert")) {
      struct entry *newEntry;
      
      struct string_buffer *key = create_string_buffer();
      struct string_buffer *value = create_string_buffer();
      if (!string_buffer_split(arg, " ", key, value)) {
        socket_write_string(socket, "value expected\n");
        string_buffer_dispose(value);
        string_buffer_dispose(key);
        string_buffer_dispose(arg);
        string_buffer_dispose(cmd);
        continue;
      }
      string_buffer_dispose(arg);
      newEntry = malloc(sizeof(struct entry));
      if (newEntry == 0)
        abort();
      struct string_buffer *keyCopy = string_buffer_copy(key);
      newEntry->key = keyCopy;
      newEntry->value = value;
      newEntry->next = 0;
      dict_visit_entry(dict, key, insert_entry_visitor, &newEntry);
      if (newEntry == 0) {
        socket_write_string(socket, "ok\n");
      } else {
        socket_write_string(socket, "exists\n");
        string_buffer_dispose(newEntry->key);
        string_buffer_dispose(newEntry->value);
        free(newEntry);
      }
      string_buffer_dispose(key);
    } else if (string_buffer_equals_string(cmd, "replace")) {
      struct compare_and_set_spec spec;
      struct string_buffer *key = create_string_buffer();
      struct string_buffer *values = create_string_buffer();
      if (!string_buffer_split(arg, " ", key, values)) {
        socket_write_string(socket, "values expected\n");
        string_buffer_dispose(values);
        string_buffer_dispose(key);
        string_buffer_dispose(arg);
        string_buffer_dispose(cmd);
        continue;
      }
      string_buffer_dispose(arg);
      struct string_buffer *old_value = create_string_buffer();
      struct string_buffer *new_value = create_string_buffer();
      if (!string_buffer_split(values, " ", old_value, new_value)) {
        socket_write_string(socket, "new value expected\n");
        string_buffer_dispose(new_value);
        string_buffer_dispose(old_value);
        string_buffer_dispose(values);
        string_buffer_dispose(key);
        string_buffer_dispose(cmd);
        continue;
      }
      string_buffer_dispose(values);
      spec.old_value = old_value;
      spec.new_value = new_value;
      dict_visit_entry(dict, key, compare_and_set_entry_visitor, &spec);
      if (spec.success) {
        socket_write_string(socket, "ok\n");
      } else {
        socket_write_string(socket, "no such entry\n");
        string_buffer_dispose(spec.new_value);
      }
      string_buffer_dispose(spec.old_value);
      string_buffer_dispose(key);
    } else if (string_buffer_equals_string(cmd, "remove")) {
      struct remove_spec spec;
      struct string_buffer *key = create_string_buffer();
      struct string_buffer *value = create_string_buffer();
      if (!string_buffer_split(arg, " ", key, value)) {
        socket_write_string(socket, "value expected\n");
        string_buffer_dispose(value);
        string_buffer_dispose(key);
        string_buffer_dispose(arg);
        string_buffer_dispose(cmd);
        continue;
      }
      string_buffer_dispose(arg);
      spec.value = value;
      dict_visit_entry(dict, key, remove_entry_visitor, &spec);
      if (spec.success) {
        socket_write_string(socket, "ok\n");
      } else {
        socket_write_string(socket, "no such entry\n");
      }
      string_buffer_dispose(spec.value);
      string_buffer_dispose(key);
    } else {
      string_buffer_dispose(arg);
      socket_write_string(socket, "no such command\n");
    }
    string_buffer_dispose(cmd);
  }
}
      
int main()
{
  struct dict *dict = create_dict(1 << 10);

  struct server_socket *serverSocket = create_server_socket(12345);
  if (serverSocket == 0) {
    printf("Could not create server socket at port 12345. Terminating the program...\n");
    return 1;
  }

  for (;;)
  {
    struct socket *socket = server_socket_accept(serverSocket);
    if (socket == 0) {
      printf("server_socket_accept failed. Terminating the program.\n");
      abort();
    }
    struct connection *connection = malloc(sizeof(struct connection));
    if (connection == 0) abort();
    connection->dict = dict;
    connection->socket = socket;
    thread_start(handle_connection, connection);
  }
}
