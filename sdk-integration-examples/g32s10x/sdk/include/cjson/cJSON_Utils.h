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

#ifndef tgJSON_Utils__h
#define tgJSON_Utils__h

#ifdef __cplusplus
extern "C"
{
#endif

#include "tgJSON.h"

/* Implement RFC6901 (https://tools.ietf.org/html/rfc6901) JSON Pointer spec. */
CJSON_PUBLIC(tgJSON *) tgJSONUtils_GetPointer(tgJSON * const object, const char *pointer);
CJSON_PUBLIC(tgJSON *) tgJSONUtils_GetPointerCaseSensitive(tgJSON * const object, const char *pointer);

/* Implement RFC6902 (https://tools.ietf.org/html/rfc6902) JSON Patch spec. */
/* NOTE: This modifies objects in 'from' and 'to' by sorting the elements by their key */
CJSON_PUBLIC(tgJSON *) tgJSONUtils_GeneratePatches(tgJSON * const from, tgJSON * const to);
CJSON_PUBLIC(tgJSON *) tgJSONUtils_GeneratePatchesCaseSensitive(tgJSON * const from, tgJSON * const to);
/* Utility for generating patch array entries. */
CJSON_PUBLIC(void) tgJSONUtils_AddPatchToArray(tgJSON * const array, const char * const operation, const char * const path, const tgJSON * const value);
/* Returns 0 for success. */
CJSON_PUBLIC(int) tgJSONUtils_ApplyPatches(tgJSON * const object, const tgJSON * const patches);
CJSON_PUBLIC(int) tgJSONUtils_ApplyPatchesCaseSensitive(tgJSON * const object, const tgJSON * const patches);

/*
// Note that ApplyPatches is NOT atomic on failure. To implement an atomic ApplyPatches, use:
//int tgJSONUtils_AtomicApplyPatches(tgJSON **object, tgJSON *patches)
//{
//    tgJSON *modme = tgJSON_Duplicate(*object, 1);
//    int error = tgJSONUtils_ApplyPatches(modme, patches);
//    if (!error)
//    {
//        tgJSON_Delete(*object);
//        *object = modme;
//    }
//    else
//    {
//        tgJSON_Delete(modme);
//    }
//
//    return error;
//}
// Code not added to library since this strategy is a LOT slower.
*/

/* Implement RFC7386 (https://tools.ietf.org/html/rfc7396) JSON Merge Patch spec. */
/* target will be modified by patch. return value is new ptr for target. */
CJSON_PUBLIC(tgJSON *) tgJSONUtils_MergePatch(tgJSON *target, const tgJSON * const patch);
CJSON_PUBLIC(tgJSON *) tgJSONUtils_MergePatchCaseSensitive(tgJSON *target, const tgJSON * const patch);
/* generates a patch to move from -> to */
/* NOTE: This modifies objects in 'from' and 'to' by sorting the elements by their key */
CJSON_PUBLIC(tgJSON *) tgJSONUtils_GenerateMergePatch(tgJSON * const from, tgJSON * const to);
CJSON_PUBLIC(tgJSON *) tgJSONUtils_GenerateMergePatchCaseSensitive(tgJSON * const from, tgJSON * const to);

/* Given a root object and a target object, construct a pointer from one to the other. */
CJSON_PUBLIC(char *) tgJSONUtils_FindPointerFromObjectTo(const tgJSON * const object, const tgJSON * const target);

/* Sorts the members of the object into alphabetical order. */
CJSON_PUBLIC(void) tgJSONUtils_SortObject(tgJSON * const object);
CJSON_PUBLIC(void) tgJSONUtils_SortObjectCaseSensitive(tgJSON * const object);

#ifdef __cplusplus
}
#endif

#endif
