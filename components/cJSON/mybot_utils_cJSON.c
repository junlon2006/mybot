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

/* mybot_utils_cJSON */
/* JSON parser in C. */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <float.h>
#include <limits.h>
#include <ctype.h>
#include "mybot_utils_cJSON.h"

static const char *ep;

const char *mybot_utils_cJSON_GetErrorPtr(void) {
    return ep;
}

static int mybot_utils_cJSON_strcasecmp(const char *s1, const char *s2) {
    if (!s1)
        return (s1 == s2) ? 0 : 1;
    if (!s2)
        return 1;
    for (; tolower(*s1) == tolower(*s2); ++s1, ++s2)
        if (*s1 == 0)
            return 0;
    return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
}

static void *(*mybot_utils_cJSON_malloc)(size_t sz) = malloc;
static void (*mybot_utils_cJSON_free)(void *ptr) = free;

static char *mybot_utils_cJSON_strdup(const char *str) {
    size_t len;
    char *copy;

    len = strlen(str) + 1;
    if (!(copy = (char *)mybot_utils_cJSON_malloc(len)))
        return 0;
    memcpy(copy, str, len);
    return copy;
}

void mybot_utils_cJSON_InitHooks(mybot_utils_cJSON_Hooks *hooks) {
    if (!hooks) { /* Reset hooks */
        mybot_utils_cJSON_malloc = malloc;
        mybot_utils_cJSON_free = free;
        return;
    }

    mybot_utils_cJSON_malloc = (hooks->malloc_fn) ? hooks->malloc_fn : malloc;
    mybot_utils_cJSON_free = (hooks->free_fn) ? hooks->free_fn : free;
}

/* Internal constructor. */
static mybot_utils_cJSON *mybot_utils_cJSON_New_Item(void) {
    mybot_utils_cJSON *node =
        (mybot_utils_cJSON *)mybot_utils_cJSON_malloc(sizeof(mybot_utils_cJSON));
    if (node)
        memset(node, 0, sizeof(mybot_utils_cJSON));
    return node;
}

/* Delete a mybot_utils_cJSON structure. */
void mybot_utils_cJSON_Delete(mybot_utils_cJSON *c) {
    mybot_utils_cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & mybot_utils_cJSON_IsReference) && c->child)
            mybot_utils_cJSON_Delete(c->child);
        if (!(c->type & mybot_utils_cJSON_IsReference) && c->valuestring)
            mybot_utils_cJSON_free(c->valuestring);
        if (c->string)
            mybot_utils_cJSON_free(c->string);
        mybot_utils_cJSON_free(c);
        c = next;
    }
}

#if 0
/* Parse the input text to generate a number, and populate the result into item. */
static const char *parse_number(mybot_utils_cJSON *item,const char *num)
{
  double n=0,sign=1,scale=0;int subscale=0,signsubscale=1;

  if (*num=='-') {
    sign = -1;
    num++;	/* Has sign? */
  }
  if (*num=='0') {
    num++;			/* is zero */
  }
  if (*num >= '1' && *num <= '9')	{
    do {
      n = (n * 10.0) + (*num -'0');
      num++;
    } while (*num>='0' && *num<='9');	/* Number? */
  }

  if (*num=='.' && num[1]>='0' && num[1]<='9') {
    num++;
    do {
      n = (n * 10.0) + (*num -'0');
      scale --;
      num ++;
    } while (*num>='0' && *num<='9');
  }	/* Fractional part? */

  if (*num=='e' || *num=='E') /* Exponent? */ {
    num++;
    if (*num=='+') {
      num++;
    }
    else if (*num=='-') {
      signsubscale = -1;
      num++;		/* With sign? */
    }
    while (*num>='0' && *num<='9') {
      subscale = (subscale * 10) + (*num - '0');	/* Number? */
      num++;
    }
  }

  n = sign*n*pow(10.0,(scale+subscale*signsubscale));	/* number = +/- number.fraction * 10^+/- exponent */

  item->valuedouble = n;
  item->valueint    = (int)n;
  item->type        = mybot_utils_cJSON_Number;
  return num;
}
#endif

static const char *parse_number_u64(mybot_utils_cJSON *item, const char *num) {
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

    item->type = mybot_utils_cJSON_Number;
    return num;
}

/* Render the number nicely from the given item into a string. */
static char *print_number(mybot_utils_cJSON *item) {
    char *str;
    double d = item->valuedouble;
    if (fabs(((double)item->valueint) - d) <= DBL_EPSILON && d <= INT_MAX && d >= INT_MIN) {
        str = (char *)mybot_utils_cJSON_malloc(21); /* 2^64+1 can be represented in 21 chars. */
        if (str)
            sprintf(str, "%lld", item->valueint);
    } else {
        str = (char *)mybot_utils_cJSON_malloc(64); /* This is a nice tradeoff. */
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
static const char *parse_string(mybot_utils_cJSON *item, const char *str) {
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    int len = 0;
    unsigned uc, uc2;
    if (*str != '\"') {
        ep = str;
        return 0;
    } /* not a string! */

    while (*ptr != '\"' && *ptr && ++len)
        if (*ptr++ == '\\')
            ptr++; /* Skip escaped quotes. */

    out = (char *)mybot_utils_cJSON_malloc(
        len + 1); /* This is how long we need for the string, roughly. */
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
            case 'u': /* transcode utf16 to utf8. */
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
                case 4:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                case 3:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                case 2:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                case 1:
                    *--ptr2 = (uc | firstByteMark[len]);
                }
                ptr2 += len;
                break;
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
    item->type = mybot_utils_cJSON_String;
    return ptr;
}

/* Render the cstring provided to an escaped version that can be printed. */
static char *print_string_ptr(const char *str) {
    const char *ptr;
    char *ptr2, *out;
    int len = 0;
    unsigned char token;

    if (!str)
        return mybot_utils_cJSON_strdup("");
    ptr = str;
    while ((token = *ptr) && ++len) {
        if (strchr("\"\\\b\f\n\r\t", token))
            len++;
        else if (token < 32)
            len += 5;
        ptr++;
    }

    out = (char *)mybot_utils_cJSON_malloc(len + 3);
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
static char *print_string(mybot_utils_cJSON *item) {
    return print_string_ptr(item->valuestring);
}

/* Predeclare these prototypes. */
static const char *parse_value(mybot_utils_cJSON *item, const char *value);
static char *print_value(mybot_utils_cJSON *item, int depth, int fmt);
static const char *parse_array(mybot_utils_cJSON *item, const char *value);
static char *print_array(mybot_utils_cJSON *item, int depth, int fmt);
static const char *parse_object(mybot_utils_cJSON *item, const char *value);
static char *print_object(mybot_utils_cJSON *item, int depth, int fmt);

/* Utility to jump whitespace and cr/lf */
static const char *skip(const char *in) {
    while (in && *in && (unsigned char)*in <= 32)
        in++;
    return in;
}

/* Parse an object - create a new root, and populate. */
mybot_utils_cJSON *mybot_utils_cJSON_ParseWithOpts(const char *value, const char **return_parse_end,
                                                   int require_null_terminated) {
    const char *end = 0;
    mybot_utils_cJSON *c = mybot_utils_cJSON_New_Item();
    ep = 0;
    if (!c)
        return 0; /* memory fail */

    end = parse_value(c, skip(value));
    if (!end) {
        mybot_utils_cJSON_Delete(c);
        return 0;
    } /* parse failure. ep is set. */

    /* if we require null-terminated JSON without appended garbage, skip and then check for a null
     * terminator */
    if (require_null_terminated) {
        end = skip(end);
        if (*end) {
            mybot_utils_cJSON_Delete(c);
            ep = end;
            return 0;
        }
    }
    if (return_parse_end)
        *return_parse_end = end;
    return c;
}
/* Default options for mybot_utils_cJSON_Parse */
mybot_utils_cJSON *mybot_utils_cJSON_Parse(const char *value) {
    return mybot_utils_cJSON_ParseWithOpts(value, 0, 0);
}

/* Render a mybot_utils_cJSON item/entity/structure to text. */
char *mybot_utils_cJSON_Print(mybot_utils_cJSON *item) {
    return print_value(item, 0, 1);
}
char *mybot_utils_cJSON_PrintUnformatted(mybot_utils_cJSON *item) {
    return print_value(item, 0, 0);
}

/* Parser core - when encountering text, process appropriately. */
static const char *parse_value(mybot_utils_cJSON *item, const char *value) {
    if (!value) {
        return 0; /* Fail on null. */
    }
    if (!strncmp(value, "null", 4)) {
        item->type = mybot_utils_cJSON_NULL;
        return value + 4;
    }
    if (!strncmp(value, "false", 5)) {
        item->type = mybot_utils_cJSON_False;
        return value + 5;
    }
    if (!strncmp(value, "true", 4)) {
        item->type = mybot_utils_cJSON_True;
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

    ep = value;
    return 0; /* failure. */
}

/* Render a value to text. */
static char *print_value(mybot_utils_cJSON *item, int depth, int fmt) {
    char *out = 0;
    if (!item)
        return 0;
    switch ((item->type) & 255) {
    case mybot_utils_cJSON_NULL:
        out = mybot_utils_cJSON_strdup("null");
        break;
    case mybot_utils_cJSON_False:
        out = mybot_utils_cJSON_strdup("false");
        break;
    case mybot_utils_cJSON_True:
        out = mybot_utils_cJSON_strdup("true");
        break;
    case mybot_utils_cJSON_Number:
        out = print_number(item);
        break;
    case mybot_utils_cJSON_String:
        out = print_string(item);
        break;
    case mybot_utils_cJSON_Array:
        out = print_array(item, depth, fmt);
        break;
    case mybot_utils_cJSON_Object:
        out = print_object(item, depth, fmt);
        break;
    }
    return out;
}

/* Build an array from input text. */
static const char *parse_array(mybot_utils_cJSON *item, const char *value) {
    mybot_utils_cJSON *child;
    if (*value != '[') {
        ep = value;
        return 0;
    } /* not an array! */

    item->type = mybot_utils_cJSON_Array;
    value = skip(value + 1);
    if (*value == ']')
        return value + 1; /* empty array. */

    item->child = child = mybot_utils_cJSON_New_Item();
    if (!item->child)
        return 0;                                  /* memory fail */
    value = skip(parse_value(child, skip(value))); /* skip any spacing, get the value. */
    if (!value)
        return 0;

    while (*value == ',') {
        mybot_utils_cJSON *new_item;
        if (!(new_item = mybot_utils_cJSON_New_Item()))
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
    ep = value;
    return 0; /* malformed. */
}

/* Render an array to text */
static char *print_array(mybot_utils_cJSON *item, int depth, int fmt) {
    char **entries;
    char *out = 0, *ptr, *ret;
    int len = 5;
    mybot_utils_cJSON *child = item->child;
    int numentries = 0, i = 0, fail = 0;

    /* How many entries in the array? */
    while (child)
        numentries++, child = child->next;
    /* Explicitly handle numentries==0 */
    if (!numentries) {
        out = (char *)mybot_utils_cJSON_malloc(3);
        if (out)
            strcpy(out, "[]");
        return out;
    }
    /* Allocate an array to hold the values for each */
    entries = (char **)mybot_utils_cJSON_malloc(numentries * sizeof(char *));
    if (!entries)
        return 0;
    memset(entries, 0, numentries * sizeof(char *));
    /* Retrieve all the results: */
    child = item->child;
    while (child && !fail) {
        ret = print_value(child, depth + 1, fmt);
        entries[i++] = ret;
        if (ret)
            len += strlen(ret) + 2 + (fmt ? 1 : 0);
        else
            fail = 1;
        child = child->next;
    }

    /* If we didn't fail, try to malloc the output string */
    if (!fail)
        out = (char *)mybot_utils_cJSON_malloc(len);
    /* If that fails, we fail. */
    if (!out)
        fail = 1;

    /* Handle failure. */
    if (fail) {
        for (i = 0; i < numentries; i++)
            if (entries[i])
                mybot_utils_cJSON_free(entries[i]);
        mybot_utils_cJSON_free(entries);
        return 0;
    }

    /* Compose the output array. */
    *out = '[';
    ptr = out + 1;
    *ptr = 0;
    for (i = 0; i < numentries; i++) {
        strcpy(ptr, entries[i]);
        ptr += strlen(entries[i]);
        if (i != numentries - 1) {
            *ptr++ = ',';
            if (fmt)
                *ptr++ = ' ';
            *ptr = 0;
        }
        mybot_utils_cJSON_free(entries[i]);
    }
    mybot_utils_cJSON_free(entries);
    *ptr++ = ']';
    *ptr++ = 0;
    return out;
}

/* Build an object from the text. */
static const char *parse_object(mybot_utils_cJSON *item, const char *value) {
    mybot_utils_cJSON *child;
    if (*value != '{') {
        ep = value;
        return 0;
    } /* not an object! */

    item->type = mybot_utils_cJSON_Object;
    value = skip(value + 1);
    if (*value == '}')
        return value + 1; /* empty array. */

    item->child = child = mybot_utils_cJSON_New_Item();
    if (!item->child)
        return 0;
    value = skip(parse_string(child, skip(value)));
    if (!value)
        return 0;
    child->string = child->valuestring;
    child->valuestring = 0;
    if (*value != ':') {
        ep = value;
        return 0;
    }                                                  /* fail! */
    value = skip(parse_value(child, skip(value + 1))); /* skip any spacing, get the value. */
    if (!value)
        return 0;

    while (*value == ',') {
        mybot_utils_cJSON *new_item;
        if (!(new_item = mybot_utils_cJSON_New_Item()))
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
            ep = value;
            return 0;
        }                                                  /* fail! */
        value = skip(parse_value(child, skip(value + 1))); /* skip any spacing, get the value. */
        if (!value)
            return 0;
    }

    if (*value == '}')
        return value + 1; /* end of array */
    ep = value;
    return 0; /* malformed. */
}

/* Render an object to text. */
static char *print_object(mybot_utils_cJSON *item, int depth, int fmt) {
    char **entries = 0, **names = 0;
    char *out = 0, *ptr, *ret, *str;
    int len = 7, i = 0, j;
    mybot_utils_cJSON *child = item->child;
    int numentries = 0, fail = 0;
    /* Count the number of entries. */
    while (child)
        numentries++, child = child->next;
    /* Explicitly handle empty object case */
    if (!numentries) {
        out = (char *)mybot_utils_cJSON_malloc(fmt ? depth + 4 : 3);
        if (!out)
            return 0;
        ptr = out;
        *ptr++ = '{';
        if (fmt) {
            *ptr++ = '\n';
            for (i = 0; i < depth - 1; i++)
                *ptr++ = '\t';
        }
        *ptr++ = '}';
        *ptr++ = 0;
        return out;
    }
    /* Allocate space for the names and the objects */
    entries = (char **)mybot_utils_cJSON_malloc(numentries * sizeof(char *));
    if (!entries)
        return 0;
    names = (char **)mybot_utils_cJSON_malloc(numentries * sizeof(char *));
    if (!names) {
        mybot_utils_cJSON_free(entries);
        return 0;
    }
    memset(entries, 0, sizeof(char *) * numentries);
    memset(names, 0, sizeof(char *) * numentries);

    /* Collect all the results into our arrays: */
    child = item->child;
    depth++;
    if (fmt)
        len += depth;
    while (child) {
        names[i] = str = print_string_ptr(child->string);
        entries[i++] = ret = print_value(child, depth, fmt);
        if (str && ret)
            len += strlen(ret) + strlen(str) + 2 + (fmt ? 2 + depth : 0);
        else
            fail = 1;
        child = child->next;
    }

    /* Try to allocate the output string */
    if (!fail)
        out = (char *)mybot_utils_cJSON_malloc(len);
    if (!out)
        fail = 1;

    /* Handle failure */
    if (fail) {
        for (i = 0; i < numentries; i++) {
            if (names[i])
                mybot_utils_cJSON_free(names[i]);
            if (entries[i])
                mybot_utils_cJSON_free(entries[i]);
        }
        mybot_utils_cJSON_free(names);
        mybot_utils_cJSON_free(entries);
        return 0;
    }

    /* Compose the output: */
    *out = '{';
    ptr = out + 1;
    if (fmt)
        *ptr++ = '\n';
    *ptr = 0;
    for (i = 0; i < numentries; i++) {
        if (fmt)
            for (j = 0; j < depth; j++)
                *ptr++ = '\t';
        strcpy(ptr, names[i]);
        ptr += strlen(names[i]);
        *ptr++ = ':';
        if (fmt)
            *ptr++ = '\t';
        strcpy(ptr, entries[i]);
        ptr += strlen(entries[i]);
        if (i != numentries - 1)
            *ptr++ = ',';
        if (fmt)
            *ptr++ = '\n';
        *ptr = 0;
        mybot_utils_cJSON_free(names[i]);
        mybot_utils_cJSON_free(entries[i]);
    }

    mybot_utils_cJSON_free(names);
    mybot_utils_cJSON_free(entries);
    if (fmt)
        for (i = 0; i < depth - 1; i++)
            *ptr++ = '\t';
    *ptr++ = '}';
    *ptr++ = 0;
    return out;
}

/* Get Array size/item / object item. */
int mybot_utils_cJSON_GetArraySize(mybot_utils_cJSON *array) {
    mybot_utils_cJSON *c = array->child;
    int i = 0;
    while (c)
        i++, c = c->next;
    return i;
}
mybot_utils_cJSON *mybot_utils_cJSON_GetArrayItem(mybot_utils_cJSON *array, int item) {
    mybot_utils_cJSON *c = array->child;
    while (c && item > 0)
        item--, c = c->next;
    return c;
}
mybot_utils_cJSON *mybot_utils_cJSON_GetObjectItem(mybot_utils_cJSON *object, const char *string) {
    mybot_utils_cJSON *c = object->child;
    while (c && mybot_utils_cJSON_strcasecmp(c->string, string))
        c = c->next;
    return c;
}

/* Utility for array list handling. */
static void suffix_object(mybot_utils_cJSON *prev, mybot_utils_cJSON *item) {
    prev->next = item;
    item->prev = prev;
}
/* Utility for handling references. */
static mybot_utils_cJSON *create_reference(mybot_utils_cJSON *item) {
    mybot_utils_cJSON *ref = mybot_utils_cJSON_New_Item();
    if (!ref)
        return 0;
    memcpy(ref, item, sizeof(mybot_utils_cJSON));
    ref->string = 0;
    ref->type |= mybot_utils_cJSON_IsReference;
    ref->next = ref->prev = 0;
    return ref;
}

/* Add item to array/object. */
void mybot_utils_cJSON_AddItemToArray(mybot_utils_cJSON *array, mybot_utils_cJSON *item) {
    mybot_utils_cJSON *c = array->child;
    if (!item)
        return;
    if (!c) {
        array->child = item;
    } else {
        while (c && c->next)
            c = c->next;
        suffix_object(c, item);
    }
}
void mybot_utils_cJSON_AddItemToObject(mybot_utils_cJSON *object, const char *string,
                                       mybot_utils_cJSON *item) {
    if (!item)
        return;
    if (item->string)
        mybot_utils_cJSON_free(item->string);
    item->string = mybot_utils_cJSON_strdup(string);
    mybot_utils_cJSON_AddItemToArray(object, item);
}
void mybot_utils_cJSON_AddItemReferenceToArray(mybot_utils_cJSON *array, mybot_utils_cJSON *item) {
    mybot_utils_cJSON_AddItemToArray(array, create_reference(item));
}
void mybot_utils_cJSON_AddItemReferenceToObject(mybot_utils_cJSON *object, const char *string,
                                                mybot_utils_cJSON *item) {
    mybot_utils_cJSON_AddItemToObject(object, string, create_reference(item));
}

mybot_utils_cJSON *mybot_utils_cJSON_DetachItemFromArray(mybot_utils_cJSON *array, int which) {
    mybot_utils_cJSON *c = array->child;
    while (c && which > 0)
        c = c->next, which--;
    if (!c)
        return 0;
    if (c->prev)
        c->prev->next = c->next;
    if (c->next)
        c->next->prev = c->prev;
    if (c == array->child)
        array->child = c->next;
    c->prev = c->next = 0;
    return c;
}
void mybot_utils_cJSON_DeleteItemFromArray(mybot_utils_cJSON *array, int which) {
    mybot_utils_cJSON_Delete(mybot_utils_cJSON_DetachItemFromArray(array, which));
}
mybot_utils_cJSON *mybot_utils_cJSON_DetachItemFromObject(mybot_utils_cJSON *object,
                                                          const char *string) {
    int i = 0;
    mybot_utils_cJSON *c = object->child;
    while (c && mybot_utils_cJSON_strcasecmp(c->string, string))
        i++, c = c->next;
    if (c)
        return mybot_utils_cJSON_DetachItemFromArray(object, i);
    return 0;
}
void mybot_utils_cJSON_DeleteItemFromObject(mybot_utils_cJSON *object, const char *string) {
    mybot_utils_cJSON_Delete(mybot_utils_cJSON_DetachItemFromObject(object, string));
}

/* Replace array/object items with new ones. */
void mybot_utils_cJSON_ReplaceItemInArray(mybot_utils_cJSON *array, int which,
                                          mybot_utils_cJSON *newitem) {
    mybot_utils_cJSON *c = array->child;
    while (c && which > 0)
        c = c->next, which--;
    if (!c)
        return;
    newitem->next = c->next;
    newitem->prev = c->prev;
    if (newitem->next)
        newitem->next->prev = newitem;
    if (c == array->child)
        array->child = newitem;
    else
        newitem->prev->next = newitem;
    c->next = c->prev = 0;
    mybot_utils_cJSON_Delete(c);
}
void mybot_utils_cJSON_ReplaceItemInObject(mybot_utils_cJSON *object, const char *string,
                                           mybot_utils_cJSON *newitem) {
    int i = 0;
    mybot_utils_cJSON *c = object->child;
    while (c && mybot_utils_cJSON_strcasecmp(c->string, string))
        i++, c = c->next;
    if (c) {
        newitem->string = mybot_utils_cJSON_strdup(string);
        mybot_utils_cJSON_ReplaceItemInArray(object, i, newitem);
    }
}

/* Create basic types: */
mybot_utils_cJSON *mybot_utils_cJSON_CreateNull(void) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item)
        item->type = mybot_utils_cJSON_NULL;
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateTrue(void) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item)
        item->type = mybot_utils_cJSON_True;
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateFalse(void) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item)
        item->type = mybot_utils_cJSON_False;
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateBool(int b) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item)
        item->type = b ? mybot_utils_cJSON_True : mybot_utils_cJSON_False;
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateNumber(double num) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item) {
        item->type = mybot_utils_cJSON_Number;
        item->valuedouble = num;
        item->valueint = (long long)num;
    }
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateString(const char *string) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item) {
        item->type = mybot_utils_cJSON_String;
        item->valuestring = mybot_utils_cJSON_strdup(string);
    }
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateArray(void) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item)
        item->type = mybot_utils_cJSON_Array;
    return item;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateObject(void) {
    mybot_utils_cJSON *item = mybot_utils_cJSON_New_Item();
    if (item)
        item->type = mybot_utils_cJSON_Object;
    return item;
}

/* Create Arrays: */
mybot_utils_cJSON *mybot_utils_cJSON_CreateIntArray(const int *numbers, int count) {
    int i;
    mybot_utils_cJSON *n = 0, *p = 0, *a = mybot_utils_cJSON_CreateArray();
    for (i = 0; a && i < count; i++) {
        n = mybot_utils_cJSON_CreateNumber(numbers[i]);
        if (!i)
            a->child = n;
        else
            suffix_object(p, n);
        p = n;
    }
    return a;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateFloatArray(const float *numbers, int count) {
    int i;
    mybot_utils_cJSON *n = 0, *p = 0, *a = mybot_utils_cJSON_CreateArray();
    for (i = 0; a && i < count; i++) {
        n = mybot_utils_cJSON_CreateNumber(numbers[i]);
        if (!i)
            a->child = n;
        else
            suffix_object(p, n);
        p = n;
    }
    return a;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateDoubleArray(const double *numbers, int count) {
    int i;
    mybot_utils_cJSON *n = 0, *p = 0, *a = mybot_utils_cJSON_CreateArray();
    for (i = 0; a && i < count; i++) {
        n = mybot_utils_cJSON_CreateNumber(numbers[i]);
        if (!i)
            a->child = n;
        else
            suffix_object(p, n);
        p = n;
    }
    return a;
}
mybot_utils_cJSON *mybot_utils_cJSON_CreateStringArray(const char **strings, int count) {
    int i;
    mybot_utils_cJSON *n = 0, *p = 0, *a = mybot_utils_cJSON_CreateArray();
    for (i = 0; a && i < count; i++) {
        n = mybot_utils_cJSON_CreateString(strings[i]);
        if (!i)
            a->child = n;
        else
            suffix_object(p, n);
        p = n;
    }
    return a;
}

/* Duplication */
mybot_utils_cJSON *mybot_utils_cJSON_Duplicate(mybot_utils_cJSON *item, int recurse) {
    mybot_utils_cJSON *newitem, *cptr, *nptr = 0, *newchild;
    /* Bail on bad ptr */
    if (!item)
        return 0;
    /* Create new item */
    newitem = mybot_utils_cJSON_New_Item();
    if (!newitem)
        return 0;
    /* Copy over all vars */
    newitem->type = item->type & (~mybot_utils_cJSON_IsReference),
    newitem->valueint = item->valueint, newitem->valuedouble = item->valuedouble;
    if (item->valuestring) {
        newitem->valuestring = mybot_utils_cJSON_strdup(item->valuestring);
        if (!newitem->valuestring) {
            mybot_utils_cJSON_Delete(newitem);
            return 0;
        }
    }
    if (item->string) {
        newitem->string = mybot_utils_cJSON_strdup(item->string);
        if (!newitem->string) {
            mybot_utils_cJSON_Delete(newitem);
            return 0;
        }
    }
    /* If non-recursive, then we're done! */
    if (!recurse)
        return newitem;
    /* Walk the ->next chain for the child. */
    cptr = item->child;
    while (cptr) {
        newchild = mybot_utils_cJSON_Duplicate(
            cptr, 1); /* Duplicate (with recurse) each item in the ->next chain */
        if (!newchild) {
            mybot_utils_cJSON_Delete(newitem);
            return 0;
        }
        if (nptr) {
            nptr->next = newchild, newchild->prev = nptr;
            nptr = newchild;
        } /* If newitem->child already set, then crosswire ->prev and ->next and move on */
        else {
            newitem->child = newchild;
            nptr = newchild;
        } /* Set newitem->child and move to it */
        cptr = cptr->next;
    }
    return newitem;
}

void mybot_utils_cJSON_Minify(char *json) {
    char *into = json;
    while (*json) {
        if (*json == ' ')
            json++;
        else if (*json == '\t')
            json++; // Whitespace characters.
        else if (*json == '\r')
            json++;
        else if (*json == '\n')
            json++;
        else if (*json == '/' && json[1] == '/')
            while (*json && *json != '\n')
                json++; // double-slash comments, to end of line.
        else if (*json == '/' && json[1] == '*') {
            while (*json && !(*json == '*' && json[1] == '/'))
                json++;
            json += 2;
        } // multiline comments.
        else if (*json == '\"') {
            *into++ = *json++;
            while (*json && *json != '\"') {
                if (*json == '\\')
                    *into++ = *json++;
                *into++ = *json++;
            }
            *into++ = *json++;
        } // string literals, which are \" sensitive.
        else
            *into++ = *json++; // All other characters.
    }
    *into = 0; // and null-terminate.
}