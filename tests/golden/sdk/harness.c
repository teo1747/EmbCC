/* Drives the EmbCC-COMPILED sval SDK from a gcc-built main: build a
 * table of records, serialize it through wire.c, deserialize it back,
 * and check the values survive. If EmbCC miscompiled the struct-by-
 * value API, the varint codec, or the double path, this notices. */
#include "value/value.h"
#include "wire/wire.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    struct value t = value_table();
    for (int i = 0; i < 5; i++) {
        struct value row = value_record();
        value_record_set(&row, "n", value_int(i * 10));
        value_record_set(&row, "name", value_string("row"));
        value_record_set(&row, "size", value_filesize(1536 * (i + 1)));
        value_record_set(&row, "ratio", value_float(0.5 * i));
        value_table_push_row(&t, row);
    }
    struct table *tb = t.u.table;
    printf("built rows=%zu\n", tb->count);

    struct wire_buf b;
    wire_buf_init(&b);
    if (wire_serialize(&t, &b)) { printf("serialize failed\n"); return 1; }
    printf("serialized %zu bytes\n", b.len);

    /* wire_serialize emits a FRAME: [uvarint payload-len][payload].
     * wire_deserialize wants the payload, so step over the varint. */
    size_t pre = 0;
    while (pre < b.len && (b.data[pre] & 0x80)) pre++;
    pre++;
    struct value back;
    if (wire_deserialize(b.data + pre, b.len - pre, &back)) {
        printf("deserialize failed\n"); return 1;
    }
    if (back.type != VAL_TABLE) { printf("wrong type back\n"); return 1; }
    printf("round-tripped rows=%zu\n", back.u.table->count);

    /* spot-check a scalar of each interesting class */
    struct record *r = &back.u.table->rows[3];
    for (size_t i = 0; i < r->count; i++) {
        struct value *v = &r->values[i];
        const char *nm = r->names[i];
        if (!strcmp(nm, "n") && v->u.i != 30)
            { printf("int wrong: %lld\n", (long long)v->u.i); return 1; }
        if (!strcmp(nm, "size") && v->u.i != 6144)
            { printf("filesize wrong: %lld\n", (long long)v->u.i); return 1; }
        if (!strcmp(nm, "ratio") && v->u.f != 1.5)
            { printf("double wrong: %f\n", v->u.f); return 1; }
    }
    printf("value/wire round-trip OK (int, string, filesize, double)\n");
    value_free(&t); value_free(&back); wire_buf_free(&b);
    return 42;
}
