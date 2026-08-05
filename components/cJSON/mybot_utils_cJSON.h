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

#ifndef MYBOT_UTILS_CJSON_H_
#define MYBOT_UTILS_CJSON_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* mybot_utils_cJSON Types: */
#define mybot_utils_cJSON_False 0
#define mybot_utils_cJSON_True 1
#define mybot_utils_cJSON_NULL 2
#define mybot_utils_cJSON_Number 3
#define mybot_utils_cJSON_String 4
#define mybot_utils_cJSON_Array 5
#define mybot_utils_cJSON_Object 6

#define mybot_utils_cJSON_IsReference 256
#define MYBOT_UTILS_CJSON_OBJECT_NAME(a) #a

/* The mybot_utils_cJSON structure: */
typedef struct mybot_utils_cJSON {
    struct mybot_utils_cJSON *next,
        *prev; /* next/prev allow you to walk array/object chains. Alternatively, use
                  GetArraySize/GetArrayItem/GetObjectItem */
    struct mybot_utils_cJSON *child; /* An array or object item will have a child pointer pointing
                                        to a chain of the items in the array/object. */

    int type; /* The type of the item, as above. */

    char *valuestring;  /* The item's string, if type==mybot_utils_cJSON_String */
    long long valueint; /* The item's number, if type==mybot_utils_cJSON_Number */
    double valuedouble; /* The item's number, if type==mybot_utils_cJSON_Number */

    char *string; /* The item's name string, if this item is the child of, or is in the list of
                     subitems of an object. */
} mybot_utils_cJSON;

typedef struct mybot_utils_cJSON_Hooks {
    void *(*malloc_fn)(size_t sz);
    void (*free_fn)(void *ptr);
} mybot_utils_cJSON_Hooks;

/* Supply malloc, realloc and free functions to mybot_utils_cJSON */
extern void mybot_utils_cJSON_InitHooks(mybot_utils_cJSON_Hooks *hooks);

/* Supply a block of JSON, and this returns a mybot_utils_cJSON object you can interrogate. Call
 * mybot_utils_cJSON_Delete when finished. */
extern mybot_utils_cJSON *mybot_utils_cJSON_Parse(const char *value);
/* Render a mybot_utils_cJSON entity to text for transfer/storage. Free the char* when finished. */
extern char *mybot_utils_cJSON_Print(mybot_utils_cJSON *item);
/* Render a mybot_utils_cJSON entity to text for transfer/storage without any formatting. Free the
 * char* when finished. */
extern char *mybot_utils_cJSON_PrintUnformatted(mybot_utils_cJSON *item);
/* Delete a mybot_utils_cJSON entity and all subentities. */
extern void mybot_utils_cJSON_Delete(mybot_utils_cJSON *c);

/* Returns the number of items in an array (or object). */
extern int mybot_utils_cJSON_GetArraySize(mybot_utils_cJSON *array);
/* Retrieve item number "item" from array "array". Returns NULL if unsuccessful. */
extern mybot_utils_cJSON *mybot_utils_cJSON_GetArrayItem(mybot_utils_cJSON *array, int item);
/* Get item "string" from object. Case insensitive. */
extern mybot_utils_cJSON *mybot_utils_cJSON_GetObjectItem(mybot_utils_cJSON *object,
                                                          const char *string);

/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to
 * look a few chars back to make sense of it. Defined when mybot_utils_cJSON_Parse() returns 0. 0
 * when mybot_utils_cJSON_Parse() succeeds. */
extern const char *mybot_utils_cJSON_GetErrorPtr(void);

/* These calls create a mybot_utils_cJSON item of the appropriate type. */
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateNull(void);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateTrue(void);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateFalse(void);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateBool(int b);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateNumber(double num);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateString(const char *string);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateArray(void);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateObject(void);

/* These utilities create an Array of count items. */
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateIntArray(const int *numbers, int count);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateFloatArray(const float *numbers, int count);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateDoubleArray(const double *numbers, int count);
extern mybot_utils_cJSON *mybot_utils_cJSON_CreateStringArray(const char **strings, int count);

/* Append item to the specified array/object. */
extern void mybot_utils_cJSON_AddItemToArray(mybot_utils_cJSON *array, mybot_utils_cJSON *item);
extern void mybot_utils_cJSON_AddItemToObject(mybot_utils_cJSON *object, const char *string,
                                              mybot_utils_cJSON *item);
/* Append reference to item to the specified array/object. Use this when you want to add an existing
 * mybot_utils_cJSON to a new mybot_utils_cJSON, but don't want to corrupt your existing
 * mybot_utils_cJSON. */
extern void mybot_utils_cJSON_AddItemReferenceToArray(mybot_utils_cJSON *array,
                                                      mybot_utils_cJSON *item);
extern void mybot_utils_cJSON_AddItemReferenceToObject(mybot_utils_cJSON *object,
                                                       const char *string, mybot_utils_cJSON *item);

/* Remove/Detatch items from Arrays/Objects. */
extern mybot_utils_cJSON *mybot_utils_cJSON_DetachItemFromArray(mybot_utils_cJSON *array,
                                                                int which);
extern void mybot_utils_cJSON_DeleteItemFromArray(mybot_utils_cJSON *array, int which);
extern mybot_utils_cJSON *mybot_utils_cJSON_DetachItemFromObject(mybot_utils_cJSON *object,
                                                                 const char *string);
extern void mybot_utils_cJSON_DeleteItemFromObject(mybot_utils_cJSON *object, const char *string);

/* Update array items. */
extern void mybot_utils_cJSON_ReplaceItemInArray(mybot_utils_cJSON *array, int which,
                                                 mybot_utils_cJSON *newitem);
extern void mybot_utils_cJSON_ReplaceItemInObject(mybot_utils_cJSON *object, const char *string,
                                                  mybot_utils_cJSON *newitem);

/* Duplicate a mybot_utils_cJSON item */
extern mybot_utils_cJSON *mybot_utils_cJSON_Duplicate(mybot_utils_cJSON *item, int recurse);
/* Duplicate will create a new, identical mybot_utils_cJSON item to the one you pass, in new memory
   that will need to be released. With recurse!=0, it will duplicate any children connected to the
   item. The item->next and ->prev pointers are always zero on return from Duplicate. */

/* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve
 * the pointer to the final byte parsed. */
extern mybot_utils_cJSON *mybot_utils_cJSON_ParseWithOpts(const char *value,
                                                          const char **return_parse_end,
                                                          int require_null_terminated);

extern void mybot_utils_cJSON_Minify(char *json);

/* Macros for creating things quickly. */
#define mybot_utils_cJSON_AddNullToObject(object, name)                                            \
    mybot_utils_cJSON_AddItemToObject(object, name, mybot_utils_cJSON_CreateNull())
#define mybot_utils_cJSON_AddTrueToObject(object, name)                                            \
    mybot_utils_cJSON_AddItemToObject(object, name, mybot_utils_cJSON_CreateTrue())
#define mybot_utils_cJSON_AddFalseToObject(object, name)                                           \
    mybot_utils_cJSON_AddItemToObject(object, name, mybot_utils_cJSON_CreateFalse())
#define mybot_utils_cJSON_AddBoolToObject(object, name, b)                                         \
    mybot_utils_cJSON_AddItemToObject(object, name, mybot_utils_cJSON_CreateBool(b))
#define mybot_utils_cJSON_AddNumberToObject(object, name, n)                                       \
    mybot_utils_cJSON_AddItemToObject(object, name, mybot_utils_cJSON_CreateNumber(n))
#define mybot_utils_cJSON_AddStringToObject(object, name, s)                                       \
    mybot_utils_cJSON_AddItemToObject(object, name, mybot_utils_cJSON_CreateString(s))

/* When assigning an integer value, it needs to be propagated to valuedouble too. */
#define mybot_utils_cJSON_SetIntValue(object, val)                                                 \
    ((object) ? (object)->valueint = (object)->valuedouble = (val) : (val))

#ifdef __cplusplus
}
#endif

#endif /* MYBOT_UTILS_CJSON_H_ */
