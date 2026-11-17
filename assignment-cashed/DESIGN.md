# Design decisions for the verification of cached.c

For each decision: the choice, the code that forces it, and the alternatives that were rejected.
Line numbers refer to the original cached.c.

## Decision 1: what one bucket's list owns (the `entries` predicate) [DONE]

```c
predicate entries(struct entry *e) =
  e == 0 ?
    true
  :
    e->key |-> ?key &*& e->value |-> ?value &*& e->next |-> ?next &*&
    malloc_block_entry(e) &*&
    string_buffer(key, _) &*& string_buffer(value, _) &*&
    entries(next);
```

| Part of the predicate | Needed because (code) |
|---|---|
| `e == 0 ? true` | the end of a list is a null pointer; `insert` puts the new entry into a slot holding 0 (l.92-93) |
| `e->key \|-> ?key` | `entries_visit_entry` reads the key (l.61); `remove` reads it (l.127) |
| `e->value \|-> ?value` | `get` reads it (l.83); `compare_and_set` reads and writes it (l.107, l.111); `remove` reads it (l.124, l.128) |
| `e->next \|-> ?next` | `entries_visit_entry` recurses into `&(*pentry)->next` (l.64); `remove` reads it (l.129) |
| `malloc_block_entry(e)` | `remove` frees the entry (l.130), and `free` requires the malloc_block |
| `string_buffer(key, _)` (full) | `remove` disposes the key (l.127), and dispose requires full ownership |
| `string_buffer(value, _)` (full) | `compare_and_set` and `remove` dispose the value (l.110, l.128) |
| `entries(next)` | the rest of the list (recursion) |

Rejected alternatives:
- **Owning only the struct fields, without the string buffers:** `string_buffer_dispose` at l.110, l.127 and l.128 would fail with "No matching heap chunks".
- **Tracking the contents (e.g. `list<char>` for each key/value):** not needed, since the assignment only requires memory safety, no data races and no overflow. It would only add proof work.
- **Fractional ownership of the buffers:** dispose needs the full buffer.

## Decision 2: the bucket array [SKETCH]
`pointers(buckets, n, ?es) &*& foreach(es, entries)`: library predicates, plus `foreach_remove_nth`/`foreach_unremove_nth` from listex.gh to take bucket `index` out and put it back (dict_visit_entry, l.75).

## Decision 3: what the mutex protects [SKETCH]
- The mutex invariant holds the bucket array + all lists. They are only accessed between `mutex_acquire` (l.70) and `mutex_release` (l.76).
- The dict fields `mutex`, `bucketCount`, `buckets` are never written after `create_dict`, so they are shared read-only by all threads: `[_]dict(d)` (§24 dummy fractions; `main` starts unboundedly many threads).

## Decision 4: the visitor contract [SKETCH]
```c
//@ requires *pentry |-> ?e &*& entries(e) &*& entry_visitor_pre(this)(data);
//@ ensures  *pentry |-> ?e2 &*& entries(e2) &*& entry_visitor_post(this)(data);
```
This fits the recursion: after `open entries(e)`, `&e->next` is again "a slot + a list".

## Decision 5: what a connection thread owns [SKETCH]
`thread_run_data(handle_connection)(c)` = the connection fields + `malloc_block_connection(c)` (freed at l.145) + `socket_input_stream` + `socket_output_stream` + `[_]dict(d)`.
Tested in a small example: `//@ : thread_run` on handle_connection works without changing C code, and the predicate_family_instance can follow the function.
