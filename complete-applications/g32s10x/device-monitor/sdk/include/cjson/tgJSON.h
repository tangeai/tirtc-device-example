/*
  Copyright (c) 2009-2017 Dave Gamble and tgJSON contributors

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
/* To avoid conflict with other projects using different version of cJSON,
 * we replace the symbols cJSON_ to tgJSON_
 */

#ifndef tgJSON__h
#define tgJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

/* project version */
#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 7

#include <stddef.h>

/* tgJSON Types: */
#define tgJSON_Invalid (0)
#define tgJSON_False  (1 << 0)
#define tgJSON_True   (1 << 1)
#define tgJSON_NULL   (1 << 2)
#define tgJSON_Number (1 << 3)
#define tgJSON_String (1 << 4)
#define tgJSON_Array  (1 << 5)
#define tgJSON_Object (1 << 6)
#define tgJSON_Raw    (1 << 7) /* raw json */

#define tgJSON_IsReference 256
#define tgJSON_StringIsConst 512

/* The tgJSON structure: */
typedef struct tgJSON
{
    /* next/prev allow you to walk array/object chains. Alternatively, use GetArraySize/GetArrayItem/GetObjectItem */
    struct tgJSON *next;
    struct tgJSON *prev;
    /* An array or object item will have a child pointer pointing to a chain of the items in the array/object. */
    struct tgJSON *child;

    /* The type of the item, as above. */
    int type;

    /* The item's string, if type==tgJSON_String  and type == tgJSON_Raw */
    char *valuestring;
    /* writing to valueint is DEPRECATED, use tgJSON_SetNumberValue instead */
    int valueint;
    /* The item's number, if type==tgJSON_Number */
    double valuedouble;

    /* The item's name string, if this item is the child of, or is in the list of subitems of an object. */
    char *string;
} tgJSON;

typedef struct tgJSON_Hooks
{
      void *(*malloc_fn)(size_t sz);
      void (*free_fn)(void *ptr);
} tgJSON_Hooks;

typedef int tgJSON_bool;

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif
#ifdef __WINDOWS__

/* When compiling for windows, we specify a specific calling convention to avoid issues where we are being called from a project with a different default calling convention.  For windows you have 2 define options:

CJSON_HIDE_SYMBOLS - Define this in the case where you don't want to ever dllexport symbols
CJSON_EXPORT_SYMBOLS - Define this on library build when you want to dllexport symbols (default)
CJSON_IMPORT_SYMBOLS - Define this if you want to dllimport symbol

For *nix builds that support visibility attribute, you can define similar behavior by

setting default visibility to hidden by adding
-fvisibility=hidden (for gcc)
or
-xldscope=hidden (for sun cc)
to CFLAGS

then using the CJSON_API_VISIBILITY flag to "export" the same symbols the way CJSON_EXPORT_SYMBOLS does

*/

/* export symbols by default, this is necessary for copy pasting the C and header file */
#if !defined(CJSON_HIDE_SYMBOLS) && !defined(CJSON_IMPORT_SYMBOLS) && !defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_EXPORT_SYMBOLS
#endif

#if defined(CJSON_HIDE_SYMBOLS)
#define CJSON_PUBLIC(type)   type __stdcall
#elif defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllexport) type __stdcall
#elif defined(CJSON_IMPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllimport) type __stdcall
#endif
#else /* !WIN32 */
#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(CJSON_API_VISIBILITY)
#define CJSON_PUBLIC(type)   __attribute__((visibility("default"))) type
#else
#define CJSON_PUBLIC(type) type
#endif
#endif

/* Limits how deeply nested arrays/objects can be before tgJSON rejects to parse them.
 * This is to prevent stack overflows. */
#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

/* returns the version of tgJSON as a string */
CJSON_PUBLIC(const char*) tgJSON_Version(void);

/* Supply malloc, realloc and free functions to tgJSON */
CJSON_PUBLIC(void) tgJSON_InitHooks(tgJSON_Hooks* hooks);

/* Memory Management: the caller is always responsible to free the results from all variants of tgJSON_Parse (with tgJSON_Delete) and tgJSON_Print (with stdlib free, tgJSON_Hooks.free_fn, or tgJSON_free as appropriate). The exception is tgJSON_PrintPreallocated, where the caller has full responsibility of the buffer. */
/* Supply a block of JSON, and this returns a tgJSON object you can interrogate. */
CJSON_PUBLIC(tgJSON *) tgJSON_Parse(const char *value);
CJSON_PUBLIC(tgJSON *) tgJSON_ParseWithLength(const char *value, size_t buffer_length);
/* ParseWithOpts allows you to require (and check) that the JSON is null terminated, and to retrieve the pointer to the final byte parsed. */
/* If you supply a ptr in return_parse_end and parsing fails, then return_parse_end will contain a pointer to the error so will match tgJSON_GetErrorPtr(). */
CJSON_PUBLIC(tgJSON *) tgJSON_ParseWithOpts(const char *value, const char **return_parse_end, tgJSON_bool require_null_terminated);
CJSON_PUBLIC(tgJSON *) tgJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, tgJSON_bool require_null_terminated);

/* Render a tgJSON entity to text for transfer/storage. */
CJSON_PUBLIC(char *) tgJSON_Print(const tgJSON *item);
/* Render a tgJSON entity to text for transfer/storage without any formatting. */
CJSON_PUBLIC(char *) tgJSON_PrintUnformatted(const tgJSON *item);
/* Render a tgJSON entity to text using a buffered strategy. prebuffer is a guess at the final size. guessing well reduces reallocation. fmt=0 gives unformatted, =1 gives formatted */
CJSON_PUBLIC(char *) tgJSON_PrintBuffered(const tgJSON *item, int prebuffer, tgJSON_bool fmt);
/* Render a tgJSON entity to text using a buffer already allocated in memory with given length. Returns 1 on success and 0 on failure. */
/* NOTE: tgJSON is not always 100% accurate in estimating how much memory it will use, so to be safe allocate 5 bytes more than you actually need */
CJSON_PUBLIC(tgJSON_bool) tgJSON_PrintPreallocated(tgJSON *item, char *buffer, const int length, const tgJSON_bool format);
/* Delete a tgJSON entity and all subentities. */
CJSON_PUBLIC(void) tgJSON_Delete(tgJSON *c);

/* Returns the number of items in an array (or object). */
CJSON_PUBLIC(int) tgJSON_GetArraySize(const tgJSON *array);
/* Retrieve item number "index" from array "array". Returns NULL if unsuccessful. */
CJSON_PUBLIC(tgJSON *) tgJSON_GetArrayItem(const tgJSON *array, int index);
/* Get item "string" from object. Case insensitive. */
CJSON_PUBLIC(tgJSON *) tgJSON_GetObjectItem(const tgJSON * const object, const char * const string);
CJSON_PUBLIC(tgJSON *) tgJSON_GetObjectItemCaseSensitive(const tgJSON * const object, const char * const string);
CJSON_PUBLIC(tgJSON_bool) tgJSON_HasObjectItem(const tgJSON *object, const char *string);
/* For analysing failed parses. This returns a pointer to the parse error. You'll probably need to look a few chars back to make sense of it. Defined when tgJSON_Parse() returns 0. 0 when tgJSON_Parse() succeeds. */
CJSON_PUBLIC(const char *) tgJSON_GetErrorPtr(void);

/* Check if the item is a string and return its valuestring */
CJSON_PUBLIC(char *) tgJSON_GetStringValue(tgJSON *item);

/* These functions check the type of an item */
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsInvalid(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsFalse(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsTrue(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsBool(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsNull(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsNumber(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsString(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsArray(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsObject(const tgJSON * const item);
CJSON_PUBLIC(tgJSON_bool) tgJSON_IsRaw(const tgJSON * const item);

/* These calls create a tgJSON item of the appropriate type. */
CJSON_PUBLIC(tgJSON *) tgJSON_CreateNull(void);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateTrue(void);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateFalse(void);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateBool(tgJSON_bool boolean);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateNumber(double num);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateInteger(int num);  //ec71x 不支持double运算
CJSON_PUBLIC(tgJSON *) tgJSON_CreateString(const char *string);
/* raw json */
CJSON_PUBLIC(tgJSON *) tgJSON_CreateRaw(const char *raw);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateArray(void);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateObject(void);

/* Create a string where valuestring references a string so
 * it will not be freed by tgJSON_Delete */
CJSON_PUBLIC(tgJSON *) tgJSON_CreateStringReference(const char *string);
/* Create an object/arrray that only references it's elements so
 * they will not be freed by tgJSON_Delete */
CJSON_PUBLIC(tgJSON *) tgJSON_CreateObjectReference(const tgJSON *child);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateArrayReference(const tgJSON *child);

/* These utilities create an Array of count items. */
CJSON_PUBLIC(tgJSON *) tgJSON_CreateIntArray(const int *numbers, int count);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateFloatArray(const float *numbers, int count);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateDoubleArray(const double *numbers, int count);
CJSON_PUBLIC(tgJSON *) tgJSON_CreateStringArray(const char **strings, int count);

/* Append item to the specified array/object. */
CJSON_PUBLIC(void) tgJSON_AddItemToArray(tgJSON *array, tgJSON *item);
CJSON_PUBLIC(void) tgJSON_AddItemToObject(tgJSON *object, const char *string, tgJSON *item);
/* Use this when string is definitely const (i.e. a literal, or as good as), and will definitely survive the tgJSON object.
 * WARNING: When this function was used, make sure to always check that (item->type & tgJSON_StringIsConst) is zero before
 * writing to `item->string` */
CJSON_PUBLIC(void) tgJSON_AddItemToObjectCS(tgJSON *object, const char *string, tgJSON *item);
/* Append reference to item to the specified array/object. Use this when you want to add an existing tgJSON to a new tgJSON, but don't want to corrupt your existing tgJSON. */
CJSON_PUBLIC(void) tgJSON_AddItemReferenceToArray(tgJSON *array, tgJSON *item);
CJSON_PUBLIC(void) tgJSON_AddItemReferenceToObject(tgJSON *object, const char *string, tgJSON *item);

/* Remove/Detatch items from Arrays/Objects. */
CJSON_PUBLIC(tgJSON *) tgJSON_DetachItemViaPointer(tgJSON *parent, tgJSON * const item);
CJSON_PUBLIC(tgJSON *) tgJSON_DetachItemFromArray(tgJSON *array, int which);
CJSON_PUBLIC(void) tgJSON_DeleteItemFromArray(tgJSON *array, int which);
CJSON_PUBLIC(tgJSON *) tgJSON_DetachItemFromObject(tgJSON *object, const char *string);
CJSON_PUBLIC(tgJSON *) tgJSON_DetachItemFromObjectCaseSensitive(tgJSON *object, const char *string);
CJSON_PUBLIC(void) tgJSON_DeleteItemFromObject(tgJSON *object, const char *string);
CJSON_PUBLIC(void) tgJSON_DeleteItemFromObjectCaseSensitive(tgJSON *object, const char *string);

/* Update array items. */
CJSON_PUBLIC(void) tgJSON_InsertItemInArray(tgJSON *array, int which, tgJSON *newitem); /* Shifts pre-existing items to the right. */
CJSON_PUBLIC(tgJSON_bool) tgJSON_ReplaceItemViaPointer(tgJSON * const parent, tgJSON * const item, tgJSON * replacement);
CJSON_PUBLIC(void) tgJSON_ReplaceItemInArray(tgJSON *array, int which, tgJSON *newitem);
CJSON_PUBLIC(void) tgJSON_ReplaceItemInObject(tgJSON *object,const char *string,tgJSON *newitem);
CJSON_PUBLIC(void) tgJSON_ReplaceItemInObjectCaseSensitive(tgJSON *object,const char *string,tgJSON *newitem);

/* Duplicate a tgJSON item */
CJSON_PUBLIC(tgJSON *) tgJSON_Duplicate(const tgJSON *item, tgJSON_bool recurse);
/* Duplicate will create a new, identical tgJSON item to the one you pass, in new memory that will
need to be released. With recurse!=0, it will duplicate any children connected to the item.
The item->next and ->prev pointers are always zero on return from Duplicate. */
/* Recursively compare two tgJSON items for equality. If either a or b is NULL or invalid, they will be considered unequal.
 * case_sensitive determines if object keys are treated case sensitive (1) or case insensitive (0) */
CJSON_PUBLIC(tgJSON_bool) tgJSON_Compare(const tgJSON * const a, const tgJSON * const b, const tgJSON_bool case_sensitive);


CJSON_PUBLIC(void) tgJSON_Minify(char *json);

/* Helper functions for creating and adding items to an object at the same time.
 * They return the added item or NULL on failure. */
CJSON_PUBLIC(tgJSON*) tgJSON_AddNullToObject(tgJSON * const object, const char * const name);
CJSON_PUBLIC(tgJSON*) tgJSON_AddTrueToObject(tgJSON * const object, const char * const name);
CJSON_PUBLIC(tgJSON*) tgJSON_AddFalseToObject(tgJSON * const object, const char * const name);
CJSON_PUBLIC(tgJSON*) tgJSON_AddBoolToObject(tgJSON * const object, const char * const name, const tgJSON_bool boolean);
CJSON_PUBLIC(tgJSON*) tgJSON_AddNumberToObject(tgJSON * const object, const char * const name, const double number);
CJSON_PUBLIC(tgJSON*) tgJSON_AddIntegerToObject(tgJSON * const object, const char * const name, const int number);
CJSON_PUBLIC(tgJSON*) tgJSON_AddStringToObject(tgJSON * const object, const char * const name, const char * const string);
CJSON_PUBLIC(tgJSON*) tgJSON_AddRawToObject(tgJSON * const object, const char * const name, const char * const raw);
CJSON_PUBLIC(tgJSON*) tgJSON_AddObjectToObject(tgJSON * const object, const char * const name);
CJSON_PUBLIC(tgJSON*) tgJSON_AddArrayToObject(tgJSON * const object, const char * const name);

/* When assigning an integer value, it needs to be propagated to valuedouble too. */
#define tgJSON_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/* helper for the tgJSON_SetNumberValue macro */
CJSON_PUBLIC(double) tgJSON_SetNumberHelper(tgJSON *object, double number);
#define tgJSON_SetNumberValue(object, number) ((object != NULL) ? tgJSON_SetNumberHelper(object, (double)number) : (number))

/* Macro for iterating over an array or object */
#define tgJSON_ArrayForEach(element, array) for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

/* malloc/free objects using the malloc/free functions that have been set with tgJSON_InitHooks */
CJSON_PUBLIC(void *) tgJSON_malloc(size_t size);
CJSON_PUBLIC(void) tgJSON_free(void *object);

#ifdef __cplusplus
}
#endif

#endif
