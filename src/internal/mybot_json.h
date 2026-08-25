/* SPDX-License-Identifier: MIT */
/*
   Copyright (c) 2009 Dave Gamble

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
 */

#ifndef MYBOT_JSON_H_
#define MYBOT_JSON_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This cJSON-derived implementation is intentionally namespaced. Do not add
 * compatibility aliases for the original cJSON API: applications may link
 * another cJSON copy into the same process.
 */

/* JSON value types. */
#define MYBOT_JSON_FALSE 0
#define MYBOT_JSON_TRUE 1
#define MYBOT_JSON_NULL 2
#define MYBOT_JSON_NUMBER 3
#define MYBOT_JSON_STRING 4
#define MYBOT_JSON_ARRAY 5
#define MYBOT_JSON_OBJECT 6

/* The mybot_json_t structure: */
typedef struct mybot_json {
    struct mybot_json *next, *prev; /* linked children for arrays and objects */
    struct mybot_json *child;       /* An array or object item will have a child pointer pointing
                                              to a chain of the items in the array/object. */

    int type; /* The type of the item, as above. */

    char *valuestring;  /* The item's string, if type==MYBOT_JSON_STRING */
    long long valueint; /* The item's number, if type==MYBOT_JSON_NUMBER */
    double valuedouble; /* The item's number, if type==MYBOT_JSON_NUMBER */

    char *string; /* The item's name string, if this item is the child of, or is in the list of
                     subitems of an object. */
} mybot_json_t;

typedef struct mybot_json_hooks {
    void *(*malloc_fn)(size_t sz);
    void (*free_fn)(void *ptr);
} mybot_json_hooks_t;

/*
 * Configure a matched allocator pair, or pass NULL to restore the AOSL HAL allocators.
 * Call only when no JSON values or printed strings are alive.
 */
extern int mybot_json_init_hooks(const mybot_json_hooks_t *hooks);

/* Supply a block of JSON, and this returns a mybot_json_t object you can interrogate. Call
 * mybot_json_delete when finished. */
extern mybot_json_t *mybot_json_parse(const char *value);
/* Render without formatting. Release the result with mybot_json_free_string(). */
extern char *mybot_json_print_unformatted(mybot_json_t *item);
/* Delete a mybot_json_t entity and all subentities. */
extern void mybot_json_delete(mybot_json_t *c);

/* Get item "string" from object. Case insensitive. */
extern mybot_json_t *mybot_json_get_object_item(const mybot_json_t *object, const char *string);

/* Type-safe accessors used by project modules. */
extern const char *mybot_json_get_string(const mybot_json_t *value);
extern bool mybot_json_get_integer(const mybot_json_t *value, int64_t *result);

/* These calls create a mybot_json_t item of the appropriate type. */
extern mybot_json_t *mybot_json_create_bool(int b);
extern mybot_json_t *mybot_json_create_number(double num);
extern mybot_json_t *mybot_json_create_string(const char *string);
extern mybot_json_t *mybot_json_create_object(void);

/* Convenience object builders. mybot_json_add_item() takes ownership only on success. */
extern int mybot_json_add_string(mybot_json_t *object, const char *name, const char *value);
extern int mybot_json_add_number(mybot_json_t *object, const char *name, double value);
extern int mybot_json_add_bool(mybot_json_t *object, const char *name, bool value);
extern int mybot_json_add_item(mybot_json_t *object, const char *name, mybot_json_t *item);
extern void mybot_json_free_string(char *text);

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_JSON_H_ */
