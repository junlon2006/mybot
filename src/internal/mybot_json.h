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

#define MYBOT_JSON_IS_REFERENCE 256
#define MYBOT_JSON_OBJECT_NAME(a) #a

/* The mybot_json_t structure: */
typedef struct mybot_json {
    struct mybot_json *next,
        *prev;                /* next/prev allow you to walk array/object chains.
                                 Alternatively, use
                                 mybot_json_get_array_size/mybot_json_get_array_item/mybot_json_get_object_item */
    struct mybot_json *child; /* An array or object item will have a child pointer pointing
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
/* Render a mybot_json_t entity to text for transfer/storage. Release it with
 * mybot_json_free_string(). */
extern char *mybot_json_print(mybot_json_t *item);
/* Render without formatting. Release the result with mybot_json_free_string(). */
extern char *mybot_json_print_unformatted(mybot_json_t *item);
/* Delete a mybot_json_t entity and all subentities. */
extern void mybot_json_delete(mybot_json_t *c);

/* Returns the number of items in an array (or object). */
extern int mybot_json_get_array_size(mybot_json_t *array);
/* Retrieve item number "item" from array "array". Returns NULL if unsuccessful. */
extern mybot_json_t *mybot_json_get_array_item(mybot_json_t *array, int item);
/* Get item "string" from object. Case insensitive. */
extern mybot_json_t *mybot_json_get_object_item(const mybot_json_t *object, const char *string);

/* Type-safe accessors used by project modules. */
extern const char *mybot_json_get_string(const mybot_json_t *value);
extern bool mybot_json_get_integer(const mybot_json_t *value, int64_t *result);

/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to
 * look a few chars back to make sense of it. Defined when mybot_json_parse() returns 0. 0
 * when mybot_json_parse() succeeds. */
extern const char *mybot_json_get_error_pointer(void);

/* These calls create a mybot_json_t item of the appropriate type. */
extern mybot_json_t *mybot_json_create_null(void);
extern mybot_json_t *mybot_json_create_true(void);
extern mybot_json_t *mybot_json_create_false(void);
extern mybot_json_t *mybot_json_create_bool(int b);
extern mybot_json_t *mybot_json_create_number(double num);
extern mybot_json_t *mybot_json_create_string(const char *string);
extern mybot_json_t *mybot_json_create_array(void);
extern mybot_json_t *mybot_json_create_object(void);

/* These utilities create an Array of count items. */
extern mybot_json_t *mybot_json_create_int_array(const int *numbers, int count);
extern mybot_json_t *mybot_json_create_float_array(const float *numbers, int count);
extern mybot_json_t *mybot_json_create_double_array(const double *numbers, int count);
extern mybot_json_t *mybot_json_create_string_array(const char **strings, int count);

/* Append item to the specified array/object. Ownership transfers only on success. */
extern int mybot_json_add_item_to_array(mybot_json_t *array, mybot_json_t *item);
extern int mybot_json_add_item_to_object(mybot_json_t *object, const char *string,
                                         mybot_json_t *item);
/* Append reference to item to the specified array/object. Use this when you want to add an existing
 * mybot_json_t to a new mybot_json_t, but don't want to corrupt your existing
 * mybot_json_t. */
extern int mybot_json_add_item_reference_to_array(mybot_json_t *array, mybot_json_t *item);
extern int mybot_json_add_item_reference_to_object(mybot_json_t *object, const char *string,
                                                   mybot_json_t *item);

/* Convenience object builders. mybot_json_add_item() takes ownership only on success. */
extern int mybot_json_add_null(mybot_json_t *object, const char *name);
extern int mybot_json_add_string(mybot_json_t *object, const char *name, const char *value);
extern int mybot_json_add_number(mybot_json_t *object, const char *name, double value);
extern int mybot_json_add_bool(mybot_json_t *object, const char *name, bool value);
extern int mybot_json_add_item(mybot_json_t *object, const char *name, mybot_json_t *item);

/* Remove/detach items from Arrays/Objects. */
extern mybot_json_t *mybot_json_detach_item_from_array(mybot_json_t *array, int which);
extern void mybot_json_delete_item_from_array(mybot_json_t *array, int which);
extern mybot_json_t *mybot_json_detach_item_from_object(mybot_json_t *object, const char *string);
extern void mybot_json_delete_item_from_object(mybot_json_t *object, const char *string);

/* Update array items. */
extern void mybot_json_replace_item_in_array(mybot_json_t *array, int which, mybot_json_t *newitem);
extern void mybot_json_replace_item_in_object(mybot_json_t *object, const char *string,
                                              mybot_json_t *newitem);

/* Duplicate a mybot_json_t item */
extern mybot_json_t *mybot_json_duplicate(mybot_json_t *item, int recurse);
/* Duplicate will create a new, identical mybot_json_t item to the one you pass, in new memory
   that will need to be released. With recurse!=0, it will duplicate any children connected to the
   item. The item->next and ->prev pointers are always zero on return from Duplicate. */

/* mybot_json_parse_with_options allows you to require (and check) that the JSON is null terminated,
 * and to retrieve the pointer to the final byte parsed. */
extern mybot_json_t *mybot_json_parse_with_options(const char *value, const char **return_parse_end,
                                                   int require_null_terminated);

extern void mybot_json_minify(char *json);
extern void mybot_json_free_string(char *text);

/* Macros for creating things quickly. */
#define MYBOT_JSON_ADD_NULL_TO_OBJECT(object, name) mybot_json_add_null(object, name)
#define MYBOT_JSON_ADD_TRUE_TO_OBJECT(object, name) mybot_json_add_bool(object, name, true)
#define MYBOT_JSON_ADD_FALSE_TO_OBJECT(object, name) mybot_json_add_bool(object, name, false)
#define MYBOT_JSON_ADD_BOOL_TO_OBJECT(object, name, b) mybot_json_add_bool(object, name, b)
#define MYBOT_JSON_ADD_NUMBER_TO_OBJECT(object, name, n) mybot_json_add_number(object, name, n)
#define MYBOT_JSON_ADD_STRING_TO_OBJECT(object, name, s) mybot_json_add_string(object, name, s)

/* When assigning an integer value, it needs to be propagated to valuedouble too. */
#define MYBOT_JSON_SET_INT_VALUE(object, val)                                                      \
    ((object) ? (object)->valueint = (object)->valuedouble = (val) : (val))

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_JSON_H_ */
