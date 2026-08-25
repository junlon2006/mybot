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

/* Namespaced cJSON implementation. */
/* JSON parser in C. */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include "mybot_json.h"

#include <hal/aosl_hal_memory.h>

static int mybot_json_strcasecmp(const char *s1, const char *s2) {
    if (!s1)
        return (s1 == s2) ? 0 : 1;
    if (!s2)
        return 1;
    for (; tolower(*s1) == tolower(*s2); ++s1, ++s2)
        if (*s1 == 0)
            return 0;
    return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
}

/* Default to the AOSL HAL allocator so JSON stays consistent with the rest of
 * the SDK and follows any platform redirect of aosl_hal_malloc(). Tests may
 * override both through mybot_json_init_hooks(). */
static void *(*mybot_json_malloc_fn)(size_t sz) = aosl_hal_malloc;
static void (*mybot_json_free_fn)(void *ptr) = aosl_hal_free;

static char *mybot_json_strdup(const char *str) {
    size_t len;
    char *copy;

    len = strlen(str) + 1;
    copy = (char *)mybot_json_malloc_fn(len);
    if (!copy)
        return 0;
    memcpy(copy, str, len);
    return copy;
}

int mybot_json_init_hooks(const mybot_json_hooks_t *hooks) {
    if (!hooks) { /* Reset hooks */
        mybot_json_malloc_fn = aosl_hal_malloc;
        mybot_json_free_fn = aosl_hal_free;
        return 0;
    }
    if (!hooks->malloc_fn || !hooks->free_fn)
        return -1;

    mybot_json_malloc_fn = hooks->malloc_fn;
    mybot_json_free_fn = hooks->free_fn;
    return 0;
}

/* Internal constructor. */
static mybot_json_t *mybot_json_new_item(void) {
    mybot_json_t *node = (mybot_json_t *)mybot_json_malloc_fn(sizeof(mybot_json_t));
    if (node)
        memset(node, 0, sizeof(mybot_json_t));
    return node;
}

/* Delete a mybot_json_t structure. */
void mybot_json_delete(mybot_json_t *c) {
    mybot_json_t *next;
    while (c) {
        next = c->next;
        if (c->child)
            mybot_json_delete(c->child);
        if (c->valuestring)
            mybot_json_free_fn(c->valuestring);
        if (c->string)
            mybot_json_free_fn(c->string);
        mybot_json_free_fn(c);
        c = next;
    }
}

static const char *parse_number_u64(mybot_json_t *item, const char *num) {
    // double n=0,sign=1,scale=0;int subscale=0,signsubscale=1;
    long long n = 0;
    double f = 0;
    int sign = 1, scale = 0;
    int subscale = 0, signsubscale = 1;
    unsigned char is_float = 0;

    if (*num == '-') {
        sign = -1;
        num++; /* Has sign? */
    }

    if (*num == '0') {
        num++; /* is zero */
    }

    if (*num >= '1' && *num <= '9') {
        do {
            n = (n * 10) + (*num - '0');
            num++;
        } while (*num >= '0' && *num <= '9'); /* Number? */
    }

    if (*num == '.' && num[1] >= '0' && num[1] <= '9') {
        is_float = 1;
        f = n * 1.0;
        num++;

        do {
            f = (f * 10.0) + (*num - '0');
            scale--;
            num++;
        } while (*num >= '0' && *num <= '9');
    } /* Fractional part? */

    if (*num == 'e' || *num == 'E') /* Exponent? */ {
        num++;
        if (*num == '+') {
            num++;
        } else if (*num == '-') {
            signsubscale = -1;
            num++; /* With sign? */
        }
        while (*num >= '0' && *num <= '9') {
            subscale = (subscale * 10) + (*num - '0'); /* Number? */
            num++;
        }
    }

    if (!is_float) {
        n = sign * n;
        item->valuedouble = (double)n;
        item->valueint = n;
    } else {
        f = sign * f *
            pow(10.0,
                (scale +
                 subscale * signsubscale)); /* number = +/- number.fraction * 10^+/- exponent */
        item->valuedouble = f;
        item->valueint = (long long)f;
    }

    item->type = MYBOT_JSON_NUMBER;
    return num;
}

/* Render the number nicely from the given item into a string. */
static char *print_number(mybot_json_t *item) {
    char *str;
    double d = item->valuedouble;
    if (fabs(((double)item->valueint) - d) <= DBL_EPSILON && d <= INT_MAX && d >= INT_MIN) {
        str = (char *)mybot_json_malloc_fn(21); /* 2^64+1 can be represented in 21 chars. */
        if (str)
            sprintf(str, "%lld", item->valueint);
    } else {
        str = (char *)mybot_json_malloc_fn(64); /* This is a nice tradeoff. */
        if (str) {
            if (fabs(floor(d) - d) <= DBL_EPSILON && fabs(d) < 1.0e60)
                sprintf(str, "%.0f", d);
            else if (fabs(d) < 1.0e-6 || fabs(d) > 1.0e9)
                sprintf(str, "%e", d);
            else
                sprintf(str, "%g", d);
        }
    }
    return str;
}

static unsigned parse_hex4(const char *str) {
    unsigned h = 0;
    if (*str >= '0' && *str <= '9')
        h += (*str) - '0';
    else if (*str >= 'A' && *str <= 'F')
        h += 10 + (*str) - 'A';
    else if (*str >= 'a' && *str <= 'f')
        h += 10 + (*str) - 'a';
    else
        return 0;
    h = h << 4;
    str++;
    if (*str >= '0' && *str <= '9')
        h += (*str) - '0';
    else if (*str >= 'A' && *str <= 'F')
        h += 10 + (*str) - 'A';
    else if (*str >= 'a' && *str <= 'f')
        h += 10 + (*str) - 'a';
    else
        return 0;
    h = h << 4;
    str++;
    if (*str >= '0' && *str <= '9')
        h += (*str) - '0';
    else if (*str >= 'A' && *str <= 'F')
        h += 10 + (*str) - 'A';
    else if (*str >= 'a' && *str <= 'f')
        h += 10 + (*str) - 'a';
    else
        return 0;
    h = h << 4;
    str++;
    if (*str >= '0' && *str <= '9')
        h += (*str) - '0';
    else if (*str >= 'A' && *str <= 'F')
        h += 10 + (*str) - 'A';
    else if (*str >= 'a' && *str <= 'f')
        h += 10 + (*str) - 'a';
    else
        return 0;
    return h;
}

/* Parse the input text into an unescaped cstring, and populate item. */
static const unsigned char firstByteMark[7] = {0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC};
static const char *parse_string(mybot_json_t *item, const char *str) {
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    int len = 0;
    unsigned uc, uc2;
    if (*str != '\"') {
        return 0;
    } /* not a string! */

    while (*ptr != '\"' && *ptr && ++len)
        if (*ptr++ == '\\')
            ptr++; /* Skip escaped quotes. */

    out = (char *)mybot_json_malloc_fn(len +
                                       1); /* This is how long we need for the string, roughly. */
    if (!out)
        return 0;

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && *ptr) {
        if (*ptr != '\\')
            *ptr2++ = *ptr++;
        else {
            ptr++;
            switch (*ptr) {
            case 'b':
                *ptr2++ = '\b';
                break;
            case 'f':
                *ptr2++ = '\f';
                break;
            case 'n':
                *ptr2++ = '\n';
                break;
            case 'r':
                *ptr2++ = '\r';
                break;
            case 't':
                *ptr2++ = '\t';
                break;
            case 'u': { /* transcode utf16 to utf8. */
                /* Only consume the four hex digits when they are valid. A
                 * malformed escape such as "\u\"" would otherwise advance past
                 * the closing quote and let the writer outrun the buffer that
                 * the first length pass allocated. */
                int hex_ok = 1;
                for (int i = 1; i <= 4; i++) {
                    const unsigned char h = (const unsigned char)ptr[i];
                    if (!((h >= '0' && h <= '9') || (h >= 'A' && h <= 'F') ||
                          (h >= 'a' && h <= 'f'))) {
                        hex_ok = 0;
                        break;
                    }
                }
                if (!hex_ok) {
                    break;
                }
                uc = parse_hex4(ptr + 1);
                ptr += 4; /* get the unicode char. */

                if ((uc >= 0xDC00 && uc <= 0xDFFF) || uc == 0)
                    break; /* check for invalid.	*/

                if (uc >= 0xD800 && uc <= 0xDBFF) /* UTF16 surrogate pairs.	*/
                {
                    if (ptr[1] != '\\' || ptr[2] != 'u')
                        break; /* missing second-half of surrogate.	*/
                    uc2 = parse_hex4(ptr + 3);
                    ptr += 6;
                    if (uc2 < 0xDC00 || uc2 > 0xDFFF)
                        break; /* invalid second-half of surrogate.	*/
                    uc = 0x10000 + (((uc & 0x3FF) << 10) | (uc2 & 0x3FF));
                }

                len = 4;
                if (uc < 0x80)
                    len = 1;
                else if (uc < 0x800)
                    len = 2;
                else if (uc < 0x10000)
                    len = 3;
                ptr2 += len;

                switch (len) {
                case 4: // NOLINT(bugprone-branch-clone): intentional UTF-8 fallthrough
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                    /* falls through */
                case 3:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                    /* falls through */
                case 2:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                    /* falls through */
                case 1:
                    *--ptr2 = (uc | firstByteMark[len]);
                    break;
                default:
                    break;
                }
                ptr2 += len;
                break;
            }
            default:
                *ptr2++ = *ptr;
                break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    if (*ptr == '\"')
        ptr++;
    item->valuestring = out;
    item->type = MYBOT_JSON_STRING;
    return ptr;
}

/* Render the cstring provided to an escaped version that can be printed. */
static char *print_string_ptr(const char *str) {
    const char *ptr;
    char *ptr2, *out;
    int len = 0;
    unsigned char token;

    if (!str)
        return mybot_json_strdup("");
    ptr = str;
    while ((token = *ptr) && ++len) {
        if (strchr("\"\\\b\f\n\r\t", token))
            len++;
        else if (token < 32)
            len += 5;
        ptr++;
    }

    out = (char *)mybot_json_malloc_fn(len + 3);
    if (!out)
        return 0;

    ptr2 = out;
    ptr = str;
    *ptr2++ = '\"';
    while (*ptr) {
        if ((unsigned char)*ptr > 31 && *ptr != '\"' && *ptr != '\\')
            *ptr2++ = *ptr++;
        else {
            *ptr2++ = '\\';
            switch (token = *ptr++) {
            case '\\':
                *ptr2++ = '\\';
                break;
            case '\"':
                *ptr2++ = '\"';
                break;
            case '\b':
                *ptr2++ = 'b';
                break;
            case '\f':
                *ptr2++ = 'f';
                break;
            case '\n':
                *ptr2++ = 'n';
                break;
            case '\r':
                *ptr2++ = 'r';
                break;
            case '\t':
                *ptr2++ = 't';
                break;
            default:
                sprintf(ptr2, "u%04x", token);
                ptr2 += 5;
                break; /* escape and print */
            }
        }
    }
    *ptr2++ = '\"';
    *ptr2++ = 0;
    return out;
}
/* Invote print_string_ptr (which is useful) on an item. */
static char *print_string(mybot_json_t *item) {
    return print_string_ptr(item->valuestring);
}

/* Predeclare these prototypes. */
static const char *parse_value(mybot_json_t *item, const char *value);
static char *print_value(mybot_json_t *item);
static const char *parse_array(mybot_json_t *item, const char *value);
static char *print_array(mybot_json_t *item);
static const char *parse_object(mybot_json_t *item, const char *value);
static char *print_object(mybot_json_t *item);

/* Utility to jump whitespace and cr/lf */
static const char *skip(const char *in) {
    while (in && *in && (unsigned char)*in <= 32)
        in++;
    return in;
}

/* Parse one JSON value and ignore any trailing bytes after it. */
mybot_json_t *mybot_json_parse(const char *value) {
    mybot_json_t *c = mybot_json_new_item();
    if (!c)
        return 0; /* memory fail */

    if (!parse_value(c, skip(value))) {
        mybot_json_delete(c);
        return 0;
    }
    return c;
}

/* Render a mybot_json_t item/entity/structure without formatting. */
char *mybot_json_print_unformatted(mybot_json_t *item) {
    return print_value(item);
}

/* Parser core - when encountering text, process appropriately. */
static const char *parse_value(mybot_json_t *item, const char *value) {
    if (!value) {
        return 0; /* Fail on null. */
    }
    if (!strncmp(value, "null", 4)) {
        item->type = MYBOT_JSON_NULL;
        return value + 4;
    }
    if (!strncmp(value, "false", 5)) {
        item->type = MYBOT_JSON_FALSE;
        return value + 5;
    }
    if (!strncmp(value, "true", 4)) {
        item->type = MYBOT_JSON_TRUE;
        item->valueint = 1;
        return value + 4;
    }
    if (*value == '\"') {
        return parse_string(item, value);
    }
    if (*value == '-' || (*value >= '0' && *value <= '9')) {
        return parse_number_u64(item, value);
    }
    if (*value == '[') {
        return parse_array(item, value);
    }
    if (*value == '{') {
        return parse_object(item, value);
    }

    return 0; /* failure. */
}

/* Render a value to compact JSON text. */
static char *print_value(mybot_json_t *item) {
    char *out = 0;
    if (!item)
        return 0;
    switch (item->type) {
    case MYBOT_JSON_NULL:
        out = mybot_json_strdup("null");
        break;
    case MYBOT_JSON_FALSE:
        out = mybot_json_strdup("false");
        break;
    case MYBOT_JSON_TRUE:
        out = mybot_json_strdup("true");
        break;
    case MYBOT_JSON_NUMBER:
        out = print_number(item);
        break;
    case MYBOT_JSON_STRING:
        out = print_string(item);
        break;
    case MYBOT_JSON_ARRAY:
        out = print_array(item);
        break;
    case MYBOT_JSON_OBJECT:
        out = print_object(item);
        break;
    default:
        break;
    }
    return out;
}

/* Build an array from input text. */
static const char *parse_array(mybot_json_t *item, const char *value) {
    mybot_json_t *child;
    if (*value != '[') {
        return 0;
    } /* not an array! */

    item->type = MYBOT_JSON_ARRAY;
    value = skip(value + 1);
    if (*value == ']')
        return value + 1; /* empty array. */

    item->child = child = mybot_json_new_item();
    if (!item->child)
        return 0;                                  /* memory fail */
    value = skip(parse_value(child, skip(value))); /* skip any spacing, get the value. */
    if (!value)
        return 0;

    while (*value == ',') {
        mybot_json_t *new_item;
        new_item = mybot_json_new_item();
        if (!new_item)
            return 0; /* memory fail */
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value)
            return 0; /* memory fail */
    }

    if (*value == ']')
        return value + 1; /* end of array */
    return 0;             /* malformed. */
}

/* Render an array to text */
static char *print_array(mybot_json_t *item) {
    char **entries;
    char *out = 0, *ptr, *ret;
    int len = 5;
    mybot_json_t *child = item->child;
    int numentries = 0, i = 0, fail = 0;

    /* How many entries in the array? */
    while (child)
        numentries++, child = child->next;
    /* Explicitly handle numentries==0 */
    if (!numentries) {
        out = (char *)mybot_json_malloc_fn(3);
        if (out)
            memcpy(out, "[]", 3);
        return out;
    }
    /* Allocate an array to hold the values for each */
    entries = (char **)mybot_json_malloc_fn(numentries * sizeof(char *));
    if (!entries)
        return 0;
    memset(entries, 0, numentries * sizeof(char *));
    /* Retrieve all the results: */
    child = item->child;
    while (child && !fail) {
        ret = print_value(child);
        entries[i++] = ret;
        if (ret)
            len += strlen(ret) + 2;
        else
            fail = 1;
        child = child->next;
    }

    /* If we didn't fail, try to malloc the output string */
    if (!fail)
        out = (char *)mybot_json_malloc_fn(len);
    /* If that fails, we fail. */
    if (!out)
        fail = 1;

    /* Handle failure. */
    if (fail) {
        for (i = 0; i < numentries; i++)
            if (entries[i])
                mybot_json_free_fn(entries[i]);
        mybot_json_free_fn(entries);
        return 0;
    }

    /* Compose the output array. */
    *out = '[';
    ptr = out + 1;
    *ptr = 0;
    for (i = 0; i < numentries; i++) {
        size_t slen = strlen(entries[i]);
        memcpy(ptr, entries[i], slen + 1);
        ptr += slen;
        if (i != numentries - 1) {
            *ptr++ = ',';
            *ptr = 0;
        }
        mybot_json_free_fn(entries[i]);
    }
    mybot_json_free_fn(entries);
    *ptr++ = ']';
    *ptr++ = 0;
    return out;
}

/* Build an object from the text. */
static const char *parse_object(mybot_json_t *item, const char *value) {
    mybot_json_t *child;
    if (*value != '{') {
        return 0;
    } /* not an object! */

    item->type = MYBOT_JSON_OBJECT;
    value = skip(value + 1);
    if (*value == '}')
        return value + 1; /* empty array. */

    item->child = child = mybot_json_new_item();
    if (!item->child)
        return 0;
    value = skip(parse_string(child, skip(value)));
    if (!value)
        return 0;
    child->string = child->valuestring;
    child->valuestring = 0;
    if (*value != ':') {
        return 0;
    } /* fail! */
    value = skip(parse_value(child, skip(value + 1))); /* skip any spacing, get the value. */
    if (!value)
        return 0;

    while (*value == ',') {
        mybot_json_t *new_item;
        new_item = mybot_json_new_item();
        if (!new_item)
            return 0; /* memory fail */
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_string(child, skip(value + 1)));
        if (!value)
            return 0;
        child->string = child->valuestring;
        child->valuestring = 0;
        if (*value != ':') {
            return 0;
        } /* fail! */
        value = skip(parse_value(child, skip(value + 1))); /* skip any spacing, get the value. */
        if (!value)
            return 0;
    }

    if (*value == '}')
        return value + 1; /* end of array */
    return 0;             /* malformed. */
}

/* Render an object to text. */
static char *print_object(mybot_json_t *item) {
    char **entries = 0, **names = 0;
    char *out = 0, *ptr, *ret, *str;
    int len = 7, i = 0;
    mybot_json_t *child = item->child;
    int numentries = 0, fail = 0;
    /* Count the number of entries. */
    while (child)
        numentries++, child = child->next;
    /* Explicitly handle empty object case */
    if (!numentries) {
        out = (char *)mybot_json_malloc_fn(3);
        if (!out)
            return 0;
        ptr = out;
        *ptr++ = '{';
        *ptr++ = '}';
        *ptr++ = 0;
        return out;
    }
    /* Allocate space for the names and the objects */
    entries = (char **)mybot_json_malloc_fn(numentries * sizeof(char *));
    if (!entries)
        return 0;
    names = (char **)mybot_json_malloc_fn(numentries * sizeof(char *));
    if (!names) {
        mybot_json_free_fn(entries);
        return 0;
    }
    memset(entries, 0, sizeof(char *) * numentries);
    memset(names, 0, sizeof(char *) * numentries);

    /* Collect all the results into our arrays: */
    child = item->child;
    while (child) {
        names[i] = str = print_string_ptr(child->string);
        entries[i++] = ret = print_value(child);
        if (str && ret)
            len += strlen(ret) + strlen(str) + 2;
        else
            fail = 1;
        child = child->next;
    }

    /* Try to allocate the output string */
    if (!fail)
        out = (char *)mybot_json_malloc_fn(len);
    if (!out)
        fail = 1;

    /* Handle failure */
    if (fail) {
        for (i = 0; i < numentries; i++) {
            if (names[i])
                mybot_json_free_fn(names[i]);
            if (entries[i])
                mybot_json_free_fn(entries[i]);
        }
        mybot_json_free_fn(names);
        mybot_json_free_fn(entries);
        return 0;
    }

    /* Compose the output: */
    *out = '{';
    ptr = out + 1;
    *ptr = 0;
    for (i = 0; i < numentries; i++) {
        size_t nlen = strlen(names[i]);
        memcpy(ptr, names[i], nlen + 1);
        ptr += nlen;
        *ptr++ = ':';
        size_t elen = strlen(entries[i]);
        memcpy(ptr, entries[i], elen + 1);
        ptr += elen;
        if (i != numentries - 1)
            *ptr++ = ',';
        *ptr = 0;
        mybot_json_free_fn(names[i]);
        mybot_json_free_fn(entries[i]);
    }

    mybot_json_free_fn(names);
    mybot_json_free_fn(entries);
    *ptr++ = '}';
    *ptr++ = 0;
    return out;
}

mybot_json_t *mybot_json_get_object_item(const mybot_json_t *object, const char *string) {
    if (!object || !string)
        return NULL;

    mybot_json_t *c = object->child;
    while (c && mybot_json_strcasecmp(c->string, string))
        c = c->next;
    return c;
}

const char *mybot_json_get_string(const mybot_json_t *value) {
    if (!value || value->type != MYBOT_JSON_STRING)
        return NULL;
    return value->valuestring;
}

bool mybot_json_get_integer(const mybot_json_t *value, int64_t *result) {
    if (!value || !result || value->type != MYBOT_JSON_NUMBER)
        return false;
    *result = (int64_t)value->valueint;
    return true;
}

/* Utility for array list handling. */
static void suffix_object(mybot_json_t *prev, mybot_json_t *item) {
    prev->next = item;
    item->prev = prev;
}
/* Add an owned item to an object. */
static int add_item_to_object(mybot_json_t *object, const char *string, mybot_json_t *item) {
    if (!object || !string || !item)
        return -1;

    char *item_name = mybot_json_strdup(string);
    if (!item_name)
        return -1;

    if (item->string)
        mybot_json_free_fn(item->string);
    item->string = item_name;
    mybot_json_t *child = object->child;
    if (!child) {
        object->child = item;
    } else {
        while (child->next)
            child = child->next;
        suffix_object(child, item);
    }
    return 0;
}

static int add_created_item(mybot_json_t *object, const char *name, mybot_json_t *item) {
    if (!object || !name || !item) {
        mybot_json_delete(item);
        return -1;
    }

    int result = add_item_to_object(object, name, item);
    if (result < 0)
        mybot_json_delete(item);
    return result;
}

int mybot_json_add_string(mybot_json_t *object, const char *name, const char *value) {
    if (!value)
        return -1;
    return add_created_item(object, name, mybot_json_create_string(value));
}

int mybot_json_add_number(mybot_json_t *object, const char *name, double value) {
    return add_created_item(object, name, mybot_json_create_number(value));
}

int mybot_json_add_bool(mybot_json_t *object, const char *name, bool value) {
    return add_created_item(object, name, mybot_json_create_bool(value));
}

int mybot_json_add_item(mybot_json_t *object, const char *name, mybot_json_t *item) {
    if (!object || !name || !item)
        return -1;

    return add_item_to_object(object, name, item);
}

/* Create basic types: */
mybot_json_t *mybot_json_create_bool(int b) {
    mybot_json_t *item = mybot_json_new_item();
    if (item)
        item->type = b ? MYBOT_JSON_TRUE : MYBOT_JSON_FALSE;
    return item;
}
mybot_json_t *mybot_json_create_number(double num) {
    mybot_json_t *item = mybot_json_new_item();
    if (item) {
        item->type = MYBOT_JSON_NUMBER;
        item->valuedouble = num;
        item->valueint = (long long)num;
    }
    return item;
}
mybot_json_t *mybot_json_create_string(const char *string) {
    if (!string)
        return NULL;

    mybot_json_t *item = mybot_json_new_item();
    if (item) {
        item->type = MYBOT_JSON_STRING;
        item->valuestring = mybot_json_strdup(string);
        if (!item->valuestring) {
            mybot_json_delete(item);
            return NULL;
        }
    }
    return item;
}
mybot_json_t *mybot_json_create_object(void) {
    mybot_json_t *item = mybot_json_new_item();
    if (item)
        item->type = MYBOT_JSON_OBJECT;
    return item;
}

void mybot_json_free_string(char *text) {
    if (text)
        mybot_json_free_fn(text);
}
